#pragma once

#include <optional>

#include "src/base/box.h"
#include "src/base/length.h"
#include "src/base/transform.h"
#include "src/svg/components/computed_style_component.h"
#include "src/svg/components/preserve_aspect_ratio_component.h"
#include "src/svg/components/registry.h"
#include "src/svg/components/tree_component.h"
#include "src/svg/components/viewbox_component.h"
#include "src/svg/properties/presentation_attribute_parsing.h"

namespace donner::svg {

/**
 * Stores an offset/size for elements that are positioned with x/y/width/height attributes with
 * respect to their parent. Used for <svg>, <image> and <foreignObject> by the standard, and also
 * internally with <use> for Donner.
 *
 * If not specified, x/y default to 0, and width/height are std::nullopt.
 */
struct SizedElementProperties {
  Property<Lengthd> x{"x",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> y{"y",
                      []() -> std::optional<Lengthd> { return Lengthd(0, Lengthd::Unit::None); }};
  Property<Lengthd> width{"width", []() -> std::optional<Lengthd> { return std::nullopt; }};
  Property<Lengthd> height{"height", []() -> std::optional<Lengthd> { return std::nullopt; }};

  auto allProperties() { return std::forward_as_tuple(x, y, width, height); }
};

struct ComputedSizedElementComponent {
  ComputedSizedElementComponent(EntityHandle handle, SizedElementProperties properties,
                                const std::map<RcString, UnparsedProperty>& unparsedProperties,
                                Boxd inheritedViewbox, FontMetrics fontMetrics,
                                std::vector<ParseError>* outWarnings);

  Boxd bounds;
  Boxd inheritedViewbox;

  Transformd computeTransform(EntityHandle handle) const {
    // TODO: A component with different behavior based on type seems like an
    // antipattern, perhaps this should be a separate component class?
    // If this entity also has a viewbox, this SizedElementComponent is used to define a viewport.

    if (const auto* viewbox = handle.try_get<ViewboxComponent>()) {
      return viewbox->computeTransform(
          bounds, handle.get<PreserveAspectRatioComponent>().preserveAspectRatio);
    } else {
      PreserveAspectRatio preserveAspectRatio;
      if (const auto* preserveAspectRatioComponent =
              handle.try_get<PreserveAspectRatioComponent>()) {
        preserveAspectRatio = preserveAspectRatioComponent->preserveAspectRatio;
      }

      Vector2d scale = bounds.size() / inheritedViewbox.size();

      if (preserveAspectRatio.align != PreserveAspectRatio::Align::None) {
        if (preserveAspectRatio.meetOrSlice == PreserveAspectRatio::MeetOrSlice::Meet) {
          scale.x = scale.y = std::min(scale.x, scale.y);
        } else {
          scale.x = scale.y = std::max(scale.x, scale.y);
        }
      }

      Vector2d translation = bounds.top_left - (inheritedViewbox.top_left * scale);
      const Vector2d alignMaxOffset = bounds.size() - inheritedViewbox.size() * scale;

      const Vector2d alignMultiplier(preserveAspectRatio.alignMultiplierX(),
                                     preserveAspectRatio.alignMultiplierY());
      return Transformd::Scale(scale) *
             Transformd::Translate(translation + alignMaxOffset * alignMultiplier);
    }
  }
};

struct SizedElementComponent {
  SizedElementProperties properties;

  void computeWithPrecomputedStyle(EntityHandle handle, const ComputedStyleComponent& style,
                                   FontMetrics fontMetrics, std::vector<ParseError>* outWarnings);

  void compute(EntityHandle handle) {
    ComputedStyleComponent& style = handle.get_or_emplace<ComputedStyleComponent>();
    style.computeProperties(handle);

    return computeWithPrecomputedStyle(handle, style, FontMetrics(), nullptr);
  }
};

}  // namespace donner::svg
