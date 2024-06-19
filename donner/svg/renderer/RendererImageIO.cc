#include "donner/svg/renderer/RendererImageIO.h"

#include <stb/stb_image_write.h>

#include <cassert>
#include <fstream>

namespace donner::svg {

bool RendererImageIO::writeRgbaPixelsToPngFile(const char* filename,
                                               std::span<const uint8_t> rgbaPixels, int width,
                                               int height, size_t strideInPixels) {
  struct Context {
    std::ofstream output;
  };

  assert(width > 0);
  assert(height > 0);
  assert(rgbaPixels.size() == width * height * 4);

  Context context;
  context.output = std::ofstream(filename, std::ofstream::out | std::ofstream::binary);
  if (!context.output) {
    return false;
  }

  stbi_write_png_to_func(
      [](void* context, void* data, int len) {
        Context* contextObj = reinterpret_cast<Context*>(context);
        contextObj->output.write(static_cast<const char*>(data), len);
      },
      &context, width, height, 4, rgbaPixels.data(),
      /* stride in bytes */ strideInPixels ? strideInPixels * 4 : width * 4);

  return context.output.good();
}

}  // namespace donner::svg
