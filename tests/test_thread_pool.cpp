// m13 item 4: unit tests for the persistent worker pool. The pool's
// contract is the determinism backbone of --threads: fixed chunks
// independent of the worker count, exclusive slot ranges, repeated
// execution, dynamic scheduling, exception capture and rethrow, and a
// clean lifecycle. The byte-identity of full battles across thread
// counts is covered by tests/test_threads.cpp (CLI level) and the
// determinism suite.
#include <atomic>
#include <stdexcept>
#include <set>
#include <vector>

#include <gtest/gtest.h>

#include "sim/thread_pool.hpp"

namespace {

// Records every invocation (worker, begin, end).
struct CallLog {
  struct Entry {
    unsigned worker;
    std::size_t begin;
    std::size_t end;
  };
  std::vector<Entry> entries;

  void record(unsigned worker, std::size_t begin, std::size_t end) {
    entries.push_back({worker, begin, end});
  }
};

}  // namespace

TEST(ThreadPoolTest, InlinePoolRunsOnTheCaller) {
  sim::ThreadPool pool(0);
  EXPECT_EQ(pool.worker_count(), 0u);
  sim::ThreadPool one(1);
  EXPECT_EQ(one.worker_count(), 0u);  // 1 also selects inline execution

  CallLog log;
  pool.run_chunked(10, 4, [&](unsigned worker, std::size_t begin,
                              std::size_t end) {
    log.record(worker, begin, end);
    EXPECT_EQ(worker, 0u);
  });
  // Exactly the fixed chunks of [0, 10) with size 4: 4/4/2.
  ASSERT_EQ(log.entries.size(), std::size_t{3});
  EXPECT_EQ(log.entries[0].begin, 0u);
  EXPECT_EQ(log.entries[0].end, 4u);
  EXPECT_EQ(log.entries[1].begin, 4u);
  EXPECT_EQ(log.entries[1].end, 8u);
  EXPECT_EQ(log.entries[2].begin, 8u);
  EXPECT_EQ(log.entries[2].end, 10u);
}

TEST(ThreadPoolTest, ChunksPartitionExactlyWithUnevenTail) {
  // The chunk boundaries depend only on (count, chunk_size) — never on
  // the worker count or scheduling.
  for (unsigned workers : {2u, 3u, 4u, 8u}) {
    sim::ThreadPool pool(workers);
    ASSERT_EQ(pool.worker_count(), workers);
    for (const auto& [count, chunk] :
         std::vector<std::pair<std::size_t, std::size_t>>{
             {0, 4}, {1, 4}, {3, 4}, {4, 4}, {5, 4}, {10, 4}, {100, 7},
             {257, 64}, {1000, 256}, {17, 0 /* -> single chunk */}}) {
      std::vector<unsigned> writes(count, 0u);
      std::atomic<std::size_t> invocations{0};
      pool.run_chunked(count, chunk,
                       [&](unsigned worker, std::size_t begin,
                           std::size_t end) {
                         ++invocations;
                         EXPECT_LT(worker, workers);
                         EXPECT_LT(begin, end);
                         EXPECT_LE(end, count);
                         for (std::size_t i = begin; i < end; ++i) {
                           writes[i] += 1;  // each slot written exactly once
                         }
                       });
      const std::size_t expected_chunks =
          count == 0 ? 0 : (chunk == 0 || chunk >= count
                                ? 1
                                : (count + chunk - 1) / chunk);
      EXPECT_EQ(invocations.load(), expected_chunks)
          << "workers=" << workers << " count=" << count;
      for (std::size_t i = 0; i < count; ++i) {
        ASSERT_EQ(writes[i], 1u)
            << "slot " << i << " written " << writes[i] << " times"
            << " (workers=" << workers << ", count=" << count << ")";
      }
    }
  }
}

TEST(ThreadPoolTest, SchedulingIndependence) {
  // The same computation over the same chunks gives the same result for
  // every worker count (dynamic scheduling only reorders disjoint
  // chunks).
  std::vector<long long> reference;
  for (unsigned workers : {0u, 2u, 3u, 4u, 8u}) {
    sim::ThreadPool pool(workers);
    std::vector<long long> squares(500);
    pool.run_chunked(500, 32, [&](unsigned, std::size_t begin,
                                  std::size_t end) {
      for (std::size_t i = begin; i < end; ++i) {
        squares[i] = static_cast<long long>(i) * static_cast<long long>(i);
      }
    });
    if (reference.empty()) {
      reference = squares;
    } else {
      ASSERT_EQ(squares, reference) << "workers=" << workers;
    }
  }
}

