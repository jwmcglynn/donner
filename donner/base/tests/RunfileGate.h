#pragma once
/// @file
/// What a test does when a file it declared in `data` is not in its runfiles.
///
/// Unlike a missing GPU adapter or an uninstalled command-line tool, this is never a property of
/// the machine: the runfiles tree is generated from the target's `data` attribute, so an entry that
/// does not resolve is a build-graph defect on every lane and every host. gtest reports a run whose
/// every case skipped as a pass, so skipping on a missing data file hides that defect behind a
/// green result.

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/tests/Runfiles.h"

namespace donner::tests {

/// A file the test target lists in `data`, with its location and contents, or the reason it is
/// unusable.
struct RequiredRunfile {
  /// Path as the test asked for it, relative to the workspace root.
  std::string requestedPath;

  /// Location the file was found at. Empty when it was not found.
  std::string path;

  /// File contents. Populated only by ReadRequiredRunfile(), and only when \ref error is empty.
  std::string contents;

  /// Why the file is unusable, naming the path, the target that should carry it, and every
  /// resolution attempted. Empty when the file is usable.
  std::string error;

  /// Whether the file resolved, and was read when reading was asked for.
  bool ok() const { return error.empty(); }
};

/**
 * Label of the target under test, as Bazel reports it.
 *
 * @return The label, or a description of it when the process was not launched by Bazel.
 */
inline std::string TestTargetLabel() {
  const char* label = std::getenv("TEST_TARGET");
  if (label != nullptr && *label != '\0') {
    return label;
  }
  return "the test target that owns this case";
}

/**
 * The failure text for a runfile that did not resolve.
 *
 * @param requestedPath Path as the test asked for it.
 * @param targetLabel Target whose `data` attribute should carry the path.
 * @param manifestPath What the runfiles manifest mapped the path to, empty when it mapped nothing.
 * @param workingDirectoryPath What the path resolves to against the test's working directory.
 * @return A message naming the path, the target, and both resolutions attempted.
 */
inline std::string MissingRunfileMessage(std::string_view requestedPath,
                                         std::string_view targetLabel,
                                         std::string_view manifestPath,
                                         std::string_view workingDirectoryPath) {
  std::string message =
      "Required runfile \"" + std::string(requestedPath) + "\" is not in the runfiles tree.\n";
  message += "Add it to the `data` attribute of " + std::string(targetLabel) + ".\n";
  message +=
      "Failing rather than skipping: a runfiles tree is generated from `data`, so a path that "
      "does not resolve is a build-graph defect on every lane and every host, not something this "
      "machine lacks. gtest reports a run whose every case skipped as a pass, so skipping here "
      "would report success without exercising anything.\n";
  message += "Resolution attempted:\n";
  message += "  runfiles manifest -> " +
             (manifestPath.empty() ? std::string("(unmapped)") : std::string(manifestPath)) + "\n";
  message += "  test working directory -> " + std::string(workingDirectoryPath);
  return message;
}

/**
 * Locates a file the test target lists in `data`, without reading it.
 *
 * Resolves through the runfiles manifest, then against the test's working directory, which is the
 * runfiles root under Bazel.
 *
 * @param runfilePath Path relative to the workspace root, as it appears in `data`.
 * @return The resolved location, or a populated RequiredRunfile::error.
 */
inline RequiredRunfile ResolveRequiredRunfile(std::string_view runfilePath) {
  RequiredRunfile result;
  result.requestedPath = std::string(runfilePath);

  const std::string manifestPath = Runfiles::instance().Rlocation(result.requestedPath);
  if (!manifestPath.empty() && std::filesystem::exists(manifestPath)) {
    result.path = manifestPath;
    return result;
  }

  std::error_code ignored;
  const std::string workingDirectoryPath =
      (std::filesystem::current_path(ignored) / result.requestedPath).string();
  if (std::filesystem::exists(workingDirectoryPath)) {
    result.path = workingDirectoryPath;
    return result;
  }

  result.error = MissingRunfileMessage(result.requestedPath, TestTargetLabel(), manifestPath,
                                       workingDirectoryPath);
  return result;
}

/**
 * Reads a file the test target lists in `data`.
 *
 * @param runfilePath Path relative to the workspace root, as it appears in `data`.
 * @return The file's contents, or a populated RequiredRunfile::error.
 */
inline RequiredRunfile ReadRequiredRunfile(std::string_view runfilePath) {
  RequiredRunfile result = ResolveRequiredRunfile(runfilePath);
  if (!result.ok()) {
    return result;
  }

  std::ifstream stream(result.path, std::ios::binary);
  if (!stream.is_open()) {
    result.error = "Required runfile \"" + result.requestedPath + "\" resolved to " + result.path +
                   " but could not be opened for reading.";
    return result;
  }

  std::ostringstream buffer;
  buffer << stream.rdbuf();
  result.contents = buffer.str();
  return result;
}

}  // namespace donner::tests

/**
 * Ends the current test case when \p runfile did not resolve, reporting the path, the target that
 * should carry it in `data`, and every resolution attempted.
 *
 * Use directly in a `TEST` body, a fixture method, or a void helper. In a helper that returns a
 * value the fatal failure would not stop the caller, so check RequiredRunfile::ok() there instead.
 *
 * @param runfile A donner::tests::RequiredRunfile.
 */
#define DONNER_REQUIRE_RUNFILE(runfile)                                       \
  do {                                                                        \
    const ::donner::tests::RequiredRunfile& donnerRequiredRunfile = (runfile); \
    if (!donnerRequiredRunfile.ok()) {                                         \
      FAIL() << donnerRequiredRunfile.error;                                   \
    }                                                                          \
  } while (false)
