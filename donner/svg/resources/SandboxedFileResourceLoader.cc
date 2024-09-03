#include "donner/svg/resources/SandboxedFileResourceLoader.h"

#include <fstream>
#include <string>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {

namespace {

bool IsPathUnderRoot(const std::filesystem::path& root, const std::filesystem::path& path) {
  assert(root.is_absolute());
  const std::filesystem::path absRoot = root.lexically_normal();
  const std::filesystem::path absPath = std::filesystem::absolute(path).lexically_normal();

  return absPath.string().find(absRoot.string()) == 0;
}

}  // namespace

SandboxedFileResourceLoader::SandboxedFileResourceLoader(const std::filesystem::path& root,
                                                         const std::filesystem::path& documentPath)
    : root_(root), documentPath_(documentPath) {
  UTILS_RELEASE_ASSERT_MSG(std::filesystem::is_directory(root_), "Root directory does not exist");

  root_ = std::filesystem::absolute(root_);
  documentPath_ = std::filesystem::absolute(documentPath);
  documentPath_ = documentPath_.parent_path();
}

std::variant<std::vector<uint8_t>, ResourceLoaderError>
SandboxedFileResourceLoader::fetchExternalResource(std::string_view url) {
  // Convert the url to a path, make it absolute, and make sure it's relative to the root.
  std::filesystem::path path(url);
  if (!path.is_absolute()) {
    path = documentPath_ / path;
  }

  if (!IsPathUnderRoot(root_, path)) {
    return ResourceLoaderError::SandboxViolation;
  }

  // Validated, now read the file.
  std::ifstream file(path, std::ios::binary);
  if (!file.is_open()) {
    return ResourceLoaderError::NotFound;
  }

  // Read the file into a vector.
  file.seekg(0, std::ios::end);

  std::vector<uint8_t> data;
  const std::streamsize fileSize = file.tellg();

  data.resize(fileSize);
  file.seekg(0, std::ios::beg);
  file.read(reinterpret_cast<char*>(data.data()), fileSize);  // NOLINT, allow reinterpret_cast.
  file.close();

  return data;
}

}  // namespace donner::svg
