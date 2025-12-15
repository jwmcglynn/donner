#include "donner/backends/tiny_skia_cpp/Shader.h"

#include <gtest/gtest.h>

#include <random>
#include <vector>

#include "donner/backends/tiny_skia_cpp/Transform.h"

namespace donner::backends::tiny_skia_cpp {
namespace {

TEST(ShaderStopsTests, InsertsMissingEndpointsAndNormalizes) {
  std::vector<GradientStop> stops{{0.3f, Color::RGB(10, 20, 30)}, {0.7f, Color::RGB(40, 50, 60)}};

  Expected<Shader, std::string> shader =
      Shader::MakeLinearGradient(Vector2d(0.0, 0.0), Vector2d(10.0, 0.0), stops, SpreadMode::kPad);

  ASSERT_TRUE(shader.hasValue());
  const Shader& compiled = shader.value();
  ASSERT_EQ(compiled.kind(), ShaderKind::kLinearGradient);

  const GradientData& data = compiled.linearGradient().gradient;
  ASSERT_EQ(data.stops.size(), 4u);
  EXPECT_FLOAT_EQ(data.stops[0].position, 0.0f);
  EXPECT_FLOAT_EQ(data.stops[1].position, 0.3f);
  EXPECT_FLOAT_EQ(data.stops[2].position, 0.7f);
  EXPECT_FLOAT_EQ(data.stops[3].position, 1.0f);
  EXPECT_FALSE(data.hasUniformStops);
  EXPECT_TRUE(data.colorsAreOpaque);
}

TEST(ShaderStopsTests, ClampsNonMonotonicStops) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)},
                                  {0.6f, Color::RGB(10, 10, 10)},
                                  {0.4f, Color::RGB(20, 20, 20)},
                                  {1.2f, Color::RGB(30, 30, 30)}};

  Expected<Shader, std::string> shader =
      Shader::MakeLinearGradient(Vector2d(0.0, 0.0), Vector2d(10.0, 0.0), stops, SpreadMode::kPad);

  ASSERT_TRUE(shader.hasValue());
  const GradientData& data = shader.value().linearGradient().gradient;
  ASSERT_EQ(data.stops.size(), 5u);
  EXPECT_FLOAT_EQ(data.stops[0].position, 0.0f);
  EXPECT_FLOAT_EQ(data.stops[1].position, 0.6f);
  EXPECT_FLOAT_EQ(data.stops[2].position, 0.6f);
  EXPECT_FLOAT_EQ(data.stops[3].position, 1.0f);
  EXPECT_FLOAT_EQ(data.stops[4].position, 1.0f);
}

TEST(ShaderStopsTests, DetectsUniformStopsAndTransparency) {
  std::vector<GradientStop> stops{
      {0.0f, Color::RGB(0, 0, 0)}, {0.5f, Color::RGB(10, 10, 10)}, {1.0f, Color(1, 2, 3, 0x80)}};

  Expected<Shader, std::string> shader =
      Shader::MakeLinearGradient(Vector2d(0.0, 0.0), Vector2d(10.0, 0.0), stops, SpreadMode::kPad);

  ASSERT_TRUE(shader.hasValue());
  const GradientData& data = shader.value().linearGradient().gradient;
  EXPECT_TRUE(data.hasUniformStops);
  EXPECT_FALSE(data.colorsAreOpaque);
}

TEST(ShaderCreationTests, LinearGradientFallsBackToSolidColorForSingleStop) {
  std::vector<GradientStop> stops{{0.4f, Color::RGB(1, 2, 3)}};

  Expected<Shader, std::string> shader =
      Shader::MakeLinearGradient(Vector2d(0.0, 0.0), Vector2d(1.0, 0.0), stops, SpreadMode::kPad);

  ASSERT_TRUE(shader.hasValue());
  EXPECT_EQ(shader.value().kind(), ShaderKind::kSolidColor);
  EXPECT_EQ(shader.value().solidColor().color, Color::RGB(1, 2, 3));
}

TEST(ShaderCreationTests, LinearGradientDegeneratePadUsesLastColor) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(1, 1, 1)}, {1.0f, Color::RGB(9, 9, 9)}};

  Expected<Shader, std::string> shader =
      Shader::MakeLinearGradient(Vector2d(2.0, 2.0), Vector2d(2.0, 2.0), stops, SpreadMode::kPad);

  ASSERT_TRUE(shader.hasValue());
  EXPECT_EQ(shader.value().kind(), ShaderKind::kSolidColor);
  EXPECT_EQ(shader.value().solidColor().color, Color::RGB(9, 9, 9));
}

