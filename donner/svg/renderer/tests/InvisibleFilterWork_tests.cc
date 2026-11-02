/// @file
/// Filters on rendered but invisible content must not run or spend the frame's filter budget, and a
/// graph the budget refuses must not take later, smaller graphs down with it.

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/SVG.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"

namespace donner::svg {
namespace {

using test::PixelAt;
using ::testing::Gt;
using ::testing::Lt;

SVGDocument Parse(std::string_view source) {
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(source, warnings);
  EXPECT_FALSE(parsed.hasError());
  return std::move(parsed.result());
}

RendererBitmap Tight(RendererBitmap snapshot) {
  const size_t tightRowBytes = static_cast<size_t>(snapshot.dimensions.x) * 4;
  if (snapshot.rowBytes == tightRowBytes) {
    return snapshot;
  }
  RendererBitmap tight;
  tight.dimensions = snapshot.dimensions;
  tight.rowBytes = tightRowBytes;
  tight.alphaType = snapshot.alphaType;
  tight.pixels.resize(tightRowBytes * static_cast<size_t>(snapshot.dimensions.y));
  for (int y = 0; y < snapshot.dimensions.y; ++y) {
    std::copy_n(snapshot.pixels.begin() + static_cast<std::ptrdiff_t>(y * snapshot.rowBytes),
                tightRowBytes,
                tight.pixels.begin() + static_cast<std::ptrdiff_t>(y * tightRowBytes));
  }
  return tight;
}

struct Frame {
  RendererBitmap bitmap;
  RendererResourceStats stats;
};

Frame Draw(std::string_view source) {
  SVGDocument document = Parse(source);
  Renderer renderer;
  renderer.draw(document);
  return {Tight(renderer.takeSnapshot()), renderer.resourceStats()};
}

/// A filter whose estimated work over a 128x128 frame is about an eighth of the frame's filter work
/// budget, so eight of them exhaust it.
constexpr std::string_view kHeavyFilter =
    "<filter id='heavy' filterUnits='userSpaceOnUse' x='0' y='0' width='128' height='128'>"
    "<feConvolveMatrix order='64' kernelMatrix='1'/></filter>";

/// A cheap visible filter: a blurred blue square over the bottom-right quarter.
constexpr std::string_view kVisibleFilter =
    "<filter id='soft' filterUnits='userSpaceOnUse' x='64' y='64' width='64' height='64'>"
    "<feGaussianBlur stdDeviation='1'/></filter>";

std::string HeavyShapes(int count) {
  std::string shapes;
  for (int i = 0; i < count; ++i) {
    shapes += "<rect x='" + std::to_string(i) +
              "' width='8' height='8' fill='#d02020' "
              "filter='url(#heavy)'/>";
  }
  return shapes;
}

std::string Document(std::string_view hiddenContent) {
  return "<svg xmlns='http://www.w3.org/2000/svg' width='128' height='128'><defs>" +
         std::string(kHeavyFilter) + std::string(kVisibleFilter) + "</defs>" +
         "<rect width='128' height='128' fill='#ffffff'/>" + std::string(hiddenContent) +
         "<rect x='72' y='72' width='48' height='48' fill='#2050d0' filter='url(#soft)'/></svg>";
}

/// A convolution kernel with a single tap keeps its input, so the matrix needs one entry per tap.
std::string Kernel() {
  std::string values = "1";
  for (int i = 1; i < 64 * 64; ++i) {
    values += " 0";
  }
  return values;
}

std::string WithKernel(std::string source) {
  const std::string placeholder = "kernelMatrix='1'";
  const size_t at = source.find(placeholder);
  if (at != std::string::npos) {
    source.replace(at, placeholder.size(), "kernelMatrix='" + Kernel() + "'");
  }
  return source;
}

void ExpectVisibleFilterDrawn(const Frame& frame) {
  EXPECT_THAT(PixelAt(frame.bitmap, 96, 96), test::Rgba(Lt(80), Lt(120), Gt(180), 255));
}

/// Hidden filtered content, in each way content can be rendered yet invisible, runs no filter and
/// leaves the visible filter drawn after it intact.
TEST(InvisibleFilterWorkTest, HiddenFilteredContentRunsNoFilters) {
  const std::string shapes = HeavyShapes(12);
  const std::string hidden[] = {
      "<g opacity='0'>" + shapes + "</g>",
      "<g opacity='0'><g opacity='0.5'><g>" + shapes + "</g></g></g>",
      "<g visibility='hidden'>" + shapes + "</g>",
      "<g style='opacity: 0'>" + shapes + "</g>",
      // Content drawn for a hidden leaf comes after the leaf in draw order, outside its subtree.
      "<defs><mask id='hiddenMask'>" + shapes +
          "</mask></defs><rect width='64' height='64' opacity='0' mask='url(#hiddenMask)'/>",
      "<defs><pattern id='hiddenTile' patternUnits='userSpaceOnUse' width='16' height='16'>" +
          shapes + "</pattern></defs><rect width='64' height='64' opacity='0' fill='url(#hiddenTile)'/>",
      "<defs><marker id='hiddenDot' markerWidth='8' markerHeight='8' "
      "markerUnits='userSpaceOnUse'>" +
          shapes +
          "</marker></defs><path d='M 4 4 L 40 4' stroke='#000000' opacity='0' "
          "marker-start='url(#hiddenDot)'/>",
      "<defs><mask id='hiddenLeafMask'>" + shapes +
          "</mask></defs><rect width='64' height='64' visibility='hidden' "
          "mask='url(#hiddenLeafMask)'/>",
  };
  const Frame expected = Draw(Document(""));
  ASSERT_FALSE(expected.bitmap.empty());
  ExpectVisibleFilterDrawn(expected);
  for (const std::string& content : hidden) {
    SCOPED_TRACE(content.substr(0, 40));
    const Frame actual = Draw(WithKernel(Document(content)));
    ExpectVisibleFilterDrawn(actual);
    editor::tests::CompareBitmapToBitmap(actual.bitmap, expected.bitmap, "invisible-filter-work",
                                         editor::tests::PixelmatchIdentityParams());
    if (actual.stats.filterBudgetSupported) {
      EXPECT_EQ(actual.stats.filterExecutions, expected.stats.filterExecutions);
      EXPECT_EQ(actual.stats.filterGraphsRefused, 0u);
      EXPECT_FALSE(actual.stats.filterBudgetRejected);
    }
  }
}

/// A visible graph that no longer fits the frame's remaining work is refused alone and counted;
/// the smaller graph drawn after it still runs.
TEST(InvisibleFilterWorkTest, RefusedGraphLeavesLaterGraphsRunning) {
  const Frame actual = Draw(WithKernel(Document(HeavyShapes(12))));
  ExpectVisibleFilterDrawn(actual);
  if (actual.stats.filterBudgetSupported) {
    EXPECT_THAT(actual.stats.filterGraphsRefused, Gt(0u));
    EXPECT_TRUE(actual.stats.filterBudgetRejected);
  }
}

/// Content shown through another element stays filtered even when its own definition sits in an
/// invisible container or is never rendered in place.
TEST(InvisibleFilterWorkTest, ReferencedContentStaysFiltered) {
  constexpr std::string_view kFlood =
      "<filter id='flood' filterUnits='userSpaceOnUse' x='0' y='0' width='128' height='128'>"
      "<feFlood flood-color='#d02020'/><feComposite in2='SourceAlpha' operator='in'/></filter>";
  const std::string head =
      "<svg xmlns='http://www.w3.org/2000/svg' width='128' height='128'>"
      "<defs>" +
      std::string(kFlood) +
      "<filter id='white' filterUnits='userSpaceOnUse' x='0' y='0' width='128' "
      "height='64'><feFlood flood-color='#ffffff'/></filter></defs>"
      "<rect width='128' height='128' fill='#ffffff'/>"
      // The definitions sit in an invisible container; content drawn for a visible element that
      // references them is visible.
      "<g opacity='0'>"
      "<pattern id='tile' patternUnits='userSpaceOnUse' width='16' "
      "height='16'><rect width='16' height='16' fill='#2050d0' "
      "filter='url(#flood)'/></pattern>"
      "<mask id='reveal'><rect width='8' height='8' fill='#000000' "
      "filter='url(#white)'/></mask>"
      "<marker id='dot' markerWidth='8' markerHeight='8' refX='4' refY='4' "
      "markerUnits='userSpaceOnUse'><rect width='8' height='8' "
      "fill='#2050d0' filter='url(#flood)'/></marker></g>";
  struct Case {
    std::string body;
    int x;
    int y;
  };
  const Case cases[] = {
      {"<g opacity='0'><rect id='sprite' width='16' height='16' fill='#2050d0' "
       "filter='url(#flood)'/></g><use href='#sprite' x='8' y='8'/>",
       16, 16},
      {"<rect x='40' y='0' width='32' height='32' fill='url(#tile)'/>", 48, 8},
      {"<path d='M 20 100 L 60 100' stroke='#000000' marker-start='url(#dot)'/>", 18, 97},
  };
  for (const Case& c : cases) {
    SCOPED_TRACE(c.body.substr(0, 40));
    const Frame frame = Draw(head + c.body + "</svg>");
    EXPECT_THAT(PixelAt(frame.bitmap, c.x, c.y), test::Rgba(Gt(180), Lt(80), Lt(80), 255));
  }

  // The mask's only content is black, so only its white flood reveals the masked shape.
  const Frame masked =
      Draw(head +
           "<rect x='80' y='0' width='40' height='120' fill='#20a020' mask='url(#reveal)'/></svg>");
  EXPECT_THAT(PixelAt(masked.bitmap, 100, 30), test::Rgba(Lt(80), Gt(120), Lt(80), 255));
  EXPECT_THAT(PixelAt(masked.bitmap, 100, 100), test::Rgba(255, 255, 255, 255));
}

/// A filter can paint without any source paint, so a shape with transparent fill and no stroke
/// still runs its filter.
TEST(InvisibleFilterWorkTest, TransparentPaintStillRunsItsFilter) {
  const Frame frame = Draw(
      "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'><defs>"
      "<filter id='fill' filterUnits='userSpaceOnUse' x='0' y='0' width='32' height='32'>"
      "<feFlood flood-color='#d02020'/></filter></defs>"
      "<rect width='32' height='32' fill='#ffffff'/>"
      "<rect width='8' height='8' fill='#2050d0' fill-opacity='0' filter='url(#fill)'/></svg>");
  EXPECT_THAT(PixelAt(frame.bitmap, 16, 16), test::Rgba(Gt(180), Lt(80), Lt(80), 255));
}

/// A hidden leaf draws nothing, so its filter does not run even when the filter would paint with
/// no source; browsers render nothing for it. A visible element inside a hidden group still draws
/// and still runs its filter.
TEST(InvisibleFilterWorkTest, HiddenLeafRunsNoFilterButVisibleChildOfHiddenGroupDoes) {
  constexpr std::string_view kHead =
      "<svg xmlns='http://www.w3.org/2000/svg' width='32' height='32'><defs>"
      "<filter id='fill' filterUnits='userSpaceOnUse' x='0' y='0' width='32' height='32'>"
      "<feFlood flood-color='#d02020'/></filter></defs>"
      "<rect width='32' height='32' fill='#ffffff'/>";
  const Frame hidden = Draw(std::string(kHead) +
                            "<rect width='8' height='8' fill='#2050d0' visibility='hidden' "
                            "filter='url(#fill)'/></svg>");
  EXPECT_THAT(PixelAt(hidden.bitmap, 16, 16), test::Rgba(255, 255, 255, 255));
  if (hidden.stats.filterBudgetSupported) {
    EXPECT_EQ(hidden.stats.filterExecutions, 0u);
  }

  const Frame shown = Draw(std::string(kHead) +
                           "<g visibility='hidden'><rect width='8' height='8' fill='#2050d0' "
                           "visibility='visible' filter='url(#fill)'/></g></svg>");
  EXPECT_THAT(PixelAt(shown.bitmap, 16, 16), test::Rgba(Gt(180), Lt(80), Lt(80), 255));
  if (shown.stats.filterBudgetSupported) {
    EXPECT_EQ(shown.stats.filterExecutions, 1u);
  }
}

}  // namespace
}  // namespace donner::svg
