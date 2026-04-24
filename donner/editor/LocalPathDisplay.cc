#include "donner/editor/LocalPathDisplay.h"

#include <algorithm>

namespace donner::editor {

namespace {

constexpr std::string_view kFileScheme = "file://";

/// True iff `uri` carries a non-file scheme (http, https, data, etc.).
bool HasRemoteScheme(std::string_view uri) {
  const auto colon = uri.find(':');
  if (colon == std::string_view::npos || colon + 2 >= uri.size()) {
    return false;
  }
  if (uri[colon + 1] != '/' || uri[colon + 2] != '/') {
    // `data:` doesn't use `//` but still isn't a local file.
    return uri.substr(0, colon) == "data";
  }
  return uri.substr(0, kFileScheme.size()) != kFileScheme;
}

}  // namespace

std::string PrettifyLocalPath(std::string_view uri, const std::filesystem::path& baseDir) {
  if (uri.empty() || HasRemoteScheme(uri)) {
    return std::string(uri);
  }

  std::string_view pathStr = uri;
  if (pathStr.substr(0, kFileScheme.size()) == kFileScheme) {
    pathStr.remove_prefix(kFileScheme.size());
  }

  std::filesystem::path absolute = std::filesystem::path(pathStr);
  if (!absolute.is_absolute()) {
    absolute = baseDir / absolute;
  }
  absolute = absolute.lexically_normal();

  const auto relative = absolute.lexically_relative(baseDir.lexically_normal());
  const std::string relStr = relative.generic_string();
  if (relStr.empty() || relStr == "." || relStr.substr(0, 2) == "..") {
    // Outside the cwd, or exactly the cwd — leave the input alone.
    return std::string(uri);
  }

  return "./" + relStr;
}

}  // namespace donner::editor
