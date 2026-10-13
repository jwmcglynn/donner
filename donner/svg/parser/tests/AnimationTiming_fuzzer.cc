#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

/// Exercise timing refresh, dependency expansion, seeking, and bounded lifecycle queries.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Byte input is borrowed for this fuzz invocation.
  const std::string_view input(reinterpret_cast<const char*>(data), std::min(size, size_t(4096)));
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect id="r">
      <animate id="a" attributeName="width" from="0" to="100" dur="1s"/>
      <set id="b" attributeName="fill" to="red" begin="a.end" dur="1s"/>
    </rect></svg>)svg",
                                            warnings);
  if (!parsed.hasResult()) {
    return 0;
  }
  auto& document = parsed.result();
  auto animation = document.querySelector("#a");
  if (!animation) {
    return 0;
  }
  animation->setAttribute("begin", input);
  animation->setAttribute("end", input);
  for (double time : {-1.0, 0.0, 0.5, 2.0}) {
    components::AnimationSystem().advance(document.registry(), time);
  }
  animation->setAttribute("begin", "1s");
  animation->setAttribute("dur", input);
  animation->setAttribute("repeatCount", "indefinite");
  (void)document.animationEvents(1, 2);
  return 0;
}

}  // namespace donner::svg
