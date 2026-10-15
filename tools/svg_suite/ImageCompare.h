#pragma once
/// @file
/// Renderer-independent, bounded PNG IO and the shared pixelmatch comparison.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace donner::svg_suite {

/// Borrowed RGBA pixels, valid only for the duration of an operation accepting the view.
struct ImageView {
  int width = 0;
  int height = 0;
  size_t strideInPixels = 0;
  std::span<const uint8_t> data;
};

/// Owned, straight-alpha sRGB RGBA image. Padding is permitted after each row.
struct Image {
  int width = 0;
  int height = 0;
  size_t strideInPixels = 0;
  std::vector<uint8_t> data;
};

/// Explicit load/render outcome; errors never contain a usable image.
struct ImageResult {
  std::optional<Image> image;
  std::string error;
};

/// Pixelmatch policy. Defaults count every pixelmatch-visible difference, including antialiasing.
struct ComparisonOptions {
  double threshold = 0.0;
  int maxMismatchedPixels = 0;
  bool includeAntiAliasing = true;
};

/// Comparison result. An error never passes and has no mismatch count or diff.
struct ComparisonResult {
  bool passed = false;
  std::optional<int> mismatchedPixels;
  std::optional<Image> diff;
  std::string error;
};

/// Compare validated RGBA views through pixelmatch. Visible dimensions must match; row strides
/// may differ. Padding is ignored, with at most one bounded input normalization buffer.
/// @param expected Expected pixels; borrowed for this call only.
/// @param actual Actual pixels; borrowed for this call only.
/// @param options Reviewed comparison policy.
ComparisonResult CompareImages(ImageView expected, ImageView actual,
                               const ComparisonOptions& options = {});

/// Read a PNG with a 32 MiB encoded, 256 MiB decoded, and 16384-pixel dimension limit.
/// @param path Input PNG path.
/// @param maxDecoderBytes Maximum live decoder allocation bytes, including inflater scratch and
/// concurrent buffers during growth. May lower the 256 MiB ceiling, but cannot raise it.
ImageResult ReadPng(const std::filesystem::path& path,
                    size_t maxDecoderBytes = 256u * 1024u * 1024u);

/// Write a bounded RGBA image to an existing directory, returning an error string on failure.
/// @param path Output path.
/// @param image Pixels borrowed for this call only.
std::string WritePng(const std::filesystem::path& path, ImageView image);

/// Validate dimensions, stride and byte length under the decoded image bound.
/// @param image Pixels borrowed for this call only.
std::string ValidateImage(ImageView image);

}  // namespace donner::svg_suite
