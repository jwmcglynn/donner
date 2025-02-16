#pragma once
/// @file

#include <cstdint>
#include <string_view>
#include <variant>
#include <vector>

namespace donner::svg {

/// Error codes for resource loading.
enum class ResourceLoaderError : uint8_t {
  NotFound,          //!< File not found.
  SandboxViolation,  //!< File access violation, such as attempting to access a file outside the
                     //!< sandbox.
};

/**
 * Interface for loading external resources, such as images. To load files from the local
 * filesystem, use \ref SandboxedFileResourceLoader.
 */
class ResourceLoaderInterface {
public:
  /// Default constructor.
  ResourceLoaderInterface() = default;

  // No copy or move.
  ResourceLoaderInterface(const ResourceLoaderInterface&) = delete;
  ResourceLoaderInterface& operator=(const ResourceLoaderInterface&) = delete;
  ResourceLoaderInterface(ResourceLoaderInterface&&) = delete;
  ResourceLoaderInterface& operator=(ResourceLoaderInterface&&) = delete;

  /// Destructor.
  virtual ~ResourceLoaderInterface() = default;

  /**
   * Fetch external resource from a given URL.
   *
   * @param url URL of the external resource.
   * @returns A vector containing the fetched data, or \c std::nullopt if the resource could not be
   * loaded.
   */
  virtual std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) = 0;
};

}  // namespace donner::svg
