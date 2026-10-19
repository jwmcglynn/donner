/**
 * @example svg_animation.cc Drive declarative SVG animation from a host clock
 * @details Parses an SVG with `<animate>` and `<set>` elements, renders frames at chosen document
 * times, starts a click-triggered animation, and lists the animation lifecycle boundaries. The
 * program exits non-zero if the click starts no animation or the lifecycle boundaries differ from
 * the ones the document declares.
 *
 * ```sh
 * bazel run //examples:svg_animation
 * ```
 */

#include <cstddef>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

int main() {
  //! [animated_svg]
  // The left square fades from red to blue over two seconds and keeps its final color. The right
  // square turns green for one second after it is clicked.
  constexpr std::string_view kSvg = R"(
    <svg xmlns="http://www.w3.org/2000/svg" width="64" height="32">
      <rect width="32" height="32" fill="red">
        <animate id="fade" attributeName="fill" from="red" to="blue" dur="2s" fill="freeze" />
      </rect>
      <rect id="button" x="32" width="32" height="32" fill="gray">
        <set attributeName="fill" to="green" begin="button.click" dur="1s" />
      </rect>
    </svg>)";
  //! [animated_svg]

  donner::ParseWarningSink warnings;
  donner::ParseResult<donner::svg::SVGDocument> maybeDocument =
      donner::svg::parser::SVGParser::ParseSVG(kSvg, warnings);
  if (maybeDocument.hasError()) {
    std::cerr << "Parse Error: " << maybeDocument.error() << "\n";
    return 1;
  }
  donner::svg::SVGDocument document = std::move(maybeDocument.result());
  donner::svg::Renderer renderer;

  //! [set_time]
  // Donner has no clock of its own: the host chooses each frame's document time, in seconds.
  // Each draw() samples the animations at the document's current time.
  for (const double seconds : {0.0, 1.0, 3.0}) {
    document.setTime(seconds);
    renderer.draw(document);
    donner::svg::RendererBitmap frame = renderer.takeSnapshot();
    std::cout << "t=" << document.currentTime() << "s: " << frame.dimensions.x << "x"
              << frame.dimensions.y << " frame\n";
  }
  //! [set_time]

  //! [dispatch_event]
  // Pointer input stays with the host. Deliver a hit element's events to begin="id.click"
  // triggers; the triggered interval starts at the current document time.
  std::optional<donner::svg::SVGElement> button = document.querySelector("#button");
  if (!button || !document.dispatchMouseEvent(donner::svg::EventType::Click, *button)) {
    std::cerr << "The click did not start an animation\n";
    return 1;
  }
  //! [dispatch_event]

  //! [lifecycle_events]
  // Lifecycle boundaries in the interval (from, to], for hosts that react to begin, repeat and
  // end without polling every frame.
  donner::svg::SVGAnimationEventBatch batch = document.animationEvents(0.0, 5.0);
  for (const donner::svg::SVGAnimationEvent& event : batch.events) {
    const char* type = event.type == donner::svg::SVGAnimationEvent::Type::Begin    ? "begin"
                       : event.type == donner::svg::SVGAnimationEvent::Type::Repeat ? "repeat"
                                                                                    : "end";
    std::cout << type << " at " << event.time << "s\n";
  }
  //! [lifecycle_events]
  // The fade ends at 2s; the clicked square's interval runs from 3s to 4s.
  const std::vector<std::pair<donner::svg::SVGAnimationEvent::Type, double>> expected = {
      {donner::svg::SVGAnimationEvent::Type::End, 2.0},
      {donner::svg::SVGAnimationEvent::Type::Begin, 3.0},
      {donner::svg::SVGAnimationEvent::Type::End, 4.0},
  };
  bool matches = batch.complete && batch.events.size() == expected.size();
  for (std::size_t i = 0; matches && i < expected.size(); ++i) {
    matches =
        batch.events[i].type == expected[i].first && batch.events[i].time == expected[i].second;
  }
  if (!matches) {
    std::cerr << "Unexpected animation lifecycle boundaries\n";
    return 1;
  }

  //! [restart]
  // Return to time zero and discard event-started intervals, as when replaying from the start.
  document.restartAnimations();
  //! [restart]

  // The click-started interval is gone after the restart; only the authored fade remains.
  const donner::svg::SVGAnimationEventBatch afterRestart = document.animationEvents(0.0, 5.0);
  if (!afterRestart.complete || afterRestart.events.size() != 1 || document.currentTime() != 0.0) {
    std::cerr << "Restart did not return to the authored timeline\n";
    return 1;
  }
  renderer.draw(document);
  return 0;
}
