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
  if (!std::isfinite(options.threshold) || options.threshold < 0.0 || options.threshold > 1.0 ||
      options.maxMismatchedPixels < 0) {
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
  if (expected.width != actual.width || expected.height != actual.height ||
      expected.strideInPixels != actual.strideInPixels) {
    result.error = "dimension-mismatch";
    return result;
  }
  Image diff{expected.width, expected.height, expected.strideInPixels,
             std::vector<uint8_t>(expected.data.size())};
  pixelmatch::Options policy;
  policy.threshold = options.threshold;
  policy.includeAA = options.includeAntiAliasing;
  const int count = pixelmatch::pixelmatch(expected.data, actual.data, diff.data, expected.width,
                                           expected.height, expected.strideInPixels, policy);
  if (count < 0) {
    result.error = "pixelmatch rejected the image buffers";
    return result;
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
