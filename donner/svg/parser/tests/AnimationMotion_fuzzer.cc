#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg {

namespace {

void ConfigureMotionAttribute(SVGElement& animation, std::string_view name) {
  if (name == "values") {
    animation.removeAttribute("path");
  }
  if (name == "keyPoints" || name == "keyTimes") {
    animation.setAttribute("calcMode", "linear");
    animation.setAttribute("keyPoints", "0;0.5;1");
    animation.setAttribute("keyTimes", "0;0.5;1");
  }
  if (name == "keySplines") {
    animation.setAttribute("calcMode", "spline");
    animation.setAttribute("keyPoints", "0;1");
    animation.setAttribute("keyTimes", "0;1");
  }
}

}  // namespace

/// Exercise motion paths and keyframe parsers through authored attribute updates.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size == 0) {
    return 0;
  }
  static constexpr std::array<std::string_view, 6> attributes = {
      "path", "values", "rotate", "keyPoints", "keyTimes", "keySplines"};
  const auto attribute = attributes[data[0] % attributes.size()];
  // NOLINTNEXTLINE: Byte input is borrowed for this fuzz invocation.
  const std::string_view input(reinterpret_cast<const char*>(data + 1),
                               std::min(size - 1, size_t(4096)));
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(R"svg(
    <svg xmlns="http://www.w3.org/2000/svg"><rect>
      <animateMotion id="a" path="M0 0L20 20" dur="2s"/>
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
  ConfigureMotionAttribute(*animation, attribute);
  animation->setAttribute(attribute, input);
  for (double time : {0.0, 0.5, 1.0, 2.0}) {
    components::AnimationSystem().advance(document.registry(), time);
  }
  return 0;
}

}  // namespace donner::svg
