#pragma once
/// @file

#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {

/**
 * A resource loader that does not load any resources and always returns an error.
 */
class NullResourceLoader : public ResourceLoaderInterface {
public:
  /**
   * Default constructor.
   */
  NullResourceLoader() = default;

  // No copy or move.
  NullResourceLoader(const NullResourceLoader&) = delete;
  NullResourceLoader& operator=(const NullResourceLoader&) = delete;
  NullResourceLoader(NullResourceLoader&&) = delete;
  NullResourceLoader& operator=(NullResourceLoader&&) = delete;

  /// Destructor.
  ~NullResourceLoader() override = default;

  /**
   * Try to fetch external resource from a given URL, always returning an error.
   *
   * @param url URL of the external resource.
   * @returns A vector containing the fetched data.
   */
  std::variant<std::vector<uint8_t>, ResourceLoaderError> fetchExternalResource(
      std::string_view url) override {
    // Always return an error, as this loader does not load any resources.
    return ResourceLoaderError::NotFound;
  }
};

}  // namespace donner::svg
