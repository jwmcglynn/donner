#include "donner/svg/resources/SandboxedFileResourceLoader.h"

#include <cstdint>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {

namespace {

bool IsPathUnderRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
  assert(root.is_absolute());
  assert(path.is_absolute());

  auto rootIt = root.begin();
  auto pathIt = path.begin();
  for (; rootIt != root.end() && pathIt != path.end(); ++rootIt, ++pathIt) {
    if (*rootIt != *pathIt) {
      return false;
    }
  }
  return rootIt == root.end();
}

}  // namespace

SandboxedFileResourceLoader::SandboxedFileResourceLoader(const std::filesystem::path& root,
                                                         const std::filesystem::path& documentPath,
                                                         size_t maximumResourceSize)
    : root_(root), documentPath_(documentPath), maximumResourceSize_(maximumResourceSize) {
  UTILS_RELEASE_ASSERT_MSG(std::filesystem::is_directory(root_), "Root directory does not exist");

  root_ = std::filesystem::canonical(root_);
  documentPath_ =
      std::filesystem::weakly_canonical(std::filesystem::absolute(documentPath)).parent_path();
}

std::variant<std::vector<uint8_t>, ResourceLoaderError>
SandboxedFileResourceLoader::fetchExternalResource(std::string_view url) {
  // Convert the url to a path, make it absolute, and make sure it's relative to the root.
  std::filesystem::path path(url);
  if (!path.is_absolute()) {
    path = documentPath_ / path;
  }

  std::error_code canonicalError;
  path = std::filesystem::canonical(path, canonicalError);
  if (canonicalError) {
    return ResourceLoaderError::NotFound;
  }

  if (!IsPathUnderRoot(root_, path)) {
    return ResourceLoaderError::SandboxViolation;
  }

  std::error_code statusError;
  if (!std::filesystem::is_regular_file(path, statusError) || statusError) {
    return ResourceLoaderError::NotFound;
  }

  const uintmax_t fileSize = std::filesystem::file_size(path, statusError);
  if (statusError || fileSize > std::numeric_limits<size_t>::max()) {
    return ResourceLoaderError::NotFound;
  }
  if (fileSize > maximumResourceSize_) {
    return ResourceLoaderError::TooLarge;
  }

  // Open the canonical path rather than the attacker-controlled symlink path.
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return ResourceLoaderError::NotFound;
  }

  std::vector<uint8_t> data;
  data.resize(static_cast<size_t>(fileSize));
  file.read(reinterpret_cast<char*>(data.data()),
            static_cast<std::streamsize>(fileSize));  // NOLINT
  if (file.bad() || file.gcount() != static_cast<std::streamsize>(fileSize)) {
    return ResourceLoaderError::NotFound;
  }
  if (file.peek() != std::char_traits<char>::eof()) {
    return ResourceLoaderError::TooLarge;
  }

  return data;
}

}  // namespace donner::svg
