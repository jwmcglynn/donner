#include <string_view>

#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  // Build SVG with the fuzzed value as an animateMotion path attribute.
  std::string svg = "<svg xmlns='http://www.w3.org/2000/svg'>"
                    "<circle id='c' cx='0' cy='0' r='5'>"
                    "<animateMotion path='";
  svg.append(input.substr(0, std::min(input.size(), size_t(512))));
  svg += "' begin='0s' dur='2s' />"
         "</circle></svg>";

  parser::SVGParser::Options options;
  options.enableExperimental = true;
  auto result = parser::SVGParser::ParseSVG(svg, nullptr, options);
  if (result.hasResult()) {
    auto& registry = result.result().registry();
    components::AnimationSystem().advance(registry, 0.5, nullptr);
    components::AnimationSystem().advance(registry, 1.0, nullptr);
    components::AnimationSystem().advance(registry, 2.0, nullptr);
  }

  return 0;
}

}  // namespace donner::svg
