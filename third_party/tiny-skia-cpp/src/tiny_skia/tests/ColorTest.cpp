#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

#include "tiny_skia/Color.h"
#include "tiny_skia/Geom.h"
#include "tiny_skia/pipeline/Highp.h"
#include "tiny_skia/pipeline/Lowp.h"

namespace tiny_skia {
class PixmapView {};
}  // namespace tiny_skia

namespace {

using ::testing::ElementsAre;
using ::testing::Optional;
using ::testing::SizeIs;

constexpr float kFloatTolerance = 0.0001f;
constexpr std::string_view kOpaque = "opaque";

void expectColorU8Is(const tiny_skia::ColorU8& actual, std::uint8_t red, std::uint8_t green,
                     std::uint8_t blue, std::uint8_t alpha) {
  EXPECT_EQ(actual.red(), red) << "r";
  EXPECT_EQ(actual.green(), green) << "g";
  EXPECT_EQ(actual.blue(), blue) << "b";
  EXPECT_EQ(actual.alpha(), alpha) << "a";
}

void expectColorU8Is(const tiny_skia::PremultipliedColorU8& actual, std::uint8_t red,
                     std::uint8_t green, std::uint8_t blue, std::uint8_t alpha) {
  expectColorU8Is(tiny_skia::ColorU8(actual.red(), actual.green(), actual.blue(), actual.alpha()),
                  red, green, blue, alpha);
}

void expectColorFloatIs(const tiny_skia::Color& actual, float red, float green, float blue,
                        float alpha) {
  EXPECT_NEAR(actual.red(), red, kFloatTolerance) << "r";
  EXPECT_NEAR(actual.green(), green, kFloatTolerance) << "g";
  EXPECT_NEAR(actual.blue(), blue, kFloatTolerance) << "b";
  EXPECT_NEAR(actual.alpha(), alpha, kFloatTolerance) << "a";
}

}  // namespace

TEST(ColorTest, ColorUPremultiplyPreservesAlphaAndClamp) {
  const tiny_skia::ColorU8 source = tiny_skia::ColorU8::fromRgba(10, 20, 30, 40);
  const auto got = source.premultiply();
  const tiny_skia::PremultipliedColorU8 expected =
      tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(2, 3, 5, 40);
  EXPECT_EQ(got, expected);

  const auto noOp = tiny_skia::ColorU8::fromRgba(10, 20, 30, tiny_skia::kAlphaU8Opaque);
  EXPECT_EQ(noOp.premultiply(),
            tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10, 20, 30, 255));

  const auto validated = tiny_skia::PremultipliedColorU8::fromRgba(2, 3, 5, 40);
  ASSERT_THAT(validated, Optional(testing::_)) << "valid premultiplied values must be accepted";
}

TEST(ColorTest, ColorUPremultiplyU8UsesFixedPointMultiply) {
  struct PremulCase {
    std::string_view label;
    std::uint8_t color;
    std::uint8_t alpha;
    std::uint8_t expected;
  };

  for (const auto& tc : {
           PremulCase{kOpaque, 255, 255, 255},
           PremulCase{"half", 2, 4, 0},
           PremulCase{"round", 10, 40, 2},
           PremulCase{"zero", 0, 255, 0},
       }) {
    SCOPED_TRACE(tc.label);
    EXPECT_EQ(tiny_skia::premultiplyU8(tc.color, tc.alpha), tc.expected);
  }
}

TEST(ColorTest, PremultipliedColorDemultiply) {
  struct DemulCase {
    std::string_view label;
    tiny_skia::PremultipliedColorU8 in;
    tiny_skia::ColorU8 expected;
  };

  for (const auto& tc : {
           DemulCase{"partial alpha",
                     tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(2, 3, 5, 40),
                     tiny_skia::ColorU8::fromRgba(13, 19, 32, 40)},
           DemulCase{"opaque", tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10, 20, 30, 255),
                     tiny_skia::ColorU8::fromRgba(10, 20, 30, 255)},
           DemulCase{"non-trivial",
                     tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(153, 99, 54, 180),
                     tiny_skia::ColorU8::fromRgba(217, 140, 77, 180)},
       }) {
    SCOPED_TRACE(tc.label);
    EXPECT_EQ(tc.in.demultiply(), tc.expected);
  }
}

