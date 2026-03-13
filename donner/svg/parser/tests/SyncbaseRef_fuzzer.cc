#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

/// Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view input(reinterpret_cast<const char*>(data), size);

  // Limit input size to prevent excessive allocation inside the SVG parser.
  const size_t clampedSize = std::min(input.size(), size_t(256));

  // Build SVG where the fuzzed input is used as a begin attribute (syncbase reference).
  std::string svg = "<svg xmlns='http://www.w3.org/2000/svg'>"
                    "<rect id='r' width='100' height='100'>"
                    "<animate id='a1' attributeName='width' from='100' to='200' begin='0s' dur='1s' />"
                    "<animate attributeName='height' from='100' to='200' begin='";
  svg.append(input.substr(0, clampedSize));
  svg += "' dur='1s' />"
         "</rect></svg>";

  parser::SVGParser::Options options;
  options.enableExperimental = true;

  std::vector<ParseError> warnings;
  auto result = parser::SVGParser::ParseSVG(svg, &warnings, options);
  if (result.hasResult()) {
    auto& registry = result.result().registry();
    // Advance at a few time points to exercise syncbase resolution and animation timing.
    components::AnimationSystem().advance(registry, 0.5, nullptr);
    components::AnimationSystem().advance(registry, 1.0, nullptr);
    components::AnimationSystem().advance(registry, 2.0, nullptr);
  }

  return 0;
}

}  // namespace donner::svg
