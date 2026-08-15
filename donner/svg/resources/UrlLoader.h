#pragma once
/// @file

#include <cstddef>
#include <string>

#include "donner/base/Utils.h"
#include "donner/svg/resources/ResourceLoaderInterface.h"

namespace donner::svg {

/**
 * Enum of possible errors that can occur when loading an image.
 */
enum class UrlLoaderError : uint8_t {
  NotFound,           ///< The file was not found.
  UnsupportedFormat,  ///< The format is not supported (for images, mime type must be either
                      ///< "image/png" or "image/jpeg").
  InvalidDataUrl,     ///< The data URL is invalid.
  DataCorrupt,        ///< The loaded data is corrupt and cannot be decoded.
  ResourceTooLarge,   ///< The loaded or decoded resource exceeds the byte limit.
};

/// Converts a \ref UrlLoaderError to a human-readable string.
inline std::string_view ToString(UrlLoaderError err) {
  switch (err) {
    case UrlLoaderError::NotFound: return "File not found";
    case UrlLoaderError::UnsupportedFormat: return "Unsupported format";
    case UrlLoaderError::InvalidDataUrl: return "Invalid data URL";
    case UrlLoaderError::DataCorrupt: return "Data corrupted";
    case UrlLoaderError::ResourceTooLarge: return "Resource too large";
  }

  UTILS_UNREACHABLE();
}

/// Ostream operator for \ref UrlLoaderError.
inline std::ostream& operator<<(std::ostream& os, UrlLoaderError err) {
  return os << ToString(err);
}

/**
 * Utility class for loading a URI or decoding a data URL.
 */
class UrlLoader {
public:
  /// Default maximum decoded or fetched resource size.
  static constexpr size_t kDefaultMaximumResourceSize = 16 * 1024 * 1024;

  /**
   * Result of loading a URI or decoding a data URL.
   */
  struct Result {
    /// Loaded data, either from a data URL or from a fetched URI.
    std::vector<uint8_t> data;

    /// MIME type of the data, if known. Otherwise, an empty string.
    std::string mimeType;
  };

  /**
   * Create a new URL loader that uses the given resource loader to fetch external resources.
   *
   * @param resourceLoader Resource loader to use for fetching external resources.
   */
  explicit UrlLoader(ResourceLoaderInterface& resourceLoader,
                     size_t maximumResourceSize = kDefaultMaximumResourceSize,
                     size_t* remainingResourceBytes = nullptr)
      : resourceLoader_(resourceLoader),
        maximumResourceSize_(maximumResourceSize),
        remainingResourceBytes_(remainingResourceBytes) {}

  /// Destructor.
  ~UrlLoader() = default;

  // No copy or move.
  UrlLoader(const UrlLoader&) = delete;
  UrlLoader(UrlLoader&&) = delete;
  UrlLoader& operator=(const UrlLoader&) = delete;
  UrlLoader& operator=(UrlLoader&&) = delete;

  /**
   * Read an image from a URI, which can be a file path, a URL, or a data URL (e.g.
   * "data:image/png;base64,...").
   *
   * @param uri URI of the image, or data URL containing a base64 embedded image.
   * @return Loaded image data or an error code.
   */
  std::variant<Result, UrlLoaderError> fromUri(std::string_view uri);

private:
  bool consumeResourceBytes(size_t size);

  /// Resource loader to use for fetching external resources.
  ResourceLoaderInterface& resourceLoader_;  // NOLINT

  /// Maximum decoded or fetched bytes returned to the caller.
  size_t maximumResourceSize_;

  /// Optional shared byte budget across multiple loaders in one document.
  size_t* remainingResourceBytes_;
};

}  // namespace donner::svg