TEST(ShaderCreationTests, LinearGradientDegenerateRepeatAveragesColor) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(10, 20, 30)}};

  Expected<Shader, std::string> shader = Shader::MakeLinearGradient(
      Vector2d(5.0, 5.0), Vector2d(5.0, 5.0), stops, SpreadMode::kRepeat);

  ASSERT_TRUE(shader.hasValue());
  EXPECT_EQ(shader.value().kind(), ShaderKind::kSolidColor);
  EXPECT_EQ(shader.value().solidColor().color, Color(5, 10, 15, 0xFF));
}

TEST(ShaderCreationTests, LinearGradientRejectsSingularTransform) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(10, 10, 10)}};

  const Transform singular = Transform::Scale({0.0, 1.0});
  Expected<Shader, std::string> shader = Shader::MakeLinearGradient(
      Vector2d(0.0, 0.0), Vector2d(1.0, 0.0), stops, SpreadMode::kPad, singular);

  EXPECT_FALSE(shader.hasValue());
}

TEST(ShaderCreationTests, RadialGradientValidatesRadiusAndStops) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(10, 10, 10)}};

  Expected<Shader, std::string> missingRadius = Shader::MakeRadialGradient(
      Vector2d(0.0, 0.0), Vector2d(1.0, 1.0), 0.0f, stops, SpreadMode::kPad);
  EXPECT_FALSE(missingRadius.hasValue());

  Expected<Shader, std::string> valid = Shader::MakeRadialGradient(
      Vector2d(0.0, 0.0), Vector2d(1.0, 1.0), 10.0f, stops, SpreadMode::kPad);
  ASSERT_TRUE(valid.hasValue());
  EXPECT_EQ(valid.value().kind(), ShaderKind::kRadialGradient);
  EXPECT_EQ(valid.value().radialGradient().radius, 10.0f);
}

TEST(ShaderCreationTests, RadialGradientRejectsSingularTransform) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(10, 10, 10)}};
  const Transform singular = Transform::Scale({1.0, 0.0});

  Expected<Shader, std::string> shader = Shader::MakeRadialGradient(
      Vector2d(0.0, 0.0), Vector2d(1.0, 1.0), 5.0f, stops, SpreadMode::kPad, singular);

  EXPECT_FALSE(shader.hasValue());
}

TEST(ShaderCreationTests, RadialGradientRejectsFullyDegenerate) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(10, 10, 10)}};

  Expected<Shader, std::string> shader = Shader::MakeRadialGradient(
      Vector2d(0.0, 0.0), Vector2d(0.0, 0.0), 1e-8f, stops, SpreadMode::kPad);

  EXPECT_FALSE(shader.hasValue());
}

TEST(ShaderSamplingTests, SamplesSolidColor) {
  Shader shader = Shader::MakeSolidColor(Color::RGB(1, 2, 3));
  Expected<ShaderContext, std::string> context = ShaderContext::Create(shader);

  ASSERT_TRUE(context.hasValue());
  EXPECT_EQ(context.value().sample(Vector2d(0.0, 0.0)), Color::RGB(1, 2, 3));
  EXPECT_EQ(context.value().sample(Vector2d(10.0, 5.0)), Color::RGB(1, 2, 3));
}

TEST(ShaderSamplingTests, SamplesLinearGradientWithSpread) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(255, 255, 255)}};
  Expected<Shader, std::string> shader = Shader::MakeLinearGradient(
      Vector2d(0.0, 0.0), Vector2d(10.0, 0.0), stops, SpreadMode::kRepeat);

  ASSERT_TRUE(shader.hasValue());
  Expected<ShaderContext, std::string> context = ShaderContext::Create(shader.value());
  ASSERT_TRUE(context.hasValue());

  EXPECT_EQ(context.value().sample(Vector2d(2.5, 0.0)), Color(64, 64, 64, 255));
  EXPECT_EQ(context.value().sample(Vector2d(12.5, 0.0)), Color(64, 64, 64, 255));

  Expected<Shader, std::string> reflectShader = Shader::MakeLinearGradient(
      Vector2d(0.0, 0.0), Vector2d(10.0, 0.0), stops, SpreadMode::kReflect);
  ASSERT_TRUE(reflectShader.hasValue());
  Expected<ShaderContext, std::string> reflectContext =
      ShaderContext::Create(reflectShader.value());
  ASSERT_TRUE(reflectContext.hasValue());

  EXPECT_EQ(reflectContext.value().sample(Vector2d(15.0, 0.0)), Color(128, 128, 128, 255));
}

