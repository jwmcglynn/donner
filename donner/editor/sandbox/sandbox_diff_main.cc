/// @file
///
/// `sandbox_diff` — structurally diffs two `.rnr` recordings for regression
/// triage. Compares headers and decoded command streams, printing a
/// unified-diff-style report with depth-aware indentation.
///
/// Usage:
///     sandbox_diff <file_a.rnr> <file_b.rnr>
///
/// Exit codes: 0 = identical, 1 = differ, 2 = I/O or decode error.

#include <cstdio>
#include <filesystem>
#include <vector>

#include "donner/editor/sandbox/FrameInspector.h"
#include "donner/editor/sandbox/RnrFile.h"
#include "donner/editor/sandbox/SandboxDiff.h"

namespace {

const char* RnrStatusString(donner::editor::sandbox::RnrIoStatus status) {
  using donner::editor::sandbox::RnrIoStatus;
  switch (status) {
    case RnrIoStatus::kOk:              return "ok";
    case RnrIoStatus::kWriteFailed:     return "write failed";
    case RnrIoStatus::kReadFailed:      return "read failed";
    case RnrIoStatus::kTruncated:       return "truncated header";
    case RnrIoStatus::kMagicMismatch:   return "magic mismatch";
    case RnrIoStatus::kVersionMismatch: return "version mismatch";
    case RnrIoStatus::kUriTooLong:      return "uri too long";
  }
  return "unknown";
}

}  // namespace

int main(int argc, char* argv[]) {
  using namespace donner::editor::sandbox;  // NOLINT(google-build-using-namespace)

  if (argc != 3) {
    std::fprintf(stderr, "usage: sandbox_diff <file_a.rnr> <file_b.rnr>\n");
    return 64;
  }

  const std::filesystem::path pathA = argv[1];
  const std::filesystem::path pathB = argv[2];

  // Load both files.
  RnrHeader headerA;
  std::vector<uint8_t> wireA;
  if (const auto s = LoadRnrFile(pathA, headerA, wireA); s != RnrIoStatus::kOk) {
    std::fprintf(stderr, "sandbox_diff: failed to load %s: %s\n", pathA.string().c_str(),
                 RnrStatusString(s));
    return 2;
  }

  RnrHeader headerB;
  std::vector<uint8_t> wireB;
  if (const auto s = LoadRnrFile(pathB, headerB, wireB); s != RnrIoStatus::kOk) {
    std::fprintf(stderr, "sandbox_diff: failed to load %s: %s\n", pathB.string().c_str(),
                 RnrStatusString(s));
    return 2;
  }

  // Decode both streams.
  const auto resultA = FrameInspector::Decode(wireA);
  if (!resultA.streamValid) {
    std::fprintf(stderr, "sandbox_diff: decode error in %s: %s\n", pathA.string().c_str(),
                 resultA.error.c_str());
    return 2;
  }

  const auto resultB = FrameInspector::Decode(wireB);
  if (!resultB.streamValid) {
    std::fprintf(stderr, "sandbox_diff: decode error in %s: %s\n", pathB.string().c_str(),
                 resultB.error.c_str());
    return 2;
  }

  // Compute and print the diff.
  const auto diff = ComputeRnrDiff(headerA, resultA.commands, headerB, resultB.commands);
  if (!diff.report.empty()) {
    std::fwrite(diff.report.data(), 1, diff.report.size(), stdout);
  }

  return diff.identical ? 0 : 1;
}
