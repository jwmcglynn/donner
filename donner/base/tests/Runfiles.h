#pragma once
/// @file

#include <iostream>
#include <string>

#include "donner/base/Utils.h"
#include "rules_cc/cc/runfiles/runfiles.h"

namespace donner {

/**
 * Helper class to access bazel runfiles in a test environment.
 *
 * To get the filename for a binary, use `Runfiles::instance().Rlocation("path/to/file")`.
 */
class Runfiles {
public:
  /// Get the Runfiles singleton instance.
  static Runfiles& instance() {
    static Runfiles runfiles;
    return runfiles;
  }

  /**
   * Get the runfile location for the given relative path.
   *
   * @param path Relative path to the file.
   */
  std::string Rlocation(const std::string& path) const {
    // First find the main repo.
    std::string target = runfiles_->Rlocation("donner/");
    UTILS_RELEASE_ASSERT_MSG(!target.empty(), "Failed to find main repo");

    // Then find the file in the main repo.
    return target + path;
  }

  /**
   * Get the runfile location for the given relative path in an external repository.
   *
   * @param repository External repository name, corresponding to the "@repo-name" in the BUILD
   * file. If the name is "@repo-name", specify "repo-name" here.
   * @param path Relative path to the file.
   */
  std::string RlocationExternal(const std::string& repository, const std::string& path) const {
    std::string target = runfiles_->Rlocation(repository + "/");
    UTILS_RELEASE_ASSERT_MSG(!target.empty(), "Failed to find external repo");

    return target + path;
  }

private:
  // Create an initialize a new Runfiles instance.
  Runfiles() {
    std::string error;
    runfiles_ = rules_cc::cc::runfiles::Runfiles::CreateForTest(&error);
    UTILS_RELEASE_ASSERT_MSG(runfiles_,
                             (std::string("Failed to create runfiles: ") + error).c_str());
  }

  /// Bazel rules_cc runfiles instance.
  rules_cc::cc::runfiles::Runfiles* runfiles_ = nullptr;
};

}  // namespace donner
