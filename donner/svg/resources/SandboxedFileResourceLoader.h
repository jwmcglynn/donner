#pragma once
/// @file

#include <filesystem>

#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {

/**
 * A resource loader that loads files from a sandboxed directory.
 */
class SandboxedFileResourceLoader : public ResourceLoaderInterface {
public:
  /**
   * Create a new resource loader that loads files sandboxed within the given root directory.
   *
   * Paths should be relative to the documentPath directory. Paths that use ".." to escape the root
   * directory will be rejected.
   *
   * @param root Sandbox directory, loads outside of this directory will be rejected.
   * @param documentPath Path to the document being loaded, used to resolve relative paths.
   */
  explicit SandboxedFileResourceLoader(const std::filesystem::path& root,
                                       const std::filesystem::path& documentPath);

  // No copy or move.
  SandboxedFileResourceLoader(const SandboxedFileResourceLoader&) = delete;
  SandboxedFileResourceLoader& operator=(const SandboxedFileResourceLoader&) = delete;
  SandboxedFileResourceLoader(SandboxedFileResourceLoader&&) = delete;
  SandboxedFileResourceLoader& operator=(SandboxedFileResourceLoader&&) = delete;

  /// Destructor.
  ~SandboxedFileResourceLoader() override = default;

  /**
   * Fetch external resource from a given URL.
   *
   * @param url URL of the external resource.
   * @returns A vector containing the fetched data.
   */
  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override;

private:
  std::filesystem::path root_;          //!< Root directory of the sandbox.
  std::filesystem::path documentPath_;  //!< Path to the document being loaded.
};

}  // namespace donner::svg
