#include <gtest/gtest.h>

#include <string>
#include <string_view>

#include "donner/svg/SVGElement.h"
#include "donner/svg/renderer/RendererTinySkia.h"
#include "donner/svg/tests/ParserTestUtils.h"

/// @file
/// Steady-state pins for the tiny-skia backend's per-entity conversion caches.
///
/// The caches exist so that re-rendering an unchanged document does not redo work that only
/// document state can invalidate. That property is invisible in output pixels and easy to lose
/// to an innocuous refactor (a lost cache lookup, a key that never matches, a listener that
/// fires too eagerly), and it would show up only as a gradual frame-time regression. Counting
/// the conversions turns it into an assertion.

namespace donner::svg {
namespace {

/// A 2x2 solid red PNG as a data URI, so the image loads with no external resources.
constexpr std::string_view kRedImageDataUri =
    "data:image/png;base64,"
    "iVBORw0KGgoAAAANSUhEUgAAAAIAAAACCAIAAAD91JpzAAAAEElEQVR4nGP4z8AARAwQCgAf7gP9i18U1AAAAABJRU5E"
    "rkJggg==";

/// Several shapes plus one image, so both counters have work to do.
std::string ShapesAndImageMarkup() {
  return std::string(R"(<g>
      <path id="p" d="M 0 0 L 6 0 L 6 6 Z" fill="black"/>
      <rect x="1" y="1" width="4" height="4" fill="red"/>
      <circle cx="8" cy="8" r="3" fill="blue"/>
      <image id="i" x="8" y="0" width="8" height="8" href=")") +
         std::string(kRedImageDataUri) + R"(" />
    </g>)";
}

SVGDocument MakeDocument() {
  return instantiateSubtree(ShapesAndImageMarkup(), {}, Vector2i(16, 16));
}

TEST(RendererTinySkiaPerfTests, SettledFrameConvertsNoPathsAndPremultipliesNoImages) {
  SVGDocument document = MakeDocument();

  RendererTinySkia renderer;
  renderer.draw(document);

  // Sanity: the first frame really does the work, so a zero on the second frame means "served
  // from cache" rather than "this counter never moves".
  const RendererTinySkiaFrameCounters first = renderer.frameCounters();
  EXPECT_GT(first.pathConversions, 0u);
  EXPECT_GT(first.imagePremultiplies, 0u);

  renderer.draw(document);

  const RendererTinySkiaFrameCounters second = renderer.frameCounters();
  EXPECT_EQ(second.pathConversions, 0u)
      << "a settled frame re-converted " << second.pathConversions
      << " path(s); every shape should have been served from its per-entity cache";
  EXPECT_EQ(second.imagePremultiplies, 0u)
      << "a settled frame re-premultiplied " << second.imagePremultiplies
      << " image(s); the payload should have been served from its per-entity cache";
}

// The other half of the contract: a cache that never refills would also report zero forever. A
// geometry change has to put the shape back on the conversion path for exactly one frame.
TEST(RendererTinySkiaPerfTests, MutatedPathReconvertsOnceThenSettlesAgain) {
  SVGDocument document = MakeDocument();

  RendererTinySkia renderer;
  renderer.draw(document);
  renderer.draw(document);
  ASSERT_EQ(renderer.frameCounters().pathConversions, 0u);

  std::optional<SVGElement> path = document.querySelector("#p");
  ASSERT_TRUE(path.has_value());
  path->setAttribute("d", "M 0 0 L 12 0 L 12 12 Z");

  renderer.draw(document);
  EXPECT_GT(renderer.frameCounters().pathConversions, 0u)
      << "the mutated shape must be re-converted, not served from the pre-mutation cache";
  // The image was untouched, so it must not be dragged along by the path invalidation.
  EXPECT_EQ(renderer.frameCounters().imagePremultiplies, 0u);

  renderer.draw(document);
  EXPECT_EQ(renderer.frameCounters().pathConversions, 0u)
      << "the frame after a mutation should settle back to zero conversions";
}

}  // namespace
}  // namespace donner::svg
