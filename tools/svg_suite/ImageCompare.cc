#include "tools/svg_suite/ImageCompare.h"

#include <pixelmatch/pixelmatch.h>
#include <stb/stb_image_write.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace donner::svg_suite {
namespace {
constexpr size_t kMaxDecoderAllocationBytes = 256u * 1024u * 1024u;
thread_local size_t decoderAllocatedBytes = 0;
thread_local size_t decoderAllocationLimitBytes = kMaxDecoderAllocationBytes;
thread_local bool decoderAllocationLimitExceeded = false;

struct alignas(std::max_align_t) DecoderAllocation {
  size_t size;
};

void* AllocateDecoderBytes(size_t size) {
  if (decoderAllocationLimitBytes < sizeof(DecoderAllocation) ||
      size > decoderAllocationLimitBytes - sizeof(DecoderAllocation) ||
      size + sizeof(DecoderAllocation) > decoderAllocationLimitBytes - decoderAllocatedBytes) {
    decoderAllocationLimitExceeded = true;
    return nullptr;
  }
  const size_t allocationSize = size + sizeof(DecoderAllocation);
  auto* allocation = static_cast<DecoderAllocation*>(std::malloc(allocationSize));
  if (!allocation) {
    return nullptr;
  }
  allocation->size = size;
  decoderAllocatedBytes += allocationSize;
  return allocation + 1;
}

void FreeDecoderBytes(void* memory) {
  if (memory) {
    auto* allocation = static_cast<DecoderAllocation*>(memory) - 1;
    decoderAllocatedBytes -= allocation->size + sizeof(DecoderAllocation);
    std::free(allocation);
  }
}

void* ReallocateDecoderBytes(void* memory, size_t size) {
  if (!memory) {
    return AllocateDecoderBytes(size);
  }
  if (size == 0) {
    FreeDecoderBytes(memory);
    return nullptr;
  }
  auto* allocation = static_cast<DecoderAllocation*>(memory) - 1;
  void* replacement = AllocateDecoderBytes(size);
  if (!replacement) {
    return nullptr;
  }
  // Account for both buffers during growth; realloc can otherwise hide a transient allocation peak.
  std::memcpy(replacement, memory, std::min(size, allocation->size));
  FreeDecoderBytes(memory);
  return replacement;
}
}  // namespace
}  // namespace donner::svg_suite

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_MALLOC(size) donner::svg_suite::AllocateDecoderBytes(size)
#define STBI_REALLOC(memory, size) donner::svg_suite::ReallocateDecoderBytes(memory, size)
#define STBI_FREE(memory) donner::svg_suite::FreeDecoderBytes(memory)
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include <stb/stb_image.h>
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
#undef STBI_FREE
#undef STBI_REALLOC
#undef STBI_MALLOC
#undef STBI_ONLY_PNG
#undef STB_IMAGE_STATIC
#undef STB_IMAGE_IMPLEMENTATION