TEST(ColorTest, PremultipliedColorOptionValidation) {
  EXPECT_THAT(tiny_skia::PremultipliedColorU8::fromRgba(255, 0, 0, 128), testing::Eq(std::nullopt))
      << "channel values above alpha must be invalid";
  EXPECT_THAT(tiny_skia::PremultipliedColorU8::fromRgba(0, 0, 0, 0), Optional(testing::_))
      << "all-zero must be valid";
}

TEST(ColorTest, ColorFromRgbaAndOpacity) {
  const tiny_skia::Color color = tiny_skia::Color::fromRgba8(255, 128, 64, 255);
  expectColorFloatIs(color, 1.0f, 128.0f / 255.0f, 64.0f / 255.0f, 1.0f);
  EXPECT_TRUE(color.isOpaque());

  const auto semiColor = tiny_skia::Color::fromRgba(1.0f, 0.5f, 0.25f, 1.0f);
  ASSERT_THAT(semiColor, Optional(testing::_));
  auto semi = *semiColor;
  semi.applyOpacity(0.5f);
  EXPECT_NEAR(semi.alpha(), 0.5f, kFloatTolerance);

  EXPECT_THAT(tiny_skia::Color::fromRgba(1.1f, -0.1f, 0.0f, 0.5f), testing::Eq(std::nullopt))
      << "out of range color component is rejected";
}

TEST(ColorTest, ColorFromRgbaEdgeCases) {
  EXPECT_THAT(tiny_skia::Color::fromRgba(0.0f, 0.0f, 0.0f, 0.0f), Optional(testing::_));
  EXPECT_THAT(tiny_skia::Color::fromRgba(1.0f, 1.0f, 1.0f, 1.0f), Optional(testing::_));
  EXPECT_THAT(tiny_skia::Color::fromRgba(std::numeric_limits<float>::quiet_NaN(), 0.0f, 0.0f, 0.0f),
              testing::Eq(std::nullopt))
      << "NaN should be rejected";
  EXPECT_THAT(tiny_skia::Color::fromRgba(std::numeric_limits<float>::infinity(), 0.0f, 0.0f, 0.0f),
              testing::Eq(std::nullopt))
      << "infinity should be rejected";
}

TEST(ColorTest, ColorSettersClampToRange) {
  auto color = tiny_skia::Color::fromRgba8(0, 0, 0, 255);
  color.setRed(-1.0f);
  color.setGreen(1.5f);
  color.setBlue(-0.2f);
  color.setAlpha(2.0f);
  expectColorFloatIs(color, 0.0f, 1.0f, 0.0f, 1.0f);
}

TEST(ColorTest, ColorConversionToU8AndPremultiplyRoundTrip) {
  const auto colorOpt = tiny_skia::Color::fromRgba(1.0f, 0.5f, 0.0f, 0.5f);
  ASSERT_THAT(colorOpt, Optional(testing::_));
  const auto color = *colorOpt;
  const auto u8 = color.toColorU8();
  expectColorU8Is(u8, 255, 128, 0, 128);

  const auto pre = color.premultiply();
  const auto premulU8 = pre.toColorU8();
  expectColorU8Is(premulU8, 128, 64, 0, 128);
  EXPECT_EQ(premulU8.alpha(), 128);

  const auto unmul = pre.demultiply();
  expectColorFloatIs(unmul, 1.0f, 0.5f, 0.0f, 0.5f);
}

