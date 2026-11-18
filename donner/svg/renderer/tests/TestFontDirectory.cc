#include "donner/svg/renderer/tests/TestFontDirectory.h"

#include <algorithm>

namespace donner::svg {

std::vector<std::filesystem::path> ListFontFilesSorted(const std::filesystem::path& directory,
                                                       std::error_code& error) {
  std::vector<std::filesystem::path> paths;
  error.clear();
  std::filesystem::directory_iterator entry(directory, error);
  while (!error && entry != std::filesystem::directory_iterator()) {
    const std::filesystem::path extension = entry->path().extension();
    if (extension == ".ttf" || extension == ".otf") {
      paths.push_back(entry->path());
    }
    entry.increment(error);
  }
  if (error) {
    return {};
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

}  // namespace donner::svg
