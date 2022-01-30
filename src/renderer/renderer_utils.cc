#include "src/renderer/renderer_utils.h"

#include <stb/stb_image_write.h>

#include <fstream>

#include "src/svg/components/circle_component.h"
#include "src/svg/components/computed_shadow_tree_component.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/document_context.h"
#include "src/svg/components/path_component.h"
#include "src/svg/components/rect_component.h"
#include "src/svg/components/registry.h"
#include "src/svg/components/shadow_tree_component.h"
#include "src/svg/components/sized_element_component.h"
#include "src/svg/components/transform_component.h"
#include "src/svg/components/tree_component.h"

namespace donner {

void RendererUtils::prepareDocumentForRendering(SVGDocument& document, Vector2d defaultSize) {
  Registry& registry = document.registry();
  registry.ctx<DocumentContext>().defaultSize = defaultSize;

  for (auto view = registry.view<SizedElementComponent>(); auto entity : view) {
    auto [sizedElement] = view.get(entity);

    registry.emplace_or_replace<ViewboxTransformComponent>(
        entity, sizedElement.computeTransform(registry, entity, defaultSize));
  }

  // Instantiate shadow trees.
  for (auto view = registry.view<ShadowTreeComponent>(); auto entity : view) {
    auto [shadowTreeComponent] = view.get(entity);
    if (auto targetEntity = shadowTreeComponent.targetEntity(registry);
        targetEntity != entt::null) {
      registry.get_or_emplace<ComputedShadowTreeComponent>(entity).instantiate(registry,
                                                                               targetEntity);
    } else {
      std::cerr << "Warning: Failed to resolve shadow tree target with href '"
                << shadowTreeComponent.href() << "'." << std::endl;
    }
  }

  // Create placeholder ComputedStyleComponents for all elements in the tree.
  for (auto view = registry.view<TreeComponent>(); auto entity : view) {
    // TODO: Can this be done in one step, or do the two loops need to be separate?
    std::ignore = registry.get_or_emplace<ComputedStyleComponent>(entity);
  }

  // Compute the styles for all elements.
  for (auto view = registry.view<ComputedStyleComponent>(); auto entity : view) {
    auto [styleComponent] = view.get(entity);
    styleComponent.computeProperties(registry, entity);
  }

  // Then compute all paths.
  for (auto view = registry.view<RectComponent, ComputedStyleComponent>(); auto entity : view) {
    auto [rect, style] = view.get(entity);
    rect.computePath(registry.get_or_emplace<ComputedPathComponent>(entity), style.viewbox(),
                     FontMetrics());
  }

  for (auto view = registry.view<CircleComponent, ComputedStyleComponent>(); auto entity : view) {
    auto [circle, style] = view.get(entity);
    circle.computePath(registry.get_or_emplace<ComputedPathComponent>(entity), style.viewbox(),
                       FontMetrics());
  }
}

bool RendererUtils::writeRgbaPixelsToPngFile(const char* filename,
                                             std::span<const uint8_t> rgbaPixels, int width,
                                             int height) {
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
      &context, width, height, 4, rgbaPixels.data(), width * 4);

  return context.output.good();
}

}  // namespace donner