TEST(ColorTest, ColorSpaceTransforms) {
  using tiny_skia::ColorSpace;
  EXPECT_THAT(tiny_skia::expandStage(ColorSpace::Linear), testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::expandDestStage(ColorSpace::Linear), testing::Eq(std::nullopt));
  EXPECT_THAT(tiny_skia::compressStage(ColorSpace::Linear), testing::Eq(std::nullopt));

  EXPECT_EQ(tiny_skia::expandStage(ColorSpace::Gamma2).value(),
            tiny_skia::pipeline::Stage::GammaExpand2);
  EXPECT_EQ(tiny_skia::expandDestStage(ColorSpace::SimpleSRGB).value(),
            tiny_skia::pipeline::Stage::GammaExpandDestination22);
  EXPECT_EQ(tiny_skia::compressStage(ColorSpace::FullSRGBGamma).value(),
            tiny_skia::pipeline::Stage::GammaCompressSrgb);

  const auto linearOpt = tiny_skia::Color::fromRgba(0.25f, 0.5f, 0.75f, 1.0f);
  ASSERT_THAT(linearOpt, Optional(testing::_));
  const auto color = *linearOpt;
  const auto expanded = tiny_skia::expandColor(ColorSpace::Linear, color);
  const std::array expandedChannels{expanded.red(), expanded.green(), expanded.blue(),
                                    expanded.alpha()};
  EXPECT_THAT(expandedChannels, ElementsAre(0.25f, 0.5f, 0.75f, 1.0f));

  const auto gammaCompressed =
      tiny_skia::compressChannel(ColorSpace::Gamma2, tiny_skia::NormalizedF32::newUnchecked(0.25f));
  EXPECT_NEAR(gammaCompressed.get(), 0.5f, 0.0005f);
}

TEST(ColorTest, PipelineStageOrderingMatchesRustReference) {
  using tiny_skia::pipeline::Stage;

  constexpr std::array<Stage, 84> kExpectedOrder{Stage::MoveSourceToDestination,
                                                 Stage::MoveDestinationToSource,
                                                 Stage::Clamp0,
                                                 Stage::ClampA,
                                                 Stage::Premultiply,
                                                 Stage::UniformColor,
                                                 Stage::SeedShader,
                                                 Stage::LoadDestination,
                                                 Stage::Store,
                                                 Stage::LoadDestinationU8,
                                                 Stage::StoreU8,
                                                 Stage::Gather,
                                                 Stage::LoadMaskU8,
                                                 Stage::MaskU8,
                                                 Stage::ScaleU8,
                                                 Stage::LerpU8,
                                                 Stage::Scale1Float,
                                                 Stage::Lerp1Float,
                                                 Stage::DestinationAtop,
                                                 Stage::DestinationIn,
                                                 Stage::DestinationOut,
                                                 Stage::DestinationOver,
                                                 Stage::SourceAtop,
                                                 Stage::SourceIn,
                                                 Stage::SourceOut,
                                                 Stage::SourceOver,
                                                 Stage::Clear,
                                                 Stage::Modulate,
                                                 Stage::Multiply,
                                                 Stage::Plus,
                                                 Stage::Screen,
                                                 Stage::Xor,
                                                 Stage::ColorBurn,
                                                 Stage::ColorDodge,
                                                 Stage::Darken,
                                                 Stage::Difference,
                                                 Stage::Exclusion,
                                                 Stage::HardLight,
                                                 Stage::Lighten,
                                                 Stage::Overlay,
                                                 Stage::SoftLight,
                                                 Stage::Hue,
                                                 Stage::Saturation,
                                                 Stage::Color,
                                                 Stage::Luminosity,
                                                 Stage::SourceOverRgba,
                                                 Stage::Transform,
                                                 Stage::Reflect,
                                                 Stage::Repeat,
                                                 Stage::Bilinear,
                                                 Stage::Bicubic,
                                                 Stage::PadX1,
                                                 Stage::ReflectX1,
                                                 Stage::RepeatX1,
                                                 Stage::Gradient,
                                                 Stage::EvenlySpaced2StopGradient,
                                                 Stage::XYToUnitAngle,
                                                 Stage::XYToRadius,
                                                 Stage::XYTo2PtConicalFocalOnCircle,
                                                 Stage::XYTo2PtConicalWellBehaved,
                                                 Stage::XYTo2PtConicalSmaller,
                                                 Stage::XYTo2PtConicalGreater,
                                                 Stage::XYTo2PtConicalStrip,
                                                 Stage::Mask2PtConicalNan,
                                                 Stage::Mask2PtConicalDegenerates,
                                                 Stage::ApplyVectorMask,
                                                 Stage::Alter2PtConicalCompensateFocal,
                                                 Stage::Alter2PtConicalUnswap,
                                                 Stage::NegateX,
                                                 Stage::ApplyConcentricScaleBias,
                                                 Stage::GammaExpand2,
                                                 Stage::GammaExpandDestination2,
                                                 Stage::GammaCompress2,
                                                 Stage::GammaExpand22,
                                                 Stage::GammaExpandDestination22,
                                                 Stage::GammaCompress22,
                                                 Stage::GammaExpandSrgb,
                                                 Stage::GammaExpandDestinationSrgb,
                                                 Stage::GammaCompressSrgb,
                                                 Stage::Unpremultiply,
                                                 Stage::PremultiplyDestination,
                                                 Stage::FusedLinearGradient2Stop,
                                                 Stage::FusedRadialGradient2Stop,
                                                 Stage::FusedBilinearPattern};

  EXPECT_EQ(static_cast<std::size_t>(84), tiny_skia::pipeline::kStagesCount);
  EXPECT_EQ(tiny_skia::pipeline::kStagesCount, kExpectedOrder.size());

  for (std::size_t i = 0; i < kExpectedOrder.size(); ++i) {
    EXPECT_EQ(static_cast<std::size_t>(kExpectedOrder[i]), i) << "stage at index " << i;
  }
}

