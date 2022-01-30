#pragma once

#include <optional>

#include "src/base/box.h"
#include "src/base/length.h"
#include "src/base/transform.h"
#include "src/svg/components/registry.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/components/viewbox_component.h"

namespace donner {

/**
 * Stores an offset/size for elements that are positioned with x/y/width/height attributes with
 * respect to their parent. Used for <svg>, <image> and <foreignObject> by the standard, and also
 * internally with <use> for Donner.
 *
 * If not specified, x/y default to 0, and width/height are std::nullopt.
 */
struct SizedElementComponent {
  Lengthd x;
  Lengthd y;
  std::optional<Lengthd> width;
  std::optional<Lengthd> height;

  Vector2d calculatedSize(Registry& registry, Entity entity, Vector2d defaultRenderSize) const {
    // TODO: Confirm if this is the correct behavior if <svg> has a viewbox specifying a size, but
    // no width/height. For Ghostscript_Tiger to detect the size, we need to do this.
    if (const auto* viewbox = registry.try_get<ViewboxComponent>(entity);
        viewbox && !width && !height && viewbox->viewbox) {
      return viewbox->viewbox->size();
    } else {
      return Vector2d(width ? width->value : defaultRenderSize.x,
                      height ? height->value : defaultRenderSize.y);
    }
  }

  // TODO: Should this also call Lengthd::toPixels to convert units?
  Transformd computeTransform(Registry& registry, Entity entity, Vector2d defaultRenderSize) const {
    // TODO: A component with different behavior based on type seems like an
    // antipattern, perhaps this should be a separate component class?
    // If this entity also has a viewbox, this SizedElementComponent is used to define a viewport.
    if (const auto* viewbox = registry.try_get<ViewboxComponent>(entity)) {
      const Boxd initialSize(Vector2d(x.value, y.value),
                             calculatedSize(registry, entity, defaultRenderSize));
      return viewbox->computeTransform(initialSize);
    } else {
      if (width || height) {
        // TODO: Add scale, see viewbox_component as an example of how to do so.
        assert(false && "Not implemented");
      }

      // TODO: What happens if a <use> element has a transform attribute? Do these clobber?
      return Transformd::Translate(Vector2d(x.value, y.value));
    }
  }
};

}  // namespace donner
