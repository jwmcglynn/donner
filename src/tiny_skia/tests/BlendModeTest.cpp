#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <optional>
#include <string_view>

#include "tiny_skia/BlendMode.h"

TEST(BlendModeTest, ShouldPreScaleCoverage) {
  struct CoverageCase {
    tiny_skia::BlendMode mode;
    bool expected;
    std::string_view label;
  };

  for (const auto& tc : {
           CoverageCase{tiny_skia::BlendMode::Clear, false, "clear"},
           CoverageCase{tiny_skia::BlendMode::Source, false, "source"},
           CoverageCase{tiny_skia::BlendMode::Destination, true, "destination"},
           CoverageCase{tiny_skia::BlendMode::SourceOver, true, "sourceOver"},
           CoverageCase{tiny_skia::BlendMode::DestinationOver, true, "destinationOver"},
           CoverageCase{tiny_skia::BlendMode::SourceIn, false, "sourceIn"},
           CoverageCase{tiny_skia::BlendMode::DestinationOut, true, "destinationOut"},
           CoverageCase{tiny_skia::BlendMode::SourceAtop, true, "sourceAtop"},
           CoverageCase{tiny_skia::BlendMode::Xor, true, "xor"},
           CoverageCase{tiny_skia::BlendMode::Plus, true, "plus"},
           CoverageCase{tiny_skia::BlendMode::Multiply, false, "multiply"},
       }) {
    SCOPED_TRACE(tc.label);
    EXPECT_THAT(tiny_skia::shouldPreScaleCoverage(tc.mode), testing::Eq(tc.expected));
  }
}

TEST(BlendModeTest, ToStageMapping) {
  struct StageCase {
    tiny_skia::BlendMode mode;
    std::optional<tiny_skia::pipeline::Stage> stage;
    std::string_view label;
  };

  for (const auto& tc : {
           StageCase{tiny_skia::BlendMode::Clear, tiny_skia::pipeline::Stage::Clear, "clear"},
           StageCase{tiny_skia::BlendMode::Source, std::nullopt, "source"},
           StageCase{tiny_skia::BlendMode::Destination,
                     tiny_skia::pipeline::Stage::MoveDestinationToSource, "destination"},
           StageCase{tiny_skia::BlendMode::SourceOver, tiny_skia::pipeline::Stage::SourceOver,
                     "sourceOver"},
           StageCase{tiny_skia::BlendMode::DestinationOver,
                     tiny_skia::pipeline::Stage::DestinationOver, "destinationOver"},
           StageCase{tiny_skia::BlendMode::SourceIn, tiny_skia::pipeline::Stage::SourceIn,
                     "sourceIn"},
           StageCase{tiny_skia::BlendMode::DestinationIn, tiny_skia::pipeline::Stage::DestinationIn,
                     "destinationIn"},
           StageCase{tiny_skia::BlendMode::SourceOut, tiny_skia::pipeline::Stage::SourceOut,
                     "sourceOut"},
           StageCase{tiny_skia::BlendMode::DestinationOut,
                     tiny_skia::pipeline::Stage::DestinationOut, "destinationOut"},
           StageCase{tiny_skia::BlendMode::SourceAtop, tiny_skia::pipeline::Stage::SourceAtop,
                     "sourceAtop"},
           StageCase{tiny_skia::BlendMode::DestinationAtop,
                     tiny_skia::pipeline::Stage::DestinationAtop, "destinationAtop"},
           StageCase{tiny_skia::BlendMode::Xor, tiny_skia::pipeline::Stage::Xor, "xor"},
           StageCase{tiny_skia::BlendMode::Plus, tiny_skia::pipeline::Stage::Plus, "plus"},
           StageCase{tiny_skia::BlendMode::Modulate, tiny_skia::pipeline::Stage::Modulate,
                     "modulate"},
           StageCase{tiny_skia::BlendMode::Screen, tiny_skia::pipeline::Stage::Screen, "screen"},
           StageCase{tiny_skia::BlendMode::Overlay, tiny_skia::pipeline::Stage::Overlay, "overlay"},
           StageCase{tiny_skia::BlendMode::Darken, tiny_skia::pipeline::Stage::Darken, "darken"},
           StageCase{tiny_skia::BlendMode::Lighten, tiny_skia::pipeline::Stage::Lighten, "lighten"},
           StageCase{tiny_skia::BlendMode::ColorDodge, tiny_skia::pipeline::Stage::ColorDodge,
                     "colorDodge"},
           StageCase{tiny_skia::BlendMode::ColorBurn, tiny_skia::pipeline::Stage::ColorBurn,
                     "colorBurn"},
           StageCase{tiny_skia::BlendMode::HardLight, tiny_skia::pipeline::Stage::HardLight,
                     "hardLight"},
           StageCase{tiny_skia::BlendMode::SoftLight, tiny_skia::pipeline::Stage::SoftLight,
                     "softLight"},
           StageCase{tiny_skia::BlendMode::Difference, tiny_skia::pipeline::Stage::Difference,
                     "difference"},
           StageCase{tiny_skia::BlendMode::Exclusion, tiny_skia::pipeline::Stage::Exclusion,
                     "exclusion"},
           StageCase{tiny_skia::BlendMode::Multiply, tiny_skia::pipeline::Stage::Multiply,
                     "multiply"},
           StageCase{tiny_skia::BlendMode::Hue, tiny_skia::pipeline::Stage::Hue, "hue"},
           StageCase{tiny_skia::BlendMode::Saturation, tiny_skia::pipeline::Stage::Saturation,
                     "saturation"},
           StageCase{tiny_skia::BlendMode::Color, tiny_skia::pipeline::Stage::Color, "color"},
           StageCase{tiny_skia::BlendMode::Luminosity, tiny_skia::pipeline::Stage::Luminosity,
                     "luminosity"},
       }) {
    SCOPED_TRACE(tc.label);
    const auto mapped = tiny_skia::toStage(tc.mode);
    if (tc.stage.has_value()) {
      EXPECT_THAT(mapped, testing::Optional(testing::Eq(*tc.stage)));
    } else {
      EXPECT_THAT(mapped, testing::Eq(std::nullopt));
    }
  }
}
