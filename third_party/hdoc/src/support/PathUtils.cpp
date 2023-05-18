// Copyright 2019-2023 hdoc
// SPDX-License-Identifier: AGPL-3.0-only

#include "support/PathUtils.hpp"

#include <filesystem>

namespace hdoc::utils {

std::string pathToRelative(const std::string& path, const std::string& rootDir) {
  if (path.find(rootDir) == 0) {
    if (path.size() >= rootDir.size() + 1 && path[rootDir.size()] == '/') {
      std::string relPath = path;
      relPath.replace(0, rootDir.length() + 1, "");
      return relPath;
    }
  }

  return std::filesystem::relative(std::filesystem::path(path), rootDir).string();
}

} // namespace hdoc::utils
