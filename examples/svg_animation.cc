/**
 * @example svg_animation.cc Drive declarative SVG animation from a host clock
 * @details Parses an SVG with `<animate>` and `<set>` elements, renders frames at chosen document
 * times, starts a click-triggered animation, and lists the animation lifecycle boundaries. The
 * program checks the rendered colors and exits non-zero if any of them is unexpected.
 *
 * ```sh
 * bazel run //examples:svg_animation
 * ```
 */

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <string_view>
#include <utility>

#include "donner/base/ParseResult.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"

namespace {

/// Straight-alpha RGBA color of one rendered pixel.
struct Rgba {
  std::uint8_t r, g, b, a;
};

Rgba PixelAt(const donner::svg::RendererBitmap& bitmap, int x, int y) {
  const std::size_t offset =
      static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4;
  return {bitmap.pixels[offset], bitmap.pixels[offset + 1], bitmap.pixels[offset + 2],
          bitmap.pixels[offset + 3]};
}

/// Renders the document at its current time and returns one pixel of the result.
Rgba RenderPixel(donner::svg::Renderer& renderer, donner::svg::SVGDocument& document, int x,
                 int y) {
  renderer.draw(document);
  return PixelAt(renderer.takeSnapshot(), x, y);
}

bool Expect(std::string_view what, Rgba actual, Rgba expected) {
  const auto near = [](int a, int b) { return a - b <= 2 && b - a <= 2; };
  const bool ok = near(actual.r, expected.r) && near(actual.g, expected.g) &&
                  near(actual.b, expected.b) && near(actual.a, expected.a);
  std::cout << what << ": rgba(" << int(actual.r) << ", " << int(actual.g) << ", " << int(actual.b)
            << ", " << int(actual.a) << ")" << (ok ? "" : "  UNEXPECTED") << "\n";
  return ok;
}

}  // namespace

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
  bool ok = true;

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

  document.setTime(0.0);
  ok &= Expect("fade at 0s", RenderPixel(renderer, document, 8, 8), {255, 0, 0, 255});
  document.setTime(1.0);
  ok &= Expect("fade at 1s", RenderPixel(renderer, document, 8, 8), {128, 0, 128, 255});
  document.setTime(3.0);
  ok &= Expect("fade at 3s, frozen", RenderPixel(renderer, document, 8, 8), {0, 0, 255, 255});

  //! [dispatch_event]
  // Pointer input stays with the host. Deliver a hit element's events to begin="id.click"
  // triggers; the triggered interval starts at the current document time.
  std::optional<donner::svg::SVGElement> button = document.querySelector("#button");
  if (!button || !document.dispatchMouseEvent(donner::svg::EventType::Click, *button)) {
    std::cerr << "The click did not start an animation\n";
    return 1;
  }
  //! [dispatch_event]

  ok &= Expect("button at 3s", RenderPixel(renderer, document, 40, 8), {0, 128, 0, 255});
  document.setTime(4.5);
  ok &= Expect("button at 4.5s", RenderPixel(renderer, document, 40, 8), {128, 128, 128, 255});

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
  if (!batch.complete) {
    std::cerr << "The lifecycle query was incomplete\n";
    return 1;
  }

  //! [restart]
  // Return to time zero and discard event-started intervals, as when replaying from the start.
  document.restartAnimations();
  //! [restart]
  ok &=
      Expect("button after restart", RenderPixel(renderer, document, 40, 8), {128, 128, 128, 255});

  return ok ? 0 : 1;
}