namespace donner::svg_suite {
namespace {
constexpr size_t kMaxDecodedBytes = 256u * 1024u * 1024u;
constexpr size_t kMaxEncodedBytes = 32u * 1024u * 1024u;
constexpr int kMaxDimension = 16384;

bool ValidDimensions(int width, int height) {
  return width > 0 && height > 0 && width <= kMaxDimension && height <= kMaxDimension &&
         static_cast<size_t>(width) <= kMaxDecodedBytes / 4 / static_cast<size_t>(height);
}

void NormalizeStride(ImageView& image, size_t stride, std::vector<uint8_t>& storage) {
  if (image.strideInPixels == stride) {
    return;
  }
  storage.resize(stride * static_cast<size_t>(image.height) * 4);
  for (size_t row = 0; row < static_cast<size_t>(image.height); ++row) {
    std::copy_n(image.data.begin() + row * image.strideInPixels * 4,
                static_cast<size_t>(image.width) * 4, storage.begin() + row * stride * 4);
  }
  image.strideInPixels = stride;
  image.data = storage;
}

bool ValidOptions(const ComparisonOptions& options) {
  return std::isfinite(options.threshold) && options.threshold >= 0.0 && options.threshold <= 1.0 &&
         options.maxMismatchedPixels >= 0 &&
         !(options.acceptQuarterPixelEdgeCoverage && options.includeAntiAliasing);
}

bool IsCounted(std::span<const uint8_t> diff, size_t pos, const pixelmatch::Color& counted) {
  return diff[pos] == counted.r && diff[pos + 1] == counted.g && diff[pos + 2] == counted.b &&
         diff[pos + 3] == counted.a;
}

bool PartiallyCovered(uint8_t alpha) {
  return alpha > 0 && alpha < 255;
}

/// True when the pixel at \p column, \p row of \p image is the partially covered row of a
/// horizontal edge: the pixel directly above or below it is empty or fully covered, and neither of
/// those two pixels has its alpha. Pixels outside the image do not count as empty.
bool OnHorizontalEdge(ImageView image, size_t column, size_t row) {
  const auto alphaAt = [&](size_t y) {
    return image.data[(y * image.strideInPixels + column) * 4 + 3];
  };
  const uint8_t alpha = alphaAt(row);
  bool besideEmptyOrFull = false;
  for (const bool above : {true, false}) {
    if (above ? row == 0 : row + 1 >= static_cast<size_t>(image.height)) {
      continue;
    }
    const uint8_t neighbor = alphaAt(above ? row - 1 : row + 1);
    if (neighbor == alpha) {
      return false;
    }
    besideEmptyOrFull = besideEmptyOrFull || !PartiallyCovered(neighbor);
  }
  return besideEmptyOrFull;
}

/// The alpha difference at \p column, \p row when the coverage and edge conditions of
/// ComparisonOptions::acceptQuarterPixelEdgeCoverage hold there, which leaves only the color
/// condition; std::nullopt otherwise. \p expected and \p actual share a stride.
std::optional<int> EdgeCoverageAlphaDelta(ImageView expected, ImageView actual, size_t column,
                                          size_t row) {
  const size_t pos = (row * expected.strideInPixels + column) * 4;
  const uint8_t expectedAlpha = expected.data[pos + 3];
  const uint8_t actualAlpha = actual.data[pos + 3];
  const int alphaDelta = std::abs(static_cast<int>(expectedAlpha) - static_cast<int>(actualAlpha));
  if (!PartiallyCovered(expectedAlpha) || !PartiallyCovered(actualAlpha) ||
      alphaDelta > kQuarterPixelEdgeCoverageMaxAlphaDelta ||
      !OnHorizontalEdge(expected, column, row)) {
    return std::nullopt;
  }
  return alphaDelta;
}

struct EdgeCoverage {
  int pixels = 0;
  int maxAlphaDelta = 0;
  bool failed = false;
};

/// Reclassifies the pixels pixelmatch counted in \p diff that
/// ComparisonOptions::acceptQuarterPixelEdgeCoverage accepts, redrawing each in
/// kEdgeCoverageDiffColor. \p expected and \p actual share \p diff's stride.
EdgeCoverage AcceptQuarterPixelEdgeCoverage(ImageView expected, ImageView actual,
                                            const pixelmatch::Options& policy, Image& diff) {
  // Both colors at the shared alpha; every other pixel stays transparent in both, so only the
  // candidates can differ when pixelmatch compares these buffers.
  std::vector<uint8_t> sharedExpected(diff.data.size(), 0);
  std::vector<uint8_t> sharedActual(diff.data.size(), 0);
  std::vector<std::pair<size_t, int>> candidates;
  for (size_t row = 0; row < static_cast<size_t>(diff.height); ++row) {
    for (size_t column = 0; column < static_cast<size_t>(diff.width); ++column) {
      const size_t pos = (row * diff.strideInPixels + column) * 4;
      if (!IsCounted(diff.data, pos, policy.diffColor)) {
        continue;
      }
      const std::optional<int> alphaDelta = EdgeCoverageAlphaDelta(expected, actual, column, row);
      if (!alphaDelta) {
        continue;
      }
      std::copy_n(expected.data.begin() + pos, 3, sharedExpected.begin() + pos);
      std::copy_n(actual.data.begin() + pos, 3, sharedActual.begin() + pos);
      sharedExpected[pos + 3] = sharedActual[pos + 3] =
          std::min(expected.data[pos + 3], actual.data[pos + 3]);
      candidates.emplace_back(pos, *alphaDelta);
    }
  }
  EdgeCoverage accepted;
  if (candidates.empty()) {
    return accepted;
  }

  pixelmatch::Options colorPolicy = policy;
  colorPolicy.includeAA = true;
  std::vector<uint8_t> colorDiff(diff.data.size());
  if (pixelmatch::pixelmatch(sharedExpected, sharedActual, colorDiff, diff.width, diff.height,
                             diff.strideInPixels, colorPolicy) < 0) {
    accepted.failed = true;
    return accepted;
  }
  for (const auto& [pos, alphaDelta] : candidates) {
    if (IsCounted(colorDiff, pos, colorPolicy.diffColor)) {
      continue;
    }
    std::copy(kEdgeCoverageDiffColor.begin(), kEdgeCoverageDiffColor.end(),
              diff.data.begin() + pos);
    ++accepted.pixels;
    accepted.maxAlphaDelta = std::max(accepted.maxAlphaDelta, alphaDelta);
  }
  return accepted;
}
}  // namespace

std::string ValidateImage(ImageView image) {
  if (!ValidDimensions(image.width, image.height) ||
      image.strideInPixels < static_cast<size_t>(image.width) ||
      image.strideInPixels > kMaxDecodedBytes / 4 / static_cast<size_t>(image.height)) {
    return "invalid or oversized RGBA dimensions/stride";
  }
  if (image.data.size() != image.strideInPixels * static_cast<size_t>(image.height) * 4) {
    return "RGBA byte length does not match stride and height";
  }
  return {};
}

ComparisonResult CompareImages(ImageView expected, ImageView actual,
                               const ComparisonOptions& options) {
  ComparisonResult result;
  if (!ValidOptions(options)) {
    result.error = "invalid pixelmatch options";
    return result;
  }
  result.error = ValidateImage(expected);
  if (result.error.empty()) {
    result.error = ValidateImage(actual);
  }
  if (!result.error.empty()) {
    return result;
  }
  if (expected.width != actual.width || expected.height != actual.height) {
    result.error = "dimension-mismatch";
    return result;
  }
  const size_t stride = std::min(expected.strideInPixels, actual.strideInPixels);
  std::vector<uint8_t> normalized;
  NormalizeStride(expected.strideInPixels > actual.strideInPixels ? expected : actual, stride,
                  normalized);
  Image diff{expected.width, expected.height, stride,
             std::vector<uint8_t>(stride * static_cast<size_t>(expected.height) * 4)};
  pixelmatch::Options policy;
  policy.threshold = options.threshold;
  policy.includeAA = options.includeAntiAliasing;
  int count = pixelmatch::pixelmatch(expected.data, actual.data, diff.data, expected.width,
                                     expected.height, stride, policy);
  if (count < 0) {
    result.error = "pixelmatch rejected the image buffers";
    return result;
  }
  if (options.acceptQuarterPixelEdgeCoverage) {
    const EdgeCoverage edges = AcceptQuarterPixelEdgeCoverage(expected, actual, policy, diff);
    if (edges.failed) {
      result.error = "pixelmatch rejected the edge coverage buffers";
      return result;
    }
    count -= edges.pixels;
    result.edgeCoveragePixels = edges.pixels;
    result.maxEdgeCoverageAlphaDelta = edges.maxAlphaDelta;
  }
  result.mismatchedPixels = count;
  result.passed = count <= options.maxMismatchedPixels;
  result.diff = std::move(diff);
  return result;
}

ImageResult ReadPng(const std::filesystem::path& path, size_t maxDecoderBytes) {
  if (maxDecoderBytes == 0 || maxDecoderBytes > kMaxDecoderAllocationBytes) {
    return {{}, "invalid PNG decoder allocation budget"};
  }
  decoderAllocationLimitBytes = maxDecoderBytes;
  std::error_code error;
  const uintmax_t size = std::filesystem::file_size(path, error);
  if (error || size < 8 || size > kMaxEncodedBytes) {
    return {{}, "missing, empty or oversized PNG"};
  }
  std::ifstream input(path, std::ios::binary);
  std::vector<uint8_t> encoded(static_cast<size_t>(size));
  if (!input.read(reinterpret_cast<char*>(encoded.data()), static_cast<std::streamsize>(size))) {
    return {{}, "cannot read complete PNG"};
  }
  constexpr std::array<uint8_t, 8> kSignature{137, 80, 78, 71, 13, 10, 26, 10};
  if (!std::equal(kSignature.begin(), kSignature.end(), encoded.begin())) {
    return {{}, "input is not PNG"};
  }
  decoderAllocationLimitExceeded = false;
  int width = 0;
  int height = 0;
  int channels = 0;
  const int encodedSize = static_cast<int>(encoded.size());
  if (!stbi_info_from_memory(encoded.data(), encodedSize, &width, &height, &channels) ||
      !ValidDimensions(width, height)) {
    return {{}, "invalid or oversized PNG dimensions"};
  }
  uint8_t* decoded =
      stbi_load_from_memory(encoded.data(), encodedSize, &width, &height, &channels, 4);
  if (!decoded) {
    return {{},
            decoderAllocationLimitExceeded ? "PNG decoder allocation limit exceeded"
                                           : "PNG decode failed"};
  }
  if (!ValidDimensions(width, height)) {
    stbi_image_free(decoded);
    return {{}, "invalid decoded PNG dimensions"};
  }
  Image result{width, height, static_cast<size_t>(width),
               std::vector<uint8_t>(decoded, decoded + static_cast<size_t>(width) * height * 4)};
  stbi_image_free(decoded);
  return {std::move(result), {}};
}

std::string WritePng(const std::filesystem::path& path, ImageView image) {
  const std::string error = ValidateImage(image);
  if (!error.empty()) {
    return error;
  }
  if (!stbi_write_png(path.string().c_str(), image.width, image.height, 4, image.data.data(),
                      static_cast<int>(image.strideInPixels * 4))) {
    return "cannot write PNG";
  }
  return {};
}

}  // namespace donner::svg_suite