TEST(ColorTest, PipelineAAMaskCtxCopyAtXYReturnsExpectedPairs) {
  tiny_skia::pipeline::AAMaskCtx ctx{{1, 3}, 4, 1};
  EXPECT_THAT(ctx.copyAtXY(1, 0, 1), testing::ElementsAre(1u, 0u));
  EXPECT_THAT(ctx.copyAtXY(1, 0, 2), testing::ElementsAre(1u, 3u));
  EXPECT_THAT(ctx.copyAtXY(2, 0, 1), testing::ElementsAre(3u, 0u));
  EXPECT_THAT(ctx.copyAtXY(3, 0, 1), testing::ElementsAre(0u, 0u));
}

TEST(ColorTest, PipelineGradientCtxPushConstColorAppendsBiasAndZeroFactor) {
  tiny_skia::pipeline::Context::GradientCtx gradientCtx{};
  const auto first = tiny_skia::pipeline::GradientColor::newFromRGBA(0.2f, 0.4f, 0.6f, 0.8f);
  const auto second = tiny_skia::pipeline::GradientColor::newFromRGBA(0.3f, 0.5f, 0.7f, 0.9f);

  ASSERT_THAT(gradientCtx.factors, SizeIs(0));
  ASSERT_THAT(gradientCtx.biases, SizeIs(0));
  gradientCtx.pushConstColor(first);
  EXPECT_THAT(gradientCtx.factors, SizeIs(1));
  EXPECT_THAT(gradientCtx.biases, SizeIs(1));
  EXPECT_EQ(gradientCtx.factors[0], tiny_skia::pipeline::GradientColor{});
  EXPECT_EQ(gradientCtx.biases[0].r, first.r);
  EXPECT_EQ(gradientCtx.biases[0].g, first.g);
  EXPECT_EQ(gradientCtx.biases[0].b, first.b);

  gradientCtx.pushConstColor(second);
  EXPECT_THAT(gradientCtx.factors, SizeIs(2));
  EXPECT_THAT(gradientCtx.biases, SizeIs(2));
  EXPECT_EQ(gradientCtx.biases[1].a, second.a);
}