TEST(ThreadPoolTest, RepeatedExecutionOnOnePool) {
  // The pool is persistent: many sequential dispatches, results stay
  // correct and no state leaks between them.
  sim::ThreadPool pool(3);
  std::vector<int> data(512, 0);
  for (int round = 1; round <= 32; ++round) {
    pool.run_chunked(data.size(), 64, [&](unsigned, std::size_t begin,
                                          std::size_t end) {
      for (std::size_t i = begin; i < end; ++i) {
        data[i] += 1;  // every round increments every slot exactly once
      }
    });
  }
  for (int value : data) {
    ASSERT_EQ(value, 32);
  }
}

TEST(ThreadPoolTest, WorkerIndicesAreStable) {
  // Each chunk sees one of the pool's worker indices — the scratch
  // buffers are indexed by it, so the values must be < worker_count.
  sim::ThreadPool pool(4);
  std::atomic<unsigned> max_seen{0};
  std::atomic<bool> any{false};
  pool.run_chunked(10000, 64, [&](unsigned worker, std::size_t, std::size_t) {
    any.store(true);
    unsigned current = max_seen.load();
    while (worker > current &&
           !max_seen.compare_exchange_weak(current, worker)) {
    }
  });
  EXPECT_TRUE(any.load());
  EXPECT_LT(max_seen.load(), 4u);
}

TEST(ThreadPoolTest, ExceptionIsCapturedAndRethrown) {
  sim::ThreadPool pool(3);
  std::atomic<std::size_t> completed{0};
  bool threw = false;
  try {
    pool.run_chunked(200, 16, [&](unsigned, std::size_t begin,
                                 std::size_t end) {
      for (std::size_t i = begin; i < end; ++i) {
        if (i == 100) {
          throw std::runtime_error("chunk failed");
        }
        ++completed;
      }
    });
  } catch (const std::runtime_error& error) {
    threw = true;
    EXPECT_STREQ(error.what(), "chunk failed");
  }
  EXPECT_TRUE(threw);
  // The pool stays usable after the failure.
  std::vector<int> data(64, 0);
  pool.run_chunked(64, 8, [&](unsigned, std::size_t begin, std::size_t end) {
    for (std::size_t i = begin; i < end; ++i) {
      data[i] = 1;
    }
  });
  for (int value : data) {
    ASSERT_EQ(value, 1);
  }
}

TEST(ThreadPoolTest, AllChunksFinishAfterAThrow) {
  // A throwing chunk does not stop the others: every other chunk still
  // completes. The throwing chunk itself aborts at its failing slot, so
  // its remaining slots do not run — the exception escapes fn
  // immediately, exactly like the inline path. 1000 slots in chunks of
  // 10; the chunk holding slot 500 aborts (10 slots), the other 99
  // chunks complete fully.
  sim::ThreadPool pool(2);
  std::atomic<std::size_t> done{0};
  EXPECT_THROW(
      {
        pool.run_chunked(1000, 10, [&](unsigned, std::size_t begin,
                                       std::size_t end) {
          for (std::size_t i = begin; i < end; ++i) {
            if (i == 500) {
              throw std::logic_error("boom");
            }
            ++done;
          }
        });
      },
      std::logic_error);
  EXPECT_EQ(done.load(), std::size_t{990});
}

TEST(ThreadPoolTest, LifecycleShutdownIsClean) {
  // Construct, run, let the destructor join: workers must exit (the
  // test would hang otherwise). Also exercised under TSan/ASan by the
  // sanitizer CI jobs.
  {
    sim::ThreadPool pool(4);
    std::atomic<std::size_t> total{0};
    pool.run_chunked(4096, 128,
                     [&](unsigned, std::size_t begin, std::size_t end) {
                       total.fetch_add(end - begin);
                     });
    EXPECT_EQ(total.load(), std::size_t{4096});
  }
  SUCCEED() << "pool destroyed and joined cleanly";
}

TEST(ThreadPoolTest, SingleChunkCoversWholeRange) {
  // chunk >= count degenerates to one chunk holding the whole range —
  // well-defined for every worker count.
  for (unsigned workers : {0u, 2u, 4u}) {
    sim::ThreadPool pool(workers);
    std::atomic<int> calls{0};
    std::atomic<std::size_t> covered{0};
    pool.run_chunked(50, 50, [&](unsigned, std::size_t begin,
                                std::size_t end) {
      calls.fetch_add(1);
      covered.fetch_add(end - begin);
    });
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(covered.load(), std::size_t{50});
  }
}
