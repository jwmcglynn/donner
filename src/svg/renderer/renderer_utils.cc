#include "src/svg/renderer/renderer_utils.h"

#include <stb/stb_image_write.h>

#include <fstream>

#include "src/svg/components/document_context.h"
#include "src/svg/components/rendering_context.h"
#include "src/svg/registry/registry.h"

namespace donner::svg {

void RendererUtils::prepareDocumentForRendering(SVGDocument& document, bool verbose,
                                                std::vector<ParseError>* outWarnings) {
  Registry& registry = document.registry();
  registry.ctx_or_set<RenderingContext>(registry).instantiateRenderTree(verbose, outWarnings);
}

bool RendererUtils::writeRgbaPixelsToPngFile(const char* filename,
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