TEST(ColorTest, RasterPipelineBuilderCompileBuildsExpectedKindAndContext) {
  tiny_skia::pipeline::RasterPipelineBuilder builder;
  builder.setForceHqPipeline(true);
  const auto uniform = tiny_skia::Color::fromRgba8(10, 20, 30, 40).premultiply();

  builder.pushUniformColor(uniform);
  auto pipeline = builder.compile();

  EXPECT_EQ(pipeline.kind(), tiny_skia::pipeline::RasterPipeline::Kind::High);
  const auto& ctx = pipeline.ctx();
  EXPECT_NEAR(ctx.uniformColor.r, uniform.red(), 0.0001f);
  EXPECT_NEAR(ctx.uniformColor.g, uniform.green(), 0.0001f);
  EXPECT_NEAR(ctx.uniformColor.b, uniform.blue(), 0.0001f);
  EXPECT_NEAR(ctx.uniformColor.a, uniform.alpha(), 0.0001f);
  const std::array rgba{ctx.uniformColor.rgba[0], ctx.uniformColor.rgba[1],
                        ctx.uniformColor.rgba[2], ctx.uniformColor.rgba[3]};
  EXPECT_THAT(rgba, ElementsAre(static_cast<std::uint16_t>(uniform.red() * 255.0f + 0.5f),
                                static_cast<std::uint16_t>(uniform.green() * 255.0f + 0.5f),
                                static_cast<std::uint16_t>(uniform.blue() * 255.0f + 0.5f),
                                static_cast<std::uint16_t>(uniform.alpha() * 255.0f + 0.5f)));
}

TEST(ColorTest, RasterPipelineBuilderCompileUsesLowpByDefaultForSupportedStages) {
  tiny_skia::pipeline::RasterPipelineBuilder builder;
  builder.push(tiny_skia::pipeline::Stage::LoadDestination);
  builder.push(tiny_skia::pipeline::Stage::Store);

  const auto pipeline = builder.compile();
  EXPECT_EQ(pipeline.kind(), tiny_skia::pipeline::RasterPipeline::Kind::Low);
}

TEST(ColorTest, RasterPipelineBuilderCompileForcesHighForUnsupportedStage) {
  tiny_skia::pipeline::RasterPipelineBuilder builder;
  builder.push(tiny_skia::pipeline::Stage::LoadDestination);
  builder.push(tiny_skia::pipeline::Stage::Clamp0);
  builder.push(tiny_skia::pipeline::Stage::Store);

  const auto pipeline = builder.compile();
  EXPECT_EQ(pipeline.kind(), tiny_skia::pipeline::RasterPipeline::Kind::High);
}

TEST(ColorTest, RasterPipelineCompileRecordsStageCountForExecutionPlan) {
  tiny_skia::pipeline::RasterPipelineBuilder builder;
  builder.push(tiny_skia::pipeline::Stage::LoadDestination);
  builder.push(tiny_skia::pipeline::Stage::Store);
  builder.push(tiny_skia::pipeline::Stage::LoadMaskU8);

  auto pipeline = builder.compile();
  EXPECT_EQ(pipeline.stageCount(), 3u);
  EXPECT_TRUE(pipeline.ctx().transform.isFinite());
  EXPECT_TRUE(pipeline.ctx().transform.isIdentity());
}

