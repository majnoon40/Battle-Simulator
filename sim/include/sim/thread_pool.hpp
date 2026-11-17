#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <mutex>
#include <thread>
#include <vector>

namespace sim {

// Persistent worker pool for the deterministic multithreading item
// (m13 item 4, --threads).
//
// Contract (docs/determinism.md "Threading"):
//   - The pool is created once and reused for every tick; workers are
//     persistent threads, not per-tick spawns.
//   - run_chunked(count, chunk_size, fn) splits [0, count) into FIXED
//     chunks of `chunk_size` (the last chunk may be shorter) — the chunk
//     size is independent of the worker count, so every thread count
//     produces the same chunk boundaries and the same per-unit outputs.
//   - fn(worker_index, begin, end) is invoked once per chunk. Each
//     invocation owns the slot range [begin, end) exclusively; the
//     simulation guarantees that a chunk only ever writes to the slots
//     it owns (per-unit decision inputs, per-worker scratch buffers),
//     so the chunk bodies need no locking.
//   - Dynamic scheduling: workers claim chunks one at a time under the
//     pool mutex (a slow chunk cannot stall the whole phase; claim
//     critical sections are O(1) against chunk work). Claims, completion
//     accounting and dispatch state are all mutex-guarded, so a worker
//     can never observe a half-set-up or already-replaced task — chunk
//     BODIES run outside the lock.
//   - A worker exception is captured (the first one under the mutex
//     wins) and rethrown on the caller thread after all chunks have
//     completed; the pool stays usable afterwards.
//   - worker_count() == 0 means inline execution: run_chunked runs every
//     chunk on the calling thread with worker index 0. This is the
//     `--threads 1` path and is byte-identical to the pre-threads code.
//
// Only ONE run_chunked call may be active at a time (the simulation runs
// its phases strictly one after another; the pool has a single task
// slot). The task is dispatched through a stack-allocated adapter, so a
// dispatch performs no heap allocation.
class ThreadPool {
 public:
  // Spawns `worker_count` persistent workers. 0 and 1 both select inline
  // execution (a single worker thread would only add dispatch hops
  // without parallelism; the simulation thread does the work itself).
  explicit ThreadPool(unsigned worker_count) {
    if (worker_count <= 1) {
      return;
    }
    workers_.reserve(worker_count);
    for (unsigned index = 0; index < worker_count; ++index) {
      workers_.emplace_back([this, index] { worker_loop(index); });
    }
  }

  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      shutdown_ = true;
    }
    cv_.notify_all();
    for (std::thread& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
  }

  ThreadPool(const ThreadPool&) = delete;
  ThreadPool& operator=(const ThreadPool&) = delete;
  ThreadPool(ThreadPool&&) = delete;
  ThreadPool& operator=(ThreadPool&&) = delete;

  // Number of persistent workers (0 = inline execution on the caller).
  [[nodiscard]] unsigned worker_count() const noexcept {
    return static_cast<unsigned>(workers_.size());
  }

  // Runs fn(worker_index, begin, end) over [0, count) in fixed chunks.
  // Blocks until every chunk has completed (a throwing chunk does not
  // stop the others; the first exception is rethrown once all chunks
  // are done). Deterministic by contract: the chunk boundaries depend
  // only on (count, chunk_size), never on the worker count or timing.
  template <typename Fn>
  void run_chunked(std::size_t count, std::size_t chunk_size, Fn&& fn) {
    if (count == 0) {
      return;
    }
    if (chunk_size == 0 || chunk_size >= count) {
      chunk_size = count;  // one chunk: degenerate but well-defined
    }
    const std::size_t total_chunks =
        (count + chunk_size - 1) / chunk_size;
    if (workers_.empty()) {
      // Inline: the calling thread is worker 0, chunks in order.
      for (std::size_t chunk = 0; chunk < total_chunks; ++chunk) {
        const std::size_t begin = chunk * chunk_size;
        fn(std::size_t{0}, begin,
           begin + chunk_size < count ? begin + chunk_size : count);
      }
      return;
    }
    // Stack-allocated type-erased task: no heap allocation per dispatch.
    TaskAdapter<Fn> adapter(fn);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      task_ = &adapter;
      task_count_ = count;
      task_chunk_ = chunk_size;
      task_total_chunks_ = total_chunks;
      next_chunk_ = 0;
      completed_ = 0;
      error_ = std::exception_ptr();
      ++generation_;
      active_ = true;
    }
    cv_.notify_all();
    // The calling thread orchestrates only — the persistent workers do
    // the work (N workers saturate N cores; on fewer cores the OS
    // interleaves them; the result never depends on either).
    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait(lock,
                  [this, total_chunks] { return completed_ == total_chunks; });
    task_ = nullptr;
    active_ = false;
    lock.unlock();
    if (error_) {
      std::rethrow_exception(error_);
    }
  }

 private:
  struct Task {
    virtual ~Task() = default;
    virtual void run(unsigned worker, std::size_t begin,
                     std::size_t end) = 0;
  };

  template <typename Fn>
  struct TaskAdapter final : Task {
    Fn& fn;
    explicit TaskAdapter(Fn& function) : fn(function) {}
    void run(unsigned worker, std::size_t begin, std::size_t end) override {
      fn(worker, begin, end);
    }
  };

  void worker_loop(unsigned worker_index) {
    std::unique_lock<std::mutex> lock(mutex_);
    std::uint64_t my_generation = 0;
    for (;;) {
      cv_.wait(lock, [&] {
        return shutdown_ ||
               (active_ && generation_ == my_generation &&
                next_chunk_ < task_total_chunks_) ||
               (active_ && generation_ != my_generation);
      });
      if (shutdown_) {
        return;
      }
      if (generation_ != my_generation) {
        // A new dispatch: adopt it before claiming anything.
        my_generation = generation_;
      }
      if (!active_ || next_chunk_ >= task_total_chunks_) {
        continue;  // nothing left for us; sleep until the next dispatch
      }
      // Claim one chunk under the lock, then run it unlocked.
      const std::size_t chunk = next_chunk_++;
      Task* const task = task_;
      const std::size_t count = task_count_;
      const std::size_t chunk_size = task_chunk_;
      const std::size_t total_chunks = task_total_chunks_;
      lock.unlock();
      const std::size_t begin = chunk * chunk_size;
      const std::size_t end =
          begin + chunk_size < count ? begin + chunk_size : count;
      try {
        task->run(worker_index, begin, end);
      } catch (...) {
        std::lock_guard<std::mutex> error_lock(mutex_);
        if (!error_) {
          error_ = std::current_exception();
        }
      }
      lock.lock();
      ++completed_;
      if (completed_ == total_chunks) {
        done_cv_.notify_all();
      }
    }
  }

  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::condition_variable done_cv_;
  // Dispatch state — all guarded by mutex_; task_ is only dereferenced
  // outside the lock while its dispatch is active (the caller of
  // run_chunked waits for full completion before returning, so the
  // stack-allocated adapter always outlives every use).
  Task* task_ = nullptr;
  std::size_t task_count_ = 0;
  std::size_t task_chunk_ = 0;
  std::size_t task_total_chunks_ = 0;
  std::size_t next_chunk_ = 0;
  std::size_t completed_ = 0;
  std::uint64_t generation_ = 0;
  bool active_ = false;
  bool shutdown_ = false;
  std::exception_ptr error_;
};

}  // namespace sim
