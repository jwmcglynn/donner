#include "donner/backends/tiny_skia_cpp/Paint.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <span>

#include "donner/backends/tiny_skia_cpp/Canvas.h"
#include "donner/backends/tiny_skia_cpp/Shader.h"
#include "donner/base/Vector2.h"
#include "gtest/gtest.h"

namespace donner::backends::tiny_skia_cpp {

TEST(PaintContextTests, AppliesOpacity) {
  Paint paint;
  paint.color = Color::RGB(100, 150, 200);
  paint.opacity = 0.5f;

  auto context = PaintContext::Create(paint);
  ASSERT_TRUE(context.hasValue());

  const Color shaded = context.value().shade(Vector2d());
  EXPECT_EQ(shaded.r, 50);
  EXPECT_EQ(shaded.g, 75);
  EXPECT_EQ(shaded.b, 100);
  EXPECT_EQ(shaded.a, 128);
}

TEST(PaintContextTests, ClampsOpacityScaling) {
  Paint paint;
  paint.color = Color(250, 250, 250, 200);
  paint.opacity = 2.0f;

  auto context = PaintContext::Create(paint);
  ASSERT_TRUE(context.hasValue());

  const Color shaded = context.value().shade(Vector2d());
  EXPECT_EQ(shaded, Color(250, 250, 250, 200));
}

TEST(PaintContextTests, SamplesShaderWhenPresent) {
  Paint paint;
  paint.opacity = 1.0f;

  std::vector<GradientStop> stops{{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color::RGB(255, 0, 0)}};
  auto shader = Shader::MakeLinearGradient(Vector2d(0.0, 0.0), Vector2d(1.0, 0.0), std::move(stops),
                                           SpreadMode::kPad);
  ASSERT_TRUE(shader.hasValue());

  paint.shader = shader.value();
  auto context = PaintContext::Create(paint);
  ASSERT_TRUE(context.hasValue());

  const Color left = context.value().shade(Vector2d(0.0, 0.0));
  EXPECT_EQ(left, Color::RGB(0, 0, 0));

  const Color right = context.value().shade(Vector2d(1.0, 0.0));
  EXPECT_EQ(right, Color::RGB(255, 0, 0));
}

TEST(PaintContextTests, BlendsSpanIntoPixmap) {
  auto canvas = Canvas::Create(2, 1);
  ASSERT_TRUE(canvas.hasValue());
  Canvas surface = std::move(canvas.value());
  surface.clear(Color::RGB(0, 0, 255));

  Paint paint;
  paint.color = Color::RGB(255, 0, 0);
  paint.blendMode = BlendMode::kMultiply;
  auto context = PaintContext::Create(paint);
  ASSERT_TRUE(context.hasValue());

  BlendSpan(surface.pixmap(), 0, 0, 2, context.value());

  const std::span<const uint8_t> pixels = surface.pixmap().pixels();
  ASSERT_EQ(pixels.size(), 8u);
  EXPECT_EQ(pixels[0], 0);    // r
  EXPECT_EQ(pixels[1], 0);    // g
  EXPECT_EQ(pixels[2], 0);    // b
  EXPECT_EQ(pixels[3], 255);  // a

  EXPECT_EQ(pixels[4], 0);
  EXPECT_EQ(pixels[5], 0);
  EXPECT_EQ(pixels[6], 0);
  EXPECT_EQ(pixels[7], 255);
}

TEST(PaintContextTests, IgnoresOutOfBoundsSpan) {
  auto canvas = Canvas::Create(1, 1);
  ASSERT_TRUE(canvas.hasValue());
  Canvas surface = std::move(canvas.value());
  surface.clear(Color::RGB(10, 20, 30));

  Paint paint;
  auto context = PaintContext::Create(paint);
  ASSERT_TRUE(context.hasValue());

  BlendSpan(surface.pixmap(), -5, 5, 2, context.value());

  const std::span<const uint8_t> pixels = surface.pixmap().pixels();
  EXPECT_EQ(pixels[0], 10);
  EXPECT_EQ(pixels[1], 20);
  EXPECT_EQ(pixels[2], 30);
  EXPECT_EQ(pixels[3], 255);
}

TEST(PaintContextTests, BlendsMaskSpanWithCoverage) {
  auto canvas = Canvas::Create(3, 1);
  ASSERT_TRUE(canvas.hasValue());
  Canvas surface = std::move(canvas.value());
  surface.clear(Color::RGB(0, 0, 255));

  Paint paint;
  paint.color = Color::RGB(255, 0, 0);
  auto context = PaintContext::Create(paint);
  ASSERT_TRUE(context.hasValue());

  const uint8_t coverage[3] = {0, 128, 255};
  BlendMaskSpan(surface.pixmap(), 0, 0, coverage, 3, context.value());

  const std::span<const uint8_t> pixels = surface.pixmap().pixels();
  EXPECT_EQ(pixels[0], 0);  // coverage 0 keeps destination
  EXPECT_EQ(pixels[1], 0);
  EXPECT_EQ(pixels[2], 255);
  EXPECT_EQ(pixels[3], 255);

  const Color halfExpected =
      ToColor(Blend(Premultiply(Color(128, 0, 0, 128)), Premultiply(Color::RGB(0, 0, 255)),
                    BlendMode::kSourceOver));
  EXPECT_EQ(pixels[4], halfExpected.r);  // halfway coverage
  EXPECT_EQ(pixels[5], halfExpected.g);
  EXPECT_EQ(pixels[6], halfExpected.b);
  EXPECT_EQ(pixels[7], halfExpected.a);

  EXPECT_EQ(pixels[8], 255);  // full coverage
  EXPECT_EQ(pixels[9], 0);
  EXPECT_EQ(pixels[10], 0);
  EXPECT_EQ(pixels[11], 255);
}

TEST(PaintContextTests, SourceOverBlendUsesSimdHelper) {
  auto canvas = Canvas::Create(4, 1);
  ASSERT_TRUE(canvas.hasValue());
  Canvas surface = std::move(canvas.value());
  surface.clear(Color(10, 20, 30, 128));

  Paint paint;
  paint.color = Color(200, 150, 100, 192);
  paint.blendMode = BlendMode::kSourceOver;
  auto context = PaintContext::Create(paint);
  ASSERT_TRUE(context.hasValue());

  BlendSpan(surface.pixmap(), 0, 0, 4, context.value());

  const Color expected = ToColor(
      Blend(Premultiply(paint.color), Premultiply(Color(10, 20, 30, 128)), paint.blendMode));

  const std::span<const uint8_t> pixels = surface.pixmap().pixels();
  for (int pixel = 0; pixel < 4; ++pixel) {
    const size_t base = static_cast<size_t>(pixel) * 4;
    EXPECT_EQ(pixels[base], expected.r);
    EXPECT_EQ(pixels[base + 1], expected.g);
    EXPECT_EQ(pixels[base + 2], expected.b);
    EXPECT_EQ(pixels[base + 3], expected.a);
  }
}

TEST(PaintContextTests, SourceOverMaskBlendUsesSimdHelper) {
  auto canvas = Canvas::Create(2, 1);
  ASSERT_TRUE(canvas.hasValue());
  Canvas surface = std::move(canvas.value());
  surface.clear(Color::RGB(0, 0, 255));

  Paint paint;
  paint.color = Color(255, 0, 0, 200);
  paint.blendMode = BlendMode::kSourceOver;
  auto context = PaintContext::Create(paint);
  ASSERT_TRUE(context.hasValue());

  const uint8_t coverage[2] = {64, 200};
  BlendMaskSpan(surface.pixmap(), 0, 0, coverage, 2, context.value());

  const auto ScaleColor = [](const Color& color, uint8_t mask) {
    const float scale = static_cast<float>(mask) / 255.0f;
    const auto clampChannel = [scale](uint8_t channel) {
      return static_cast<uint8_t>(
          std::lround(std::clamp(static_cast<float>(channel) * scale, 0.0f, 255.0f)));
    };
    return Color(clampChannel(color.r), clampChannel(color.g), clampChannel(color.b),
                 clampChannel(color.a));
  };

  const Color firstExpected = ToColor(Blend(Premultiply(ScaleColor(paint.color, coverage[0])),
                                            Premultiply(Color::RGB(0, 0, 255)), paint.blendMode));
  const Color secondExpected = ToColor(Blend(Premultiply(ScaleColor(paint.color, coverage[1])),
                                             Premultiply(Color::RGB(0, 0, 255)), paint.blendMode));

  const std::span<const uint8_t> pixels = surface.pixmap().pixels();
  EXPECT_EQ(pixels[0], firstExpected.r);
  EXPECT_EQ(pixels[1], firstExpected.g);
  EXPECT_EQ(pixels[2], firstExpected.b);
  EXPECT_EQ(pixels[3], firstExpected.a);

  EXPECT_EQ(pixels[4], secondExpected.r);
  EXPECT_EQ(pixels[5], secondExpected.g);
  EXPECT_EQ(pixels[6], secondExpected.b);
  EXPECT_EQ(pixels[7], secondExpected.a);
}

TEST(PaintContextTests, SpanBlendMatchesPerPixelReference) {
  auto canvasFast = Canvas::Create(6, 1);
  ASSERT_TRUE(canvasFast.hasValue());
  Canvas fastSurface = std::move(canvasFast.value());
  fastSurface.clear(Color(10, 20, 30, 200));

  auto canvasReference = Canvas::Create(6, 1);
  ASSERT_TRUE(canvasReference.hasValue());
  Canvas referenceSurface = std::move(canvasReference.value());
  referenceSurface.clear(Color(10, 20, 30, 200));

  Paint paint;
  paint.blendMode = BlendMode::kSourceOver;
  paint.shader =
      Shader::MakeLinearGradient(Vector2d(0.0, 0.0), Vector2d(6.0, 0.0),
                                 {{0.0f, Color::RGB(0, 0, 0)}, {1.0f, Color(200, 100, 50, 180)}},
                                 SpreadMode::kPad)
          .value();
  auto contextResult = PaintContext::Create(paint);
  ASSERT_TRUE(contextResult.hasValue());
  const PaintContext& context = contextResult.value();

  BlendSpan(fastSurface.pixmap(), 0, 0, 6, context);

  Pixmap& reference = referenceSurface.pixmap();
  uint8_t* referenceRow = reference.data();
  for (int x = 0; x < 6; ++x) {
    const Color src = context.shade(Vector2d(static_cast<double>(x) + 0.5, 0.5));
    const size_t offset = static_cast<size_t>(x) * 4;
    const Color dest{referenceRow[offset], referenceRow[offset + 1], referenceRow[offset + 2],
                     referenceRow[offset + 3]};
    const Color blended = ToColor(Blend(Premultiply(src), Premultiply(dest), paint.blendMode));
    referenceRow[offset] = blended.r;
    referenceRow[offset + 1] = blended.g;
    referenceRow[offset + 2] = blended.b;
    referenceRow[offset + 3] = blended.a;
  }

  const std::span<const uint8_t> fastPixels = fastSurface.pixmap().pixels();
  const std::span<const uint8_t> referencePixels = referenceSurface.pixmap().pixels();
  ASSERT_EQ(fastPixels.size(), referencePixels.size());
  for (size_t i = 0; i < fastPixels.size(); ++i) {
    EXPECT_EQ(fastPixels[i], referencePixels[i]);
  }
}

TEST(PaintContextTests, SpanBlockBlendMatchesPerPixelReferenceWithoutShader) {
  auto canvasFast = Canvas::Create(8, 1);
  ASSERT_TRUE(canvasFast.hasValue());
  Canvas fastSurface = std::move(canvasFast.value());
  fastSurface.clear(Color(0, 0, 0, 0));

  auto canvasReference = Canvas::Create(8, 1);
  ASSERT_TRUE(canvasReference.hasValue());
  Canvas referenceSurface = std::move(canvasReference.value());
  referenceSurface.clear(Color(0, 0, 0, 0));

  // Seed the destination with distinct values so per-lane blending is observable.
  Pixmap& dest = fastSurface.pixmap();
  Pixmap& destReference = referenceSurface.pixmap();
  for (int i = 0; i < 8; ++i) {
    const uint8_t base = static_cast<uint8_t>(20 * i);
    uint8_t* row = dest.data();
    uint8_t* referenceRow = destReference.data();
    const size_t offset = static_cast<size_t>(i) * 4;
    row[offset] = referenceRow[offset] = static_cast<uint8_t>(base + 5);
    row[offset + 1] = referenceRow[offset + 1] = static_cast<uint8_t>(base + 10);
    row[offset + 2] = referenceRow[offset + 2] = static_cast<uint8_t>(base + 15);
    row[offset + 3] = referenceRow[offset + 3] = static_cast<uint8_t>(200 - i * 5);
  }

  Paint paint;
  paint.blendMode = BlendMode::kSourceOver;
  paint.color = Color(50, 120, 200, 180);
  paint.opacity = 0.6f;
  auto contextResult = PaintContext::Create(paint);
  ASSERT_TRUE(contextResult.hasValue());
  const PaintContext& context = contextResult.value();

  BlendSpan(dest, 0, 0, 8, context);

  for (int x = 0; x < 8; ++x) {
    BlendSpan(destReference, x, 0, 1, context);
  }

  const std::span<const uint8_t> fastPixels = dest.pixels();
  const std::span<const uint8_t> referencePixels = destReference.pixels();
  ASSERT_EQ(fastPixels.size(), referencePixels.size());
  for (size_t i = 0; i < fastPixels.size(); ++i) {
    EXPECT_EQ(fastPixels[i], referencePixels[i]);
  }
}

TEST(PaintContextTests, MaskSpanBlendMatchesPerPixelReference) {
  auto canvasFast = Canvas::Create(4, 1);
  ASSERT_TRUE(canvasFast.hasValue());
  Canvas fastSurface = std::move(canvasFast.value());
  fastSurface.clear(Color(0, 0, 255, 255));

  auto canvasReference = Canvas::Create(4, 1);
  ASSERT_TRUE(canvasReference.hasValue());
  Canvas referenceSurface = std::move(canvasReference.value());
  referenceSurface.clear(Color(0, 0, 255, 255));

  Paint paint;
  paint.blendMode = BlendMode::kSourceOver;
  paint.shader =
      Shader::MakeLinearGradient(Vector2d(0.0, 0.0), Vector2d(4.0, 0.0),
                                 {{0.0f, Color(255, 0, 0, 180)}, {1.0f, Color(0, 255, 0, 180)}},
                                 SpreadMode::kPad)
          .value();
  auto contextResult = PaintContext::Create(paint);
  ASSERT_TRUE(contextResult.hasValue());
  const PaintContext& context = contextResult.value();

  const uint8_t coverage[4] = {0, 64, 128, 255};
  BlendMaskSpan(fastSurface.pixmap(), 0, 0, coverage, 4, context);

  Pixmap& reference = referenceSurface.pixmap();
  uint8_t* referenceRow = reference.data();
  for (int x = 0; x < 4; ++x) {
    const Color src = context.shade(Vector2d(static_cast<double>(x) + 0.5, 0.5));
    const float scale = static_cast<float>(coverage[x]) / 255.0f;
    const auto ScaleChannel = [scale](uint8_t channel) {
      return static_cast<uint8_t>(
          std::lround(std::clamp(static_cast<float>(channel) * scale, 0.0f, 255.0f)));
    };
    const Color scaled{ScaleChannel(src.r), ScaleChannel(src.g), ScaleChannel(src.b),
                       ScaleChannel(src.a)};
    const size_t offset = static_cast<size_t>(x) * 4;
    const Color dest{referenceRow[offset], referenceRow[offset + 1], referenceRow[offset + 2],
                     referenceRow[offset + 3]};
    const Color blended = ToColor(Blend(Premultiply(scaled), Premultiply(dest), paint.blendMode));
    referenceRow[offset] = blended.r;
    referenceRow[offset + 1] = blended.g;
    referenceRow[offset + 2] = blended.b;
    referenceRow[offset + 3] = blended.a;
  }

  const std::span<const uint8_t> fastPixels = fastSurface.pixmap().pixels();
  const std::span<const uint8_t> referencePixels = referenceSurface.pixmap().pixels();
  ASSERT_EQ(fastPixels.size(), referencePixels.size());
  for (size_t i = 0; i < fastPixels.size(); ++i) {
    EXPECT_EQ(fastPixels[i], referencePixels[i]);
  }
}

TEST(PaintContextTests, MaskSpanBlockBlendMatchesPerPixelReferenceWithoutShader) {
  auto canvasFast = Canvas::Create(8, 1);
  ASSERT_TRUE(canvasFast.hasValue());
  Canvas fastSurface = std::move(canvasFast.value());
  fastSurface.clear(Color(40, 80, 120, 220));

  auto canvasReference = Canvas::Create(8, 1);
  ASSERT_TRUE(canvasReference.hasValue());
  Canvas referenceSurface = std::move(canvasReference.value());
  referenceSurface.clear(Color(40, 80, 120, 220));

  Paint paint;
  paint.blendMode = BlendMode::kSourceOver;
  paint.color = Color(200, 30, 60, 160);
  paint.opacity = 0.75f;
  auto contextResult = PaintContext::Create(paint);
  ASSERT_TRUE(contextResult.hasValue());
  const PaintContext& context = contextResult.value();

  const uint8_t coverage[8] = {0, 25, 50, 75, 100, 150, 200, 255};
  BlendMaskSpan(fastSurface.pixmap(), 0, 0, coverage, 8, context);

  Pixmap& referencePixmap = referenceSurface.pixmap();
  for (int x = 0; x < 8; ++x) {
    BlendMaskSpan(referencePixmap, x, 0, &coverage[x], 1, context);
  }

  const std::span<const uint8_t> fastPixels = fastSurface.pixmap().pixels();
  const std::span<const uint8_t> referencePixels = referenceSurface.pixmap().pixels();
  ASSERT_EQ(fastPixels.size(), referencePixels.size());
  for (size_t i = 0; i < fastPixels.size(); ++i) {
    EXPECT_EQ(fastPixels[i], referencePixels[i]);
  }
}

TEST(PaintContextTests, RandomizedMaskSpansMatchReference) {
  std::mt19937 rng(1337);
  std::uniform_int_distribution<int> widthDist(1, 32);
  std::uniform_int_distribution<int> channelDist(0, 255);
  std::uniform_real_distribution<float> opacityDist(0.0f, 1.0f);

  for (int iteration = 0; iteration < 50; ++iteration) {
    const int width = widthDist(rng);
    SCOPED_TRACE(testing::Message() << "iteration " << iteration << ", width " << width);

    auto canvasFast = Canvas::Create(width, 1);
    ASSERT_TRUE(canvasFast.hasValue());
    Canvas fastSurface = std::move(canvasFast.value());

    auto canvasReference = Canvas::Create(width, 1);
    ASSERT_TRUE(canvasReference.hasValue());
    Canvas referenceSurface = std::move(canvasReference.value());

    const Color destColor(channelDist(rng), channelDist(rng), channelDist(rng), channelDist(rng));
    fastSurface.clear(destColor);
    referenceSurface.clear(destColor);

    Paint paint;
    paint.blendMode = BlendMode::kSourceOver;
    paint.opacity = opacityDist(rng);

    const Color startColor(channelDist(rng), channelDist(rng), channelDist(rng), channelDist(rng));
    const Color endColor(channelDist(rng), channelDist(rng), channelDist(rng), channelDist(rng));
    paint.shader =
        Shader::MakeLinearGradient(Vector2d(0.0, 0.0), Vector2d(static_cast<double>(width), 0.0),
                                   {{0.0f, startColor}, {1.0f, endColor}}, SpreadMode::kPad)
            .value();

    auto contextResult = PaintContext::Create(paint);
    ASSERT_TRUE(contextResult.hasValue());
    const PaintContext& context = contextResult.value();

    std::vector<uint8_t> coverage(static_cast<size_t>(width));
    for (uint8_t& value : coverage) {
      value = static_cast<uint8_t>(channelDist(rng));
    }

    BlendMaskSpan(fastSurface.pixmap(), 0, 0, coverage.data(), width, context);

    for (int x = 0; x < width; ++x) {
      const uint8_t* pixelCoverage = &coverage[static_cast<size_t>(x)];
      BlendMaskSpan(referenceSurface.pixmap(), x, 0, pixelCoverage, 1, context);
    }

    const std::span<const uint8_t> fastPixels = fastSurface.pixmap().pixels();
    const std::span<const uint8_t> referencePixels = referenceSurface.pixmap().pixels();
    ASSERT_EQ(fastPixels.size(), referencePixels.size());
    for (size_t i = 0; i < fastPixels.size(); ++i) {
      const int difference =
          std::abs(static_cast<int>(fastPixels[i]) - static_cast<int>(referencePixels[i]));
      EXPECT_LE(difference, 1) << "pixel index " << i;
    }
  }
}

TEST(PaintContextTests, SolidSourceOverSpanFastPathMatchesReference) {
  std::mt19937 rng(7);
  std::uniform_int_distribution<int> widthDist(4, 32);
  std::uniform_int_distribution<int> channelDist(0, 255);
  std::uniform_real_distribution<float> opacityDist(0.0f, 1.0f);

  for (int iteration = 0; iteration < 20; ++iteration) {
    const int width = widthDist(rng);

    auto fastCanvasResult = Canvas::Create(width, 1);
    ASSERT_TRUE(fastCanvasResult.hasValue());
    Canvas fastCanvas = std::move(fastCanvasResult.value());

    auto referenceCanvasResult = Canvas::Create(width, 1);
    ASSERT_TRUE(referenceCanvasResult.hasValue());
    Canvas referenceCanvas = std::move(referenceCanvasResult.value());

    for (int x = 0; x < width; ++x) {
      const size_t offset = static_cast<size_t>(x) * 4;
      uint8_t* fastRow = fastCanvas.pixmap().data();
      uint8_t* referenceRow = referenceCanvas.pixmap().data();
      fastRow[offset] = referenceRow[offset] = static_cast<uint8_t>(channelDist(rng));
      fastRow[offset + 1] = referenceRow[offset + 1] = static_cast<uint8_t>(channelDist(rng));
      fastRow[offset + 2] = referenceRow[offset + 2] = static_cast<uint8_t>(channelDist(rng));
      fastRow[offset + 3] = referenceRow[offset + 3] = static_cast<uint8_t>(channelDist(rng));
    }

    Paint paint;
    paint.color = Color(static_cast<uint8_t>(channelDist(rng)),
                        static_cast<uint8_t>(channelDist(rng)),
                        static_cast<uint8_t>(channelDist(rng)),
                        static_cast<uint8_t>(channelDist(rng)));
    paint.opacity = opacityDist(rng);

    auto contextResult = PaintContext::Create(paint);
    ASSERT_TRUE(contextResult.hasValue());
    const PaintContext& context = contextResult.value();

    BlendSpan(fastCanvas.pixmap(), 0, 0, width, context);

    uint8_t* referenceRow = referenceCanvas.pixmap().data();
    for (int x = 0; x < width; ++x) {
      const size_t offset = static_cast<size_t>(x) * 4;
      const Color dest{referenceRow[offset], referenceRow[offset + 1], referenceRow[offset + 2],
                       referenceRow[offset + 3]};
      const Color src = context.shade(Vector2d(static_cast<double>(x) + 0.5, 0.5));
      const Color blended =
          ToColor(Blend(Premultiply(src), Premultiply(dest), paint.blendMode));
      referenceRow[offset] = blended.r;
      referenceRow[offset + 1] = blended.g;
      referenceRow[offset + 2] = blended.b;
      referenceRow[offset + 3] = blended.a;
    }

    const std::span<const uint8_t> fastPixels = fastCanvas.pixmap().pixels();
    const std::span<const uint8_t> referencePixels = referenceCanvas.pixmap().pixels();
    ASSERT_EQ(fastPixels.size(), referencePixels.size());
    for (size_t i = 0; i < fastPixels.size(); ++i) {
      EXPECT_EQ(fastPixels[i], referencePixels[i]) << "pixel index " << i;
    }
  }
}

TEST(PaintContextTests, SolidSourceOverMaskFastPathMatchesReference) {
  std::mt19937 rng(9);
  std::uniform_int_distribution<int> widthDist(4, 32);
  std::uniform_int_distribution<int> channelDist(0, 255);
  std::uniform_int_distribution<int> coverageDist(0, 255);
  std::uniform_real_distribution<float> opacityDist(0.0f, 1.0f);

  for (int iteration = 0; iteration < 20; ++iteration) {
    const int width = widthDist(rng);

    auto fastCanvasResult = Canvas::Create(width, 1);
    ASSERT_TRUE(fastCanvasResult.hasValue());
    Canvas fastCanvas = std::move(fastCanvasResult.value());

    auto referenceCanvasResult = Canvas::Create(width, 1);
    ASSERT_TRUE(referenceCanvasResult.hasValue());
    Canvas referenceCanvas = std::move(referenceCanvasResult.value());

    std::vector<uint8_t> coverage(static_cast<size_t>(width));
    for (int x = 0; x < width; ++x) {
      const size_t offset = static_cast<size_t>(x) * 4;
      uint8_t* fastRow = fastCanvas.pixmap().data();
      uint8_t* referenceRow = referenceCanvas.pixmap().data();
      fastRow[offset] = referenceRow[offset] = static_cast<uint8_t>(channelDist(rng));
      fastRow[offset + 1] = referenceRow[offset + 1] = static_cast<uint8_t>(channelDist(rng));
      fastRow[offset + 2] = referenceRow[offset + 2] = static_cast<uint8_t>(channelDist(rng));
      fastRow[offset + 3] = referenceRow[offset + 3] = static_cast<uint8_t>(channelDist(rng));
      coverage[static_cast<size_t>(x)] = static_cast<uint8_t>(coverageDist(rng));
    }

    Paint paint;
    paint.color = Color(static_cast<uint8_t>(channelDist(rng)),
                        static_cast<uint8_t>(channelDist(rng)),
                        static_cast<uint8_t>(channelDist(rng)),
                        static_cast<uint8_t>(channelDist(rng)));
    paint.opacity = opacityDist(rng);

    auto contextResult = PaintContext::Create(paint);
    ASSERT_TRUE(contextResult.hasValue());
    const PaintContext& context = contextResult.value();

    BlendMaskSpan(fastCanvas.pixmap(), 0, 0, coverage.data(), width, context);

    uint8_t* referenceRow = referenceCanvas.pixmap().data();
    for (int x = 0; x < width; ++x) {
      const size_t offset = static_cast<size_t>(x) * 4;
      const Color dest{referenceRow[offset], referenceRow[offset + 1], referenceRow[offset + 2],
                       referenceRow[offset + 3]};
      const Color src = context.shade(Vector2d(static_cast<double>(x) + 0.5, 0.5));
      const float maskScale = static_cast<float>(coverage[static_cast<size_t>(x)]) / 255.0f;
      const auto ScaleChannel = [maskScale](uint8_t channel) {
        return static_cast<uint8_t>(std::lround(
            std::clamp(static_cast<float>(channel) * maskScale, 0.0f, 255.0f)));
      };
      const Color covered{ScaleChannel(src.r), ScaleChannel(src.g), ScaleChannel(src.b),
                          ScaleChannel(src.a)};
      const Color blended =
          ToColor(Blend(Premultiply(covered), Premultiply(dest), paint.blendMode));
      referenceRow[offset] = blended.r;
      referenceRow[offset + 1] = blended.g;
      referenceRow[offset + 2] = blended.b;
      referenceRow[offset + 3] = blended.a;
    }

    const std::span<const uint8_t> fastPixels = fastCanvas.pixmap().pixels();
    const std::span<const uint8_t> referencePixels = referenceCanvas.pixmap().pixels();
    ASSERT_EQ(fastPixels.size(), referencePixels.size());
    for (size_t i = 0; i < fastPixels.size(); ++i) {
      EXPECT_EQ(fastPixels[i], referencePixels[i]) << "pixel index " << i;
    }
  }
}

TEST(PaintContextTests, RandomizedSpanBlocksMatchReference) {
  std::mt19937 rng(2024);
  std::uniform_int_distribution<int> widthDist(1, 32);
  std::uniform_int_distribution<int> channelDist(0, 255);
  std::uniform_real_distribution<float> opacityDist(0.0f, 1.0f);

  for (int iteration = 0; iteration < 40; ++iteration) {
    const int width = widthDist(rng);
    SCOPED_TRACE(testing::Message() << "iteration " << iteration << ", width " << width);

    auto canvasFast = Canvas::Create(width, 1);
    ASSERT_TRUE(canvasFast.hasValue());
    Canvas fastSurface = std::move(canvasFast.value());

    auto canvasReference = Canvas::Create(width, 1);
    ASSERT_TRUE(canvasReference.hasValue());
    Canvas referenceSurface = std::move(canvasReference.value());

    for (int x = 0; x < width; ++x) {
      const uint8_t base = static_cast<uint8_t>(channelDist(rng));
      const size_t offset = static_cast<size_t>(x) * 4;
      uint8_t* fastRow = fastSurface.pixmap().data();
      uint8_t* referenceRow = referenceSurface.pixmap().data();
      fastRow[offset] = referenceRow[offset] = base;
      fastRow[offset + 1] = referenceRow[offset + 1] = static_cast<uint8_t>(channelDist(rng));
      fastRow[offset + 2] = referenceRow[offset + 2] = static_cast<uint8_t>(channelDist(rng));
      fastRow[offset + 3] = referenceRow[offset + 3] = static_cast<uint8_t>(channelDist(rng));
    }

    Paint paint;
    paint.blendMode = BlendMode::kSourceOver;
    paint.opacity = opacityDist(rng);
    const Color startColor(channelDist(rng), channelDist(rng), channelDist(rng), channelDist(rng));
    const Color endColor(channelDist(rng), channelDist(rng), channelDist(rng), channelDist(rng));
    paint.shader =
        Shader::MakeLinearGradient(Vector2d(0.0, 0.0), Vector2d(static_cast<double>(width), 0.0),
                                   {{0.0f, startColor}, {1.0f, endColor}}, SpreadMode::kPad)
            .value();

    auto contextResult = PaintContext::Create(paint);
    ASSERT_TRUE(contextResult.hasValue());
    const PaintContext& context = contextResult.value();

    BlendSpan(fastSurface.pixmap(), 0, 0, width, context);

    uint8_t* referenceRow = referenceSurface.pixmap().data();
    for (int x = 0; x < width; ++x) {
      const size_t offset = static_cast<size_t>(x) * 4;
      const Color dest{referenceRow[offset], referenceRow[offset + 1], referenceRow[offset + 2],
                       referenceRow[offset + 3]};
      const Color src =
          context.shade(Vector2d(static_cast<double>(x) + 0.5, static_cast<double>(0) + 0.5));
      const Color blended =
          ToColor(Blend(Premultiply(src), Premultiply(dest), paint.blendMode));
      referenceRow[offset] = blended.r;
      referenceRow[offset + 1] = blended.g;
      referenceRow[offset + 2] = blended.b;
      referenceRow[offset + 3] = blended.a;
    }

    const std::span<const uint8_t> fastPixels = fastSurface.pixmap().pixels();
    const std::span<const uint8_t> referencePixels = referenceSurface.pixmap().pixels();
    ASSERT_EQ(fastPixels.size(), referencePixels.size());
    for (size_t i = 0; i < fastPixels.size(); ++i) {
      EXPECT_EQ(fastPixels[i], referencePixels[i]) << "pixel index " << i;
    }
  }
}

}  // namespace donner::backends::tiny_skia_cpp