TEST(ColorTest, PipelineHighpLowpFunctionsHaveExpectedSignaturesAndDefaults) {
  constexpr std::size_t stageWidthHighp = tiny_skia::pipeline::highp::kStageWidth;
  constexpr std::size_t stageWidthLowp = tiny_skia::pipeline::lowp::kStageWidth;
  EXPECT_EQ(stageWidthHighp, 8u);
  EXPECT_EQ(stageWidthLowp, 16u);
  EXPECT_EQ(tiny_skia::pipeline::highp::STAGES.size(), tiny_skia::pipeline::kStagesCount);
  EXPECT_EQ(tiny_skia::pipeline::lowp::STAGES.size(), tiny_skia::pipeline::kStagesCount);

  EXPECT_FALSE(tiny_skia::pipeline::highp::fnPtrEq(tiny_skia::pipeline::highp::STAGES[0],
                                                   &tiny_skia::pipeline::highp::justReturn));
  EXPECT_FALSE(tiny_skia::pipeline::lowp::fnPtrEq(tiny_skia::pipeline::lowp::STAGES[0],
                                                  &tiny_skia::pipeline::lowp::justReturn));
  EXPECT_FALSE(tiny_skia::pipeline::highp::fnPtrEq(tiny_skia::pipeline::highp::STAGES[0],
                                                   tiny_skia::pipeline::highp::STAGES[1]));
  EXPECT_FALSE(tiny_skia::pipeline::lowp::fnPtrEq(tiny_skia::pipeline::lowp::STAGES[0],
                                                  tiny_skia::pipeline::lowp::STAGES[1]));
  EXPECT_FALSE(tiny_skia::pipeline::highp::fnPtrEq(tiny_skia::pipeline::highp::STAGES[6],
                                                   &tiny_skia::pipeline::highp::justReturn));
  EXPECT_FALSE(tiny_skia::pipeline::lowp::fnPtrEq(tiny_skia::pipeline::lowp::STAGES[6],
                                                  &tiny_skia::pipeline::lowp::justReturn));
  EXPECT_FALSE(tiny_skia::pipeline::highp::fnPtrEq(tiny_skia::pipeline::highp::STAGES[14],
                                                   &tiny_skia::pipeline::highp::justReturn));
  EXPECT_FALSE(tiny_skia::pipeline::lowp::fnPtrEq(tiny_skia::pipeline::lowp::STAGES[14],
                                                  &tiny_skia::pipeline::lowp::justReturn));
  EXPECT_FALSE(tiny_skia::pipeline::highp::fnPtrEq(tiny_skia::pipeline::highp::STAGES[15],
                                                   &tiny_skia::pipeline::highp::justReturn));
  EXPECT_FALSE(tiny_skia::pipeline::lowp::fnPtrEq(tiny_skia::pipeline::lowp::STAGES[15],
                                                  &tiny_skia::pipeline::lowp::justReturn));

  for (const std::size_t index : {17ull, 18ull, 19ull, 20ull, 21ull, 22ull, 23ull, 24ull, 25ull,
                                  26ull, 27ull, 28ull, 29ull, 30ull, 31ull}) {
    EXPECT_FALSE(tiny_skia::pipeline::highp::fnPtrEq(tiny_skia::pipeline::highp::STAGES[index],
                                                     &tiny_skia::pipeline::highp::justReturn));
    EXPECT_FALSE(tiny_skia::pipeline::lowp::fnPtrEq(tiny_skia::pipeline::lowp::STAGES[index],
                                                    &tiny_skia::pipeline::lowp::justReturn));
  }
  EXPECT_FALSE(tiny_skia::pipeline::highp::fnPtrEq(tiny_skia::pipeline::highp::STAGES[32],
                                                   &tiny_skia::pipeline::highp::justReturn));
  EXPECT_FALSE(tiny_skia::pipeline::highp::fnPtrEq(tiny_skia::pipeline::highp::STAGES[33],
                                                   &tiny_skia::pipeline::highp::justReturn));
}

namespace {

std::size_t highpFullChunkCallCount = 0;
std::size_t highpTailChunkCallCount = 0;

void highpFullChunkFn(tiny_skia::pipeline::highp::Pipeline&) { ++highpFullChunkCallCount; }

void highpTailChunkFn(tiny_skia::pipeline::highp::Pipeline&) { ++highpTailChunkCallCount; }

void resetHighpDispatchCounters() {
  highpFullChunkCallCount = 0;
  highpTailChunkCallCount = 0;
}

std::size_t lowpFullChunkCallCount = 0;
std::size_t lowpTailChunkCallCount = 0;

void lowpFullChunkFn(tiny_skia::pipeline::lowp::Pipeline&) { ++lowpFullChunkCallCount; }

void lowpTailChunkFn(tiny_skia::pipeline::lowp::Pipeline&) { ++lowpTailChunkCallCount; }

void resetLowpDispatchCounters() {
  lowpFullChunkCallCount = 0;
  lowpTailChunkCallCount = 0;
}

}  // namespace

