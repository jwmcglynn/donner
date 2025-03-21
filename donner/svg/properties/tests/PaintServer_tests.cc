#include "donner/svg/properties/PaintServer.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::svg {

using css::Color;
using css::RGBA;

TEST(PaintServer, Output) {
  EXPECT_EQ((std::ostringstream() << PaintServer(PaintServer::None())).str(), "PaintServer(none)");
  EXPECT_EQ((std::ostringstream() << PaintServer(PaintServer::ContextFill())).str(),
            "PaintServer(context-fill)");
  EXPECT_EQ((std::ostringstream() << PaintServer(PaintServer::ContextStroke())).str(),
            "PaintServer(context-stroke)");
  EXPECT_EQ((std::ostringstream() << PaintServer(PaintServer::Solid(Color(RGBA(0xFF, 0, 0, 0xFF)))))
                .str(),
            "PaintServer(solid rgba(255, 0, 0, 255))");
  EXPECT_EQ((std::ostringstream() << PaintServer(PaintServer::ElementReference("#test"))).str(),
            "PaintServer(url(#test))");
  EXPECT_EQ((std::ostringstream() << PaintServer(
                 PaintServer::ElementReference("#test", Color(RGBA(0, 0xFF, 0, 0xFF)))))
                .str(),
            "PaintServer(url(#test) rgba(0, 255, 0, 255))");
}

}  // namespace donner::svg
