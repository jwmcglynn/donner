#pragma once
/// @file

#include <cstdlib>
#include <filesystem>

namespace donner {

/**
 * Bazel's per-test scratch directory (`TEST_TMPDIR`), falling back to the system
 * temp dir when running outside `bazel test`.
 *
 * Use this instead of `std::filesystem::temp_directory_path()` in tests: the
 * latter resolves to the shared `/tmp` on remote-execution workers (which do not
 * set `TMPDIR`), so fixed filenames written there collide across users - a file
 * left by one worker user makes a later run under a different user fail to open
 * the same path for write.
 */
inline std::filesystem::path TestTempDir() {
  if (const char* testTmpdir = std::getenv("TEST_TMPDIR"); testTmpdir && *testTmpdir) {
    return std::filesystem::path(testTmpdir);
  }
  return std::filesystem::temp_directory_path();
}

}  // namespace donner