TEST(ColorTest, PipelineLowpStartUsesFullAndTailChunks) {
  const auto rect = tiny_skia::ScreenIntRect::fromXYWH(0, 0, 17, 2);
  ASSERT_THAT(rect, Optional(testing::_)) << "test rectangle should be valid";

  std::array<tiny_skia::pipeline::lowp::StageFn, tiny_skia::pipeline::kMaxStages> lowpFull{};
  std::array<tiny_skia::pipeline::lowp::StageFn, tiny_skia::pipeline::kMaxStages> lowpTail{};
  lowpFull.fill(&lowpFullChunkFn);
  lowpTail.fill(&lowpTailChunkFn);

  tiny_skia::pipeline::AAMaskCtx aaMask{};
  tiny_skia::pipeline::MaskCtx maskCtx{};
  tiny_skia::pipeline::Context context{};
  resetLowpDispatchCounters();

  tiny_skia::pipeline::lowp::start(lowpFull, lowpTail, *rect, aaMask, maskCtx, context, nullptr);

  EXPECT_EQ(lowpFullChunkCallCount, 2u) << "one full chunk per row";
  EXPECT_EQ(lowpTailChunkCallCount, 2u) << "one tail chunk per row";
}

TEST(ColorTest, PipelineHighpStartUsesFullAndTailChunks) {
  const auto rect = tiny_skia::ScreenIntRect::fromXYWH(0, 0, 17, 2);
  ASSERT_THAT(rect, Optional(testing::_)) << "test rectangle should be valid";

  std::array<tiny_skia::pipeline::highp::StageFn, tiny_skia::pipeline::kMaxStages> highpFull{};
  std::array<tiny_skia::pipeline::highp::StageFn, tiny_skia::pipeline::kMaxStages> highpTail{};
  highpFull.fill(&highpFullChunkFn);
  highpTail.fill(&highpTailChunkFn);

  tiny_skia::pipeline::AAMaskCtx aaMask{};
  tiny_skia::pipeline::MaskCtx maskCtx{};
  tiny_skia::pipeline::Context context{};
  const tiny_skia::PixmapView source{};
  resetHighpDispatchCounters();

  tiny_skia::pipeline::highp::start(highpFull, highpTail, *rect, aaMask, maskCtx, context, source,
                                    nullptr);

  EXPECT_EQ(highpFullChunkCallCount, 4u) << "two full chunks per row";
  EXPECT_EQ(highpTailChunkCallCount, 2u) << "one tail chunk per row";
}

TEST(ColorTest, GradientColorNewFromRGBARoundTripsRGBAValues) {
  const auto color = tiny_skia::pipeline::GradientColor::newFromRGBA(0.1f, 0.2f, 0.3f, 0.4f);
  EXPECT_NEAR(color.r, 0.1f, 1e-6f);
  EXPECT_NEAR(color.g, 0.2f, 1e-6f);
  EXPECT_NEAR(color.b, 0.3f, 1e-6f);
  EXPECT_NEAR(color.a, 0.4f, 1e-6f);
}

TEST(ColorTest, PipelineContextDefaultsMatchExpectedMembers) {
  const auto context = tiny_skia::pipeline::Context{};
  const auto& transform = context.transform;

  EXPECT_EQ(context.currentCoverage, 0.0f);
  EXPECT_EQ(context.sampler.spreadMode, tiny_skia::SpreadMode::Pad);
  EXPECT_EQ(context.sampler.invWidth, 0.0f);
  EXPECT_EQ(context.sampler.invHeight, 0.0f);
  const std::array uniformColor{context.uniformColor.r, context.uniformColor.g,
                                context.uniformColor.b, context.uniformColor.a};
  EXPECT_THAT(uniformColor, ElementsAre(0.0f, 0.0f, 0.0f, 0.0f));
  EXPECT_TRUE(transform.isFinite());
  EXPECT_TRUE(transform.isIdentity());
}

