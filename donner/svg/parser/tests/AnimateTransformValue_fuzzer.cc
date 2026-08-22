#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/animation/AnimateTransformComponent.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/parser/ListParser.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  const bool limitMarker = input.starts_with("animation-transform-list-budget");
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

  // Build SVG with the fuzzed values in an animateTransform element.
  std::string svg =
      "<svg xmlns='http://www.w3.org/2000/svg'>"
      "<rect id='r' width='100' height='100'>"
      "<animateTransform id='budget' attributeName='transform' type='rotate' values='";
  svg.append(values);
  svg +=
      "' begin='0s' dur='2s' />"
      "</rect></svg>";

  parser::SVGParser::Options options;
  options.enableExperimental = true;
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto result = parser::SVGParser::ParseSVG(svg, warnings, options);
  if (result.hasResult()) {
    auto& registry = result.result().registry();
    if (limitMarker) {
      const auto element = result.result().querySelector("#budget");
      const auto* component =
          element.has_value()
              ? element->entityHandle().try_get<components::AnimateTransformComponent>()
              : nullptr;
      if (component == nullptr || !component->values.empty()) {
        std::abort();
      }
    }
    components::AnimationSystem().advance(registry, 0.5, nullptr);
    components::AnimationSystem().advance(registry, 1.0, nullptr);
  }

  return 0;
}

}  // namespace donner::svg
