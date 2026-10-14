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

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  const bool limitMarker = input.starts_with("animation-list-budget");
  std::string generated;
  if (limitMarker) {
    generated.reserve((parser::ListParser::kMaximumItems + 1) * 2);
    for (std::size_t i = 0; i <= parser::ListParser::kMaximumItems; ++i) {
      generated += "1;";
    }
  }
  const std::string_view values = limitMarker
                                      ? std::string_view(generated)
                                      : input.substr(0, std::min(input.size(), size_t(512)));

  struct Target {
    std::string_view prefix;
    std::string_view element;
    std::string_view attribute;
  };
  constexpr std::array<Target, 13> targets = {{
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
  Target target{"", "rect", "width"};
  for (const auto& candidate : targets) {
    if (input.starts_with(candidate.prefix)) {
      target = candidate;
      break;
    }
  }
  std::string svg = "<svg xmlns='http://www.w3.org/2000/svg'><";
  svg += target.element;
  svg += " id='r'><animate id='budget' attributeName='";
  svg += target.attribute;
  svg += target.prefix == "reference-to:" ? "' to='"
         : target.prefix == "font-by:"    ? "' by='"
                                          : "' values='";
  svg.append(values.substr(target.prefix.size()));
  svg += "' begin='0s' dur='2s'";
  if (target.prefix.starts_with("filter-") || target.attribute == "font-size") {
    svg += " additive='sum' accumulate='sum' repeatCount='3'";
  }
  svg += "/></";
  svg += target.element;
  svg += "></svg>";

  parser::SVGParser::Options options;

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(svg, warnings, options);
  if (result.hasResult()) {
    auto& registry = result.result().registry();
    if (limitMarker) {
      const auto element = result.result().querySelector("#budget");
      const auto* component =
          element.has_value() ? element->entityHandle().try_get<components::AnimateValueComponent>()
                              : nullptr;
      if (component == nullptr || !component->values.empty()) {
        std::abort();
      }
    }
    // Advance at a few time points to exercise interpolation.
    components::AnimationSystem().advance(registry, 0.5, nullptr);
    components::AnimationSystem().advance(registry, 1.0, nullptr);
    if (target.prefix.starts_with("filter-") || target.attribute == "font-size") {
      components::AnimationSystem().advance(registry, 3.0, nullptr);
    }
    if (target.attribute == "font-size") {
      registry.ctx().emplace<components::AnimationStyleContext>().enabled = true;
      components::StyleSystem().computeAllStyles(registry, warnings);
    } else if (const auto element = result.result().querySelector("#r")) {
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

  return 0;
}

}  // namespace donner::svg