TEST(ShaderSamplingTests, LinearSpanMatchesPerPixelSampling) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(200, 100, 50)}};
  Expected<Shader, std::string> shader =
      Shader::MakeLinearGradient(Vector2d(0.0, 0.0), Vector2d(8.0, 0.0), stops, SpreadMode::kPad);

  ASSERT_TRUE(shader.hasValue());
  Expected<ShaderContext, std::string> context = ShaderContext::Create(shader.value());
  ASSERT_TRUE(context.hasValue());

  std::vector<Color> span;
  EXPECT_TRUE(context.value().sampleLinearSpan(0, 0, 5, span));
  ASSERT_EQ(span.size(), 5u);

  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(span[static_cast<size_t>(i)],
              context.value().sample(Vector2d(static_cast<double>(i) + 0.5, 0.5)));
  }
}

TEST(ShaderSamplingTests, LinearSpanSupportsNegativeStep) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(0, 0, 255)}};
  Transform flip = Transform::Scale({-1.0, 1.0});
  flip = Transform::Translate({8.0, 0.0}) * flip;

  Expected<Shader, std::string> shader = Shader::MakeLinearGradient(
      Vector2d(0.0, 0.0), Vector2d(8.0, 0.0), stops, SpreadMode::kPad, flip);

  ASSERT_TRUE(shader.hasValue());
  Expected<ShaderContext, std::string> context = ShaderContext::Create(shader.value());
  ASSERT_TRUE(context.hasValue());

  std::vector<Color> span;
  EXPECT_TRUE(context.value().sampleLinearSpan(0, 0, 3, span));
  ASSERT_EQ(span.size(), 3u);

  for (int i = 0; i < 3; ++i) {
    const double x = static_cast<double>(i) + 0.5;
    EXPECT_EQ(span[static_cast<size_t>(i)], context.value().sample(Vector2d(x, 0.5)));
  }
}

TEST(ShaderSamplingTests, LinearSpanSimdPathMatchesScalar) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(10, 20, 30)},
                                  {1.0f, Color::RGB(210, 220, 230)}};
  Expected<Shader, std::string> shader = Shader::MakeLinearGradient(
      Vector2d(0.0, 0.0), Vector2d(16.0, 0.0), stops, SpreadMode::kPad);

  ASSERT_TRUE(shader.hasValue());
  Expected<ShaderContext, std::string> context = ShaderContext::Create(shader.value());
  ASSERT_TRUE(context.hasValue());

  std::vector<Color> span;
  EXPECT_TRUE(context.value().sampleLinearSpan(0, 0, 10, span));
  ASSERT_EQ(span.size(), 10u);

  for (int i = 0; i < 10; ++i) {
    const double x = static_cast<double>(i) + 0.5;
    EXPECT_EQ(span[static_cast<size_t>(i)], context.value().sample(Vector2d(x, 0.5)));
  }
}

TEST(ShaderSamplingTests, LinearSpanSimdRandomizedParity) {
  std::mt19937 rng(12345);
  std::uniform_int_distribution<int> color(0, 255);
  std::uniform_real_distribution<double> position(-16.0, 16.0);
  std::uniform_real_distribution<double> scale(0.25, 3.0);
  std::uniform_int_distribution<int> width(1, 32);
  std::uniform_int_distribution<int> coordinate(-4, 4);

  for (int iteration = 0; iteration < 50; ++iteration) {
    const Color leftColor(static_cast<uint8_t>(color(rng)), static_cast<uint8_t>(color(rng)),
                          static_cast<uint8_t>(color(rng)), 255);
    const Color rightColor(static_cast<uint8_t>(color(rng)), static_cast<uint8_t>(color(rng)),
                           static_cast<uint8_t>(color(rng)), 255);
    std::vector<GradientStop> stops{{0.0f, leftColor}, {1.0f, rightColor}};

    const Vector2d start(position(rng), position(rng));
    const Vector2d end(position(rng), position(rng));

    Transform transform = Transform::Translate(Vector2d(position(rng), position(rng))) *
                          Transform::Scale(Vector2d(scale(rng), scale(rng)));

    Expected<Shader, std::string> shader =
        Shader::MakeLinearGradient(start, end, stops, SpreadMode::kPad, transform);
    ASSERT_TRUE(shader.hasValue());

    Expected<ShaderContext, std::string> context = ShaderContext::Create(shader.value());
    ASSERT_TRUE(context.hasValue());

    const int spanWidth = width(rng);
    const int x = coordinate(rng);
    const int y = coordinate(rng);

    std::vector<Color> span;
    EXPECT_TRUE(context.value().sampleLinearSpan(x, y, spanWidth, span));
    ASSERT_EQ(span.size(), static_cast<size_t>(spanWidth));

    for (int i = 0; i < spanWidth; ++i) {
      const double sampleX = static_cast<double>(x + i) + 0.5;
      const double sampleY = static_cast<double>(y) + 0.5;
      EXPECT_EQ(span[static_cast<size_t>(i)], context.value().sample(Vector2d(sampleX, sampleY)));
    }
  }
}

