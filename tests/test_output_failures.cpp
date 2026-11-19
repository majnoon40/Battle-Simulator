// m13 audit finding 12: silent CSV / Markdown / JSON / events / trace /
// replay write failures. The CLI must check the stream after writing
// (and flush, since stdio buffers small writes), exit nonzero when an
// output is incomplete, and name the affected path in the error.
//
// Failure injection: /dev/full — a character device whose open always
// succeeds and whose writes always fail with ENOSPC. Small outputs sit
// in the stream buffer and fail only at flush/close, which is exactly
// the case the old unchecked code missed. No privileges are needed and
// the failure is deterministic on Linux and macOS.
#include <sys/wait.h>

#include "wait_status.hpp"

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

// Runs a battle_cli command from the repo root (like the other CLI
// tests) and returns its exit code plus the combined output.
int run_cli(const std::string& arguments, std::string* output = nullptr) {
  const std::string command =
      "cd \"" TEST_DATA_DIR "/..\" && \"" BATTLE_CLI_PATH "\" " + arguments +
      " 2>&1";
  std::FILE* pipe = popen(command.c_str(), "r");
  if (pipe == nullptr) {
    return -1;
  }
  std::ostringstream captured;
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
    captured << buffer;
  }
  const int code = exit_status_of(pclose(pipe));
  if (output != nullptr) {
    *output = captured.str();
  }
  return code;
}

std::streamsize file_size(const std::string& path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  return file.tellg();
}

}  // namespace

// Every output kind fails the run (exit 1) when its writes fail, with
// an error naming the path.
TEST(OutputFailureTest, WriteFailuresFailTheRun) {
  const struct {
    const char* name;
    std::string arguments;
  } cases[] = {
      {"json-export", "--seed 42 --ticks 60 --export-json /dev/full"},
      {"csv-timeline", "--seed 42 --ticks 60 --export-csv /dev/full"},
      {"report-markdown", "--seed 42 --ticks 60 --report /dev/full"},
      // The events recorder writes during the run; 400 ticks of the
      // default 250v250 battle is past first contact (~t167), so real
      // event lines are written and the flush check has something to
      // catch.
      {"events", "--seed 42 --ticks 400 --events /dev/full"},
      {"trace", "--seed 42 --ticks 60 --trace /dev/full"},
      {"replay-record", "--seed 42 --ticks 60 --record /dev/full"},
  };
  for (const auto& item : cases) {
    std::string output;
    const int code = run_cli(item.arguments, &output);
    EXPECT_EQ(code, 1) << item.name << " (output: " << output << ")";
    EXPECT_NE(output.find("error:"), std::string::npos) << item.name;
    EXPECT_NE(output.find("/dev/full"), std::string::npos)
        << item.name << " (output: " << output << ")";
  }
}

// Positive controls: the same flags with writable paths exit 0 and
// produce non-empty files (also proving the /dev/full runs above had
// real bytes to write).
TEST(OutputFailureTest, WritablePathsStillSucceed) {
  EXPECT_EQ(run_cli("--seed 42 --ticks 60 --export-json /tmp/m13_of.json"),
            0);
  EXPECT_GT(file_size("/tmp/m13_of.json"), 0);
  EXPECT_EQ(run_cli("--seed 42 --ticks 60 --export-csv /tmp/m13_of.csv"), 0);
  EXPECT_GT(file_size("/tmp/m13_of.csv"), 0);
  EXPECT_EQ(run_cli("--seed 42 --ticks 60 --report /tmp/m13_of_report.md"),
            0);
  EXPECT_GT(file_size("/tmp/m13_of_report.md"), 0);
  // Both report files exist (the .json twin next to the .md).
  EXPECT_GT(file_size("/tmp/m13_of_report.json"), 0)
      << "report JSON twin is written next to the markdown";
  EXPECT_EQ(run_cli("--seed 42 --ticks 400 --events /tmp/m13_of.jsonl"), 0);
  EXPECT_GT(file_size("/tmp/m13_of.jsonl"), 0)
      << "the 400-tick window must contain events (else the /dev/full "
         "events case is vacuous)";
  EXPECT_EQ(run_cli("--seed 42 --ticks 60 --trace /tmp/m13_of.trace"), 0);
  EXPECT_GT(file_size("/tmp/m13_of.trace"), 0);
  EXPECT_EQ(run_cli("--seed 42 --ticks 60 --record /tmp/m13_of.replay"), 0);
  EXPECT_GT(file_size("/tmp/m13_of.replay"), 0);
}

// m13 reliability review: a 23-digit --render-frames tick overflows
// uint64; std::stoull threw out_of_range, which escaped main and
// aborted the CLI (SIGABRT). It must be a clean exit-1 error.
TEST(OutputFailureTest, RenderFramesOverflowIsRejected) {
  std::string output;
  EXPECT_EQ(run_cli("--seed 42 --ticks 60 --render-frames "
                    "99999999999999999999999 --out-prefix /tmp/m13rf",
                    &output),
            1);
  EXPECT_NE(output.find("out of range"), std::string::npos) << output;
  // The uint64 boundary itself is fine (the run stops at the tick
  // limit long before it, but parsing must not fail).
  EXPECT_EQ(run_cli("--seed 42 --ticks 60 --render-frames "
                    "18446744073709551615 --out-prefix /tmp/m13rf",
                    &output),
            0);
}

// m13 reliability review: UI script integers are read through a
// range-checked reader — nlohmann's get<int> is an unchecked
// static_cast, so {"width": 1e300} was undefined behavior and
// {"width": 2.5} was silently truncated.
TEST(OutputFailureTest, UiScriptNumbersAreRangeChecked) {
  const auto write_script = [](const std::string& body) {
    const std::string path = "/tmp/m13_ui_script.json";
    { std::ofstream file(path); file << body; }
    return path;
  };
  std::string output;
  // Non-integer / huge width and height: clean errors.
  EXPECT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 7 "
                    "--ticks 30 --ui-script " +
                        write_script(
                            R"({"width": 1e300, "height": 300,)"
                            R"( "tick": 0, "out": "/tmp/m13ui.png",)"
                            R"( "events": []})"),
                        &output),
            1)
      << output;
  EXPECT_NE(output.find("width"), std::string::npos) << output;
  EXPECT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 7 "
                    "--ticks 30 --ui-script " +
                        write_script(
                            R"({"width": 2.5, "height": 300,)"
                            R"( "tick": 0, "out": "/tmp/m13ui.png",)"
                            R"( "events": []})"),
                        &output),
            1)
      << output;
  // A huge (double) event frame: clean error instead of a cast.
  EXPECT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 7 "
                    "--ticks 30 --ui-script " +
                        write_script(
                            R"({"width": 640, "height": 480,)"
                            R"( "tick": 0, "out": "/tmp/m13ui.png",)"
                            R"( "events": [{"frame": 1e30,)"
                            R"( "type": "key", "key": "tab"}]})"),
                        &output),
            1)
      << output;
  EXPECT_NE(output.find("frame"), std::string::npos) << output;
  // Optional coordinate fields with out-of-range values fall back to
  // their defaults instead of converting badly (the run succeeds).
  EXPECT_EQ(run_cli("--scenario data/scenarios/squads.json --seed 7 "
                    "--ticks 30 --ui-script " +
                        write_script(
                            R"({"width": 640, "height": 480,)"
                            R"( "tick": 0, "out": "/tmp/m13ui.png",)"
                            R"( "events": [{"frame": 0, "type": "move",)"
                            R"( "x": 1e300, "y": 1e300}]})"),
                        &output),
            0)
      << output;
}
