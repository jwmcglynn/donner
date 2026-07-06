#include "donner/svg/properties/CssStringifier.h"

#include <gtest/gtest.h>

#include "donner/base/Length.h"
#include "donner/css/Color.h"
#include "donner/svg/core/Display.h"
#include "donner/svg/core/Visibility.h"
#include "donner/svg/properties/PaintServer.h"

namespace donner::svg {

using css::Color;
using css::RGBA;

// display: the CSS keyword, one per enumerator sampled.
TEST(CssStringifier, Display) {
  EXPECT_EQ(CssSerialize(Display::Inline), "inline");
  EXPECT_EQ(CssSerialize(Display::Block), "block");
  EXPECT_EQ(CssSerialize(Display::InlineBlock), "inline-block");
  EXPECT_EQ(CssSerialize(Display::None), "none");
}

// visibility: the CSS keyword.
TEST(CssStringifier, Visibility) {
  EXPECT_EQ(CssSerialize(Visibility::Visible), "visible");
  EXPECT_EQ(CssSerialize(Visibility::Hidden), "hidden");
  EXPECT_EQ(CssSerialize(Visibility::Collapse), "collapse");
}

// opacity / fill-opacity / stroke-opacity: a bare number, minimal precision.
TEST(CssStringifier, Number) {
  EXPECT_EQ(CssSerialize(1.0), "1");
  EXPECT_EQ(CssSerialize(0.0), "0");
  EXPECT_EQ(CssSerialize(0.5), "0.5");
  EXPECT_EQ(CssSerialize(0.25), "0.25");
}

// stroke-width and other lengths: canonical CSS length text.
TEST(CssStringifier, Length) {
  EXPECT_EQ(CssSerialize(Lengthd(1.0, Lengthd::Unit::None)), "1");
  EXPECT_EQ(CssSerialize(Lengthd(2.0, Lengthd::Unit::Px)), "2px");
  EXPECT_EQ(CssSerialize(Lengthd(50.0, Lengthd::Unit::Percent)), "50%");
  EXPECT_EQ(CssSerialize(Lengthd(1.5, Lengthd::Unit::Em)), "1.5em");
}

// color: currentColor keyword or lowercase hex, with an alpha suffix when
// translucent (never the debug `rgba(...)` form).
TEST(CssStringifier, ColorHexAndCurrentColor) {
  EXPECT_EQ(CssSerialize(Color(RGBA(0xFF, 0x00, 0x00, 0xFF))), "#ff0000");
  EXPECT_EQ(CssSerialize(Color(RGBA(0x00, 0x00, 0x00, 0xFF))), "#000000");
  EXPECT_EQ(CssSerialize(Color(RGBA(0x12, 0x34, 0x56, 0xFF))), "#123456");
  EXPECT_EQ(CssSerialize(Color(RGBA(0x12, 0x34, 0x56, 0x80))), "#12345680");
  EXPECT_EQ(CssSerialize(Color(Color::CurrentColor{})), "currentColor");
}

// paint: none / context keywords, a resolved color, and url() references.
TEST(CssStringifier, PaintServer) {
  EXPECT_EQ(CssSerialize(PaintServer(PaintServer::None())), "none");
  EXPECT_EQ(CssSerialize(PaintServer(PaintServer::ContextFill())), "context-fill");
  EXPECT_EQ(CssSerialize(PaintServer(PaintServer::ContextStroke())), "context-stroke");
  EXPECT_EQ(CssSerialize(PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0x00, 0x00, 0xFF))))),
            "#ff0000");
  EXPECT_EQ(CssSerialize(PaintServer(PaintServer::ElementReference("#grad"))), "url(#grad)");
  EXPECT_EQ(CssSerialize(PaintServer(
                PaintServer::ElementReference("#grad", Color(RGBA(0x00, 0xFF, 0x00, 0xFF))))),
            "url(#grad) #00ff00");
}

}  // namespace donner::svg
