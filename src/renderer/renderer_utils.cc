#include "src/renderer/renderer_utils.h"

#include <stb/stb_image_write.h>

#include <fstream>

#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/path_component.h"
#include "src/svg/components/rect_component.h"
#include "src/svg/components/registry.h"
#include "src/svg/components/tree_component.h"

namespace donner {

void RendererUtils::prepareDocumentForRendering(SVGDocument& document) {
  Registry& registry = document.registry();

  for (auto view = registry.view<RectComponent>(); auto entity : view) {
    auto [rect] = view.get(entity);
    rect.computePath(registry.get_or_emplace<ComputedPathComponent>(entity));
  }

  for (auto view = registry.view<TreeComponent>(); auto entity : view) {
    registry.get_or_emplace<ComputedStyleComponent>(entity).compute(
        SVGElement::fromEntityUnchecked(registry, entity), registry, entity);
  }
}

bool RendererUtils::writeRgbaPixelsToPngFile(const char* filename,
                                             std::span<const uint8_t> rgbaPixels, int width,
                                             int height) {
  struct Context {
    std::ofstream output;
  };

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
      &context, width, height, 4, rgbaPixels.data(), width * 4);

  return context.output.good();
}

}  // namespace donner
