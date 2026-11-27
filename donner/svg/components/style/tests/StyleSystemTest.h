#pragma once

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string_view>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg::components {

/// Shared parser and style computation fixture.
class StyleSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    ParseWarningSink parseSink;
    auto maybeResult = parser::SVGParser::ParseSVG(input, parseSink);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }

  SVGDocument ParseAndComputeStyles(std::string_view input) {
    auto document = ParseSVG(input);
    ParseWarningSink warningSink;
    styleSystem.computeAllStyles(document.registry(), warningSink);
    return document;
  }

  StyleSystem styleSystem;
};

}  // namespace donner::svg::components
