#include "donner/svg/renderer/RendererImageIO.h"

#include <stb/stb_image_write.h>

#include <fstream>
#include <limits>

namespace donner::svg {
namespace {

/// Returns the row stride in bytes, or zero if the span or encoder dimensions are invalid.
int PngRowStride(std::span<const uint8_t> pixels, int width, int height, size_t strideInPixels) {
  constexpr int kMaxInt = std::numeric_limits<int>::max();
  // stb_image_write keeps row offsets, filter scores, filtered input sizes, and stretchy-buffer
  // capacities in signed ints. Stay within every one of those arithmetic domains before calling it.
  constexpr size_t kMaxFilteredBytes = (static_cast<size_t>(kMaxInt) - 32'766u) / 2u;
  constexpr size_t kMaxScoredRowBytes = static_cast<size_t>(kMaxInt) / 128u;
  if (width <= 0 || height <= 0 || static_cast<size_t>(width) > kMaxScoredRowBytes / 4u) {
    return 0;
  }
  const size_t rowBytes = static_cast<size_t>(width) * 4;
  const size_t stridePixels = strideInPixels ? strideInPixels : static_cast<size_t>(width);
  if (stridePixels < static_cast<size_t>(width) || stridePixels > kMaxInt / 4) {
    return 0;
  }
  const size_t strideBytes = stridePixels * 4;
  if (static_cast<size_t>(height - 1) > static_cast<size_t>(kMaxInt) / strideBytes ||
      static_cast<size_t>(height) > kMaxFilteredBytes / (rowBytes + 1u) ||
      pixels.size() < rowBytes ||
      static_cast<size_t>(height - 1) > (pixels.size() - rowBytes) / strideBytes) {
    return 0;
  }
  return static_cast<int>(strideBytes);
}

}  // namespace

bool RendererImageIO::writeRgbaPixelsToPngFile(const char* filename,
                                               std::span<const uint8_t> rgbaPixels, int width,
                                               int height, size_t strideInPixels) {
  struct Context {
    std::ofstream output;
  };

  const int rowStride = PngRowStride(rgbaPixels, width, height, strideInPixels);
  if (rowStride == 0) {
    return false;
  }

  Context context;
  context.output = std::ofstream(filename, std::ofstream::out | std::ofstream::binary);
  if (!context.output) {
    return false;
  }

  const int encoded = stbi_write_png_to_func(
      [](void* context, void* data, int len) {
        Context* contextObj = static_cast<Context*>(context);
        contextObj->output.write(static_cast<const char*>(data), len);
      },
      &context, width, height, 4, rgbaPixels.data(), rowStride);

  context.output.flush();
  return encoded != 0 && context.output.good();
}

std::vector<uint8_t> RendererImageIO::writeRgbaPixelsToPngMemory(
    std::span<const uint8_t> rgbaPixels, int width, int height, size_t strideInPixels) {
  struct Context {
    std::vector<uint8_t> buffer;
  };

  const int rowStride = PngRowStride(rgbaPixels, width, height, strideInPixels);
  if (rowStride == 0) {
    return {};
  }

  Context context;

  const int encoded = stbi_write_png_to_func(
      [](void* context, void* data, int len) {
        Context* contextObj = static_cast<Context*>(context);
        const uint8_t* bytes = static_cast<const uint8_t*>(data);
        contextObj->buffer.insert(contextObj->buffer.end(), bytes, bytes + len);
      },
      &context, width, height, 4, rgbaPixels.data(), rowStride);

  if (encoded == 0) {
    return {};
  }
  return context.buffer;
}

}  // namespace donner::svg