TEST(ColorTest, MaskCtxOffsetMatchesPackedCoordinateFormula) {
  tiny_skia::pipeline::MaskCtx ctx{};
  ctx.realWidth = 7;

  const std::array offsets{ctx.byteOffset(0, 0), ctx.byteOffset(3, 0), ctx.byteOffset(2, 1),
                           ctx.byteOffset(6, 2)};
  EXPECT_THAT(offsets, ElementsAre(0u, 3u, 9u, 20u));
}

TEST(ColorTest, RasterPipelineBuilderPushAppendsStage) {
  tiny_skia::pipeline::RasterPipelineBuilder builder;
  builder.push(tiny_skia::pipeline::Stage::LoadDestination);

  auto pipeline = builder.compile();
  EXPECT_EQ(pipeline.kind(), tiny_skia::pipeline::RasterPipeline::Kind::Low);
}

TEST(ColorTest, RasterPipelineBuilderPushTransformSkipsIdentity) {
  tiny_skia::pipeline::RasterPipelineBuilder builder;
  builder.pushTransform(tiny_skia::Transform{});

  const auto pipeline = builder.compile();
  EXPECT_EQ(pipeline.kind(), tiny_skia::pipeline::RasterPipeline::Kind::High)
      << "identity transform must not push transform stage";
}

TEST(ColorTest, RasterPipelineBuilderPushTransformStoresNonIdentityTransform) {
  tiny_skia::pipeline::RasterPipelineBuilder builder;
  auto ts = tiny_skia::Transform::fromTranslate(10.0f, 20.0f);
  builder.pushTransform(ts);

  const auto pipeline = builder.compile();
  EXPECT_EQ(pipeline.kind(), tiny_skia::pipeline::RasterPipeline::Kind::Low);
  EXPECT_FALSE(pipeline.ctx().transform.isIdentity());
}

// Ported from Rust color.rs: bytemuck_casts_rgba
// Verifies that PremultipliedColorU8 has RGBA byte layout (R at offset 0, A at offset 3).
TEST(ColorTest, PremultipliedColorU8HasRGBAByteLayout) {
  const std::array<tiny_skia::PremultipliedColorU8, 2> slice = {
      tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(0, 1, 2, 3),
      tiny_skia::PremultipliedColorU8::fromRgbaUnchecked(10, 11, 12, 13),
  };
  // Reinterpret the array as raw bytes; the Rust bytemuck test expects
  // [0, 1, 2, 3, 10, 11, 12, 13] in RGBA order.
  static_assert(sizeof(slice) == 8, "two PremultipliedColorU8 should be 8 bytes");
  const auto* bytes = reinterpret_cast<const std::uint8_t*>(slice.data());
  EXPECT_EQ(bytes[0], 0);
  EXPECT_EQ(bytes[1], 1);
  EXPECT_EQ(bytes[2], 2);
  EXPECT_EQ(bytes[3], 3);
  EXPECT_EQ(bytes[4], 10);
  EXPECT_EQ(bytes[5], 11);
  EXPECT_EQ(bytes[6], 12);
  EXPECT_EQ(bytes[7], 13);
}

TEST(ColorTest, RasterPipelineBuilderCompileStageOverflowSkipsBeyondCapacitySafely) {
  tiny_skia::pipeline::RasterPipelineBuilder builder;
  for (std::size_t i = 0; i < tiny_skia::pipeline::kMaxStages + 1; ++i) {
    builder.push(tiny_skia::pipeline::Stage::LoadDestination);
  }

  const auto pipeline = builder.compile();
  EXPECT_EQ(pipeline.kind(), tiny_skia::pipeline::RasterPipeline::Kind::Low);
}
