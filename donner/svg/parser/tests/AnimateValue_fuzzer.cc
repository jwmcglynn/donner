#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/animation/AnimateValueComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationBaseValue.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/ListParser.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {
namespace {
struct Target {
  std::string_view prefix;
  std::string_view element;
  std::string_view attribute;
};
constexpr std::array<Target, 28> kTargets = {{
    {"path:", "path", "d"},
    {"path-by:", "path", "d"},
    {"path-from-by:", "path", "d"},
    {"path-add:", "path", "d"},
    {"fill:", "rect", "fill"},
    {"stroke:", "rect", "stroke"},
    {"color:", "rect", "color"},
    {"stop-color:", "stop", "stop-color"},
    {"flood-color:", "feFlood", "flood-color"},
    {"lighting-color:", "feDiffuseLighting", "lighting-color"},
    {"miter:", "rect", "stroke-miterlimit"},
    {"opacity:", "rect", "opacity"},
    {"stop-opacity:", "stop", "stop-opacity"},
    {"stop-offset:", "stop", "offset"},
    {"function-offset:", "feFuncR", "offset"},
    {"font:", "text", "font-size"},
    {"font-by:", "text", "font-size"},
    {"filter-x:", "feFlood", "x"},
    {"filter-width:", "feFlood", "width"},
    {"marker:", "marker", "markerWidth"},
    {"marker-ref:", "marker", "refX"},
    {"text-offset:", "textPath", "startOffset"},
    {"pair:", "feGaussianBlur", "stdDeviation"},
    {"integer:", "feTurbulence", "numOctaves"},
    {"order:", "feConvolveMatrix", "order"},
    {"input:", "feOffset", "in"},
    {"reference:", "use", "href"},
    {"reference-to:", "use", "href"},
}};
Target FindTarget(std::string_view input) {
  Target target{"", "rect", "width"};
  for (const auto& candidate : kTargets) {
    if (input.starts_with(candidate.prefix)) {
      target = candidate;
      break;
    }
  }
  return target;
}

std::string MakeOversizedValueList() {
  std::string generated;
  generated.reserve((parser::ListParser::kMaximumItems + 1) * 2);
  for (std::size_t i = 0; i <= parser::ListParser::kMaximumItems; ++i) {
    generated += "1;";
  }
  return generated;
}

bool UsesContextualStyle(const Target& target) {
  return target.attribute == "font-size" ||
         components::IsContextualScalarAttribute(target.attribute) ||
         components::IsContextualPaintAttribute(target.attribute);
}

std::string MakeFuzzSvg(const Target& target, std::string_view values, bool repeatedComposition) {
  std::string svg = "<svg xmlns='http://www.w3.org/2000/svg' color='red' fill='blue'><";
  svg += target.element;
  if (target.attribute == "d") {
    svg += " d='M0 0L1 1'";
  }
  svg += " id='r'><animate id='budget'";
  if (target.prefix == "path-from-by:") {
    svg += " from='M0 0L1 1'";
  }
  svg += " attributeName='";
  svg += target.attribute;
  const bool by = target.prefix == "font-by:" || target.prefix == "path-by:" ||
                  target.prefix == "path-from-by:";
  svg += target.prefix == "reference-to:" ? "' to='" : by ? "' by='" : "' values='";
  svg.append(values.substr(target.prefix.size()));
  svg += "' begin='0s' dur='2s'";
  if (repeatedComposition) {
    svg += " additive='sum' accumulate='sum' repeatCount='3'";
  }
  svg += "/></";
  svg += target.element;
  svg += "></svg>";
  return svg;
}

void CheckRejectedValueList(SVGDocument& document) {
  const auto element = document.querySelector("#budget");
  const auto* component = element.has_value()
                              ? element->entityHandle().try_get<components::AnimateValueComponent>()
                              : nullptr;
  if (component == nullptr || !component->values.empty()) {
    std::abort();
  }
}

void ResolveTargetContext(SVGDocument& document, const Target& target) {
  auto& registry = document.registry();
  if (const auto element = document.querySelector("#r")) {
    components::AnimationValueContext context;
    for (const double extent : {1.0, 100.0}) {
      context.viewBox = Box2d::FromXYWH(0, 0, extent, extent);
      if (target.prefix.starts_with("filter-")) {
        context.mappedLengths = true;
        context.unitlessScale = extent;
        context.unitlessOrigin = target.attribute == "x" ? 4 : 0;
        context.percentageOrigin = context.unitlessOrigin;
      }
      (void)components::ResolveContextualLength(registry, element->entityHandle().entity(),
                                                target.attribute, Lengthd(3), context);
    }
  }
}

}  // namespace

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view input(reinterpret_cast<const char*>(data), size);
  const bool limitMarker = input.starts_with("animation-list-budget");
  const std::string generated = limitMarker ? MakeOversizedValueList() : std::string();
  const std::string_view values = limitMarker
                                      ? std::string_view(generated)
                                      : input.substr(0, std::min(input.size(), size_t(512)));
  const Target target = FindTarget(input);
  const bool contextualStyle = UsesContextualStyle(target);
  const bool repeatedComposition =
      target.prefix.starts_with("filter-") || contextualStyle || target.prefix == "path-add:";
  const std::string svg = MakeFuzzSvg(target, values, repeatedComposition);
  parser::SVGParser::Options options;
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(svg, warnings, options);
  if (result.hasResult()) {
    auto& document = result.result();
    auto& registry = document.registry();
    if (limitMarker) {
      CheckRejectedValueList(document);
    }
    // Advance at a few time points to exercise interpolation.
    components::AnimationSystem().advance(registry, 0.5, nullptr);
    components::AnimationSystem().advance(registry, 1.0, nullptr);
    if (repeatedComposition) {
      components::AnimationSystem().advance(registry, 3.0, nullptr);
    }
    if (contextualStyle) {
      registry.ctx().emplace<components::AnimationStyleContext>().enabled = true;
      components::StyleSystem().computeAllStyles(registry, warnings);
    } else {
      ResolveTargetContext(document, target);
    }
  }
  return 0;
}

}  // namespace donner::svg