TEST(ShaderSamplingTests, LinearSpanRejectsNonPadSpread) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(255, 0, 0)}};
  Expected<Shader, std::string> shader = Shader::MakeLinearGradient(
      Vector2d(0.0, 0.0), Vector2d(10.0, 0.0), stops, SpreadMode::kRepeat);

  ASSERT_TRUE(shader.hasValue());
  Expected<ShaderContext, std::string> context = ShaderContext::Create(shader.value());
  ASSERT_TRUE(context.hasValue());

  std::vector<Color> span;
  EXPECT_FALSE(context.value().sampleLinearSpan(0, 0, 4, span));
}

TEST(ShaderSamplingTests, InterpolatesGradientMidpointsWithVectorLerp) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(120, 140, 200)}};
  Expected<Shader, std::string> shader =
      Shader::MakeLinearGradient(Vector2d(0.0, 0.0), Vector2d(2.0, 0.0), stops, SpreadMode::kPad);

  ASSERT_TRUE(shader.hasValue());
  Expected<ShaderContext, std::string> context = ShaderContext::Create(shader.value());
  ASSERT_TRUE(context.hasValue());

  EXPECT_EQ(context.value().sample(Vector2d(1.0, 0.0)), Color(60, 70, 100, 255));
  EXPECT_EQ(context.value().sample(Vector2d(3.0, 0.0)), Color(120, 140, 200, 255));
}

TEST(ShaderSamplingTests, LinearGradientRespectsTransform) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(200, 0, 0)}};
  const Transform transform = Transform::Scale(Vector2d(2.0, 1.0));
  Expected<Shader, std::string> shader = Shader::MakeLinearGradient(
      Vector2d(0.0, 0.0), Vector2d(10.0, 0.0), stops, SpreadMode::kPad, transform);

  ASSERT_TRUE(shader.hasValue());
  Expected<ShaderContext, std::string> context = ShaderContext::Create(shader.value());
  ASSERT_TRUE(context.hasValue());

  // Scale doubles x, so sampling at x=10 in device space maps to t=0.5 in shader space.
  EXPECT_EQ(context.value().sample(Vector2d(10.0, 0.0)), Color(100, 0, 0, 255));
}

TEST(ShaderSamplingTests, SamplesRadialGradient) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(0, 0, 255)}};
  Expected<Shader, std::string> shader = Shader::MakeRadialGradient(
      Vector2d(0.0, 0.0), Vector2d(0.0, 0.0), 10.0f, stops, SpreadMode::kPad);

  ASSERT_TRUE(shader.hasValue());
  Expected<ShaderContext, std::string> context = ShaderContext::Create(shader.value());
  ASSERT_TRUE(context.hasValue());

  EXPECT_EQ(context.value().sample(Vector2d(5.0, 0.0)), Color(0, 0, 128, 255));
  EXPECT_EQ(context.value().sample(Vector2d(20.0, 0.0)), Color(0, 0, 255, 255));
}

TEST(ShaderSamplingTests, SamplesTwoPointRadialGradient) {
  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(255, 255, 255)}};
  Expected<Shader, std::string> shader = Shader::MakeRadialGradient(
      Vector2d(0.0, 0.0), Vector2d(10.0, 0.0), 10.0f, stops, SpreadMode::kPad);

  ASSERT_TRUE(shader.hasValue());
  Expected<ShaderContext, std::string> context = ShaderContext::Create(shader.value());
  ASSERT_TRUE(context.hasValue());

  EXPECT_EQ(context.value().sample(Vector2d(5.0, 0.0)), Color(64, 64, 64, 255));
  EXPECT_EQ(context.value().sample(Vector2d(10.0, 0.0)), Color(128, 128, 128, 255));
}

}  // namespace
}  // namespace donner::backends::tiny_skia_cpp
