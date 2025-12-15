#include "donner/backends/tiny_skia_cpp/Canvas.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "donner/backends/tiny_skia_cpp/ImageIO.h"
#include "donner/backends/tiny_skia_cpp/Rasterizer.h"
#include "donner/backends/tiny_skia_cpp/Stroke.h"
#include "tools/cpp/runfiles/runfiles.h"

namespace donner::backends::tiny_skia_cpp {
namespace {

std::string ResolveRunfile(std::string_view path) {
  using ::bazel::tools::cpp::runfiles::Runfiles;

  std::string error;
  std::unique_ptr<Runfiles> runfiles(Runfiles::CreateForTest(&error));
  EXPECT_TRUE(error.empty()) << error;
  EXPECT_NE(runfiles, nullptr);
  if (!runfiles) {
    return std::string(path);
  }
  const char* workspace = std::getenv("TEST_WORKSPACE");
  const std::string prefix = workspace != nullptr ? workspace : "donner";
  const std::string resolved = runfiles->Rlocation(prefix + "/" + std::string(path));
  EXPECT_FALSE(resolved.empty());
  return resolved;
}

std::vector<uint8_t> Premultiply(const Pixmap& pixmap) {
  std::vector<uint8_t> premultiplied;
  const auto pixels = pixmap.pixels();
  premultiplied.reserve(pixels.size());

  for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
    const uint8_t a = pixels[i + 3];
    const auto premul = [&](uint8_t c) {
      return static_cast<uint8_t>((static_cast<uint16_t>(c) * a + 127) / 255);
    };

    premultiplied.push_back(premul(pixels[i]));
    premultiplied.push_back(premul(pixels[i + 1]));
    premultiplied.push_back(premul(pixels[i + 2]));
    premultiplied.push_back(a);
  }

  return premultiplied;
}

struct CoverageBounds {
  int minX = 0;
  int minY = 0;
  int maxX = 0;
  int maxY = 0;
};

std::optional<CoverageBounds> FindCoverageBounds(std::span<const uint8_t> pixels, int width,
                                                 int height) {
  int minX = width;
  int minY = height;
  int maxX = -1;
  int maxY = -1;

  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      const size_t idx = static_cast<size_t>(y * width + x) * 4 + 3;
      if (pixels[idx] > 0) {
        minX = std::min(minX, x);
        minY = std::min(minY, y);
        maxX = std::max(maxX, x);
        maxY = std::max(maxY, y);
      }
    }
  }

  if (maxX < minX || maxY < minY) {
    return std::nullopt;
  }

  return CoverageBounds{minX, minY, maxX, maxY};
}

void ExpectNearGolden(const Pixmap& actualPixmap, const Pixmap& goldenPixmap,
                      size_t maxOverOneCount = 1000u, uint8_t maxDifference = 200,
                      int boundsTolerance = 1) {
  ASSERT_EQ(goldenPixmap.width(), actualPixmap.width());
  ASSERT_EQ(goldenPixmap.height(), actualPixmap.height());

  const auto actual = actualPixmap.pixels();
  const auto expectedPremultiplied = Premultiply(goldenPixmap);
  const std::span<const uint8_t> expected(expectedPremultiplied.data(),
                                          expectedPremultiplied.size());
  ASSERT_EQ(actual.size(), expected.size());

  const auto actualBounds =
      FindCoverageBounds(actual, actualPixmap.width(), actualPixmap.height());
  const auto expectedBounds =
      FindCoverageBounds(expected, goldenPixmap.width(), goldenPixmap.height());
  ASSERT_TRUE(actualBounds.has_value());
  ASSERT_TRUE(expectedBounds.has_value());
  EXPECT_LE(std::abs(actualBounds->minX - expectedBounds->minX), boundsTolerance);
  EXPECT_LE(std::abs(actualBounds->minY - expectedBounds->minY), boundsTolerance);
  EXPECT_LE(std::abs(actualBounds->maxX - expectedBounds->maxX), boundsTolerance);
  EXPECT_LE(std::abs(actualBounds->maxY - expectedBounds->maxY), boundsTolerance);

  size_t overOneCount = 0;
  size_t firstLargeDelta = 0;
  uint8_t firstActual = 0;
  uint8_t firstExpected = 0;
  uint8_t maxDelta = 0;
  for (size_t i = 0; i < actual.size(); ++i) {
    const uint8_t diff = static_cast<uint8_t>(std::abs(static_cast<int>(actual[i]) -
                                                       static_cast<int>(expected[i])));
    maxDelta = std::max(maxDelta, diff);
    if (diff > 1) {
      if (overOneCount == 0) {
        firstLargeDelta = i;
        firstActual = actual[i];
        firstExpected = expected[i];
      }
      ++overOneCount;
    }
  }

  EXPECT_LT(overOneCount, maxOverOneCount)
      << "First diff > 1 at index " << firstLargeDelta << ": actual="
      << static_cast<int>(firstActual) << " expected=" << static_cast<int>(firstExpected);
  EXPECT_LE(maxDelta, maxDifference);
}

}  // namespace

TEST(ExpectedTests, HoldsValueAndError) {
  auto success = Expected<int, std::string>::Success(5);
  ASSERT_TRUE(success.hasValue());
  EXPECT_EQ(success.value(), 5);
  EXPECT_EQ(success.valueOr(3), 5);

  auto failure = Expected<int, std::string>::Failure("boom");
  ASSERT_FALSE(failure.hasValue());
  EXPECT_EQ(failure.error(), "boom");
  EXPECT_EQ(failure.valueOr(7), 7);
}

TEST(CanvasTests, ClearsCanvasToColor) {
  auto canvasResult = Canvas::Create(2, 2);
  ASSERT_TRUE(canvasResult);

  Canvas canvas = std::move(canvasResult.value());
  const Color magenta{0xFF, 0x00, 0xFF, 0x80};
  canvas.clear(magenta);

  const auto pixels = canvas.pixmap().pixels();
  for (int i = 0; i < 4; ++i) {
    const size_t base = static_cast<size_t>(i) * 4;
    EXPECT_EQ(pixels[base], magenta.r);
    EXPECT_EQ(pixels[base + 1], magenta.g);
    EXPECT_EQ(pixels[base + 2], magenta.b);
    EXPECT_EQ(pixels[base + 3], magenta.a);
  }
}

TEST(ImageIOTests, RoundTripsPngData) {
  auto canvasResult = Canvas::Create(3, 2);
  ASSERT_TRUE(canvasResult);

  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color::RGB(0x12, 0x34, 0x56));

  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path tempFile = std::filesystem::temp_directory_path() /
                                         ("tiny_skia_cpp_" + std::to_string(timestamp) + ".png");

  auto writeResult = ImageIO::WriteRgbaPng(tempFile.string(), canvas.pixmap());
  ASSERT_TRUE(writeResult);

  auto loadResult = ImageIO::LoadRgbaPng(tempFile.string());
  ASSERT_TRUE(loadResult);

  const Pixmap& loaded = loadResult.value();
  EXPECT_EQ(loaded.width(), canvas.width());
  EXPECT_EQ(loaded.height(), canvas.height());

  const Pixmap& expectedPixmap = canvas.pixmap();
  const auto loadedPixels = loaded.pixels();
  const auto expectedPixels = expectedPixmap.pixels();
  ASSERT_EQ(loadedPixels.size(), expectedPixels.size());
  EXPECT_TRUE(std::equal(loadedPixels.begin(), loadedPixels.end(), expectedPixels.begin()));

  std::filesystem::remove(tempFile);
}

TEST(CanvasTests, DrawsFilledPath) {
  auto canvasResult = Canvas::Create(4, 4);
  ASSERT_TRUE(canvasResult);

  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color::RGB(0, 0, 0));

  svg::PathSpline spline;
  spline.moveTo({1.0, 1.0});
  spline.lineTo({3.0, 1.0});
  spline.lineTo({3.0, 3.0});
  spline.lineTo({1.0, 3.0});
  spline.closePath();

  Paint paint;
  paint.color = Color::RGB(10, 20, 30);
  auto result = canvas.drawPath(spline, paint);
  ASSERT_TRUE(result.hasValue()) << result.error();

  const auto pixels = canvas.pixmap().pixels();
  std::vector<uint8_t> expected;
  auto append = [&expected](std::initializer_list<uint8_t> row) {
    expected.insert(expected.end(), row);
  };

  // Expect the middle 2x2 to be painted and the rest clear.
  append({0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255});
  append({0, 0, 0, 255, 10, 20, 30, 255, 10, 20, 30, 255, 0, 0, 0, 255});
  append({0, 0, 0, 255, 10, 20, 30, 255, 10, 20, 30, 255, 0, 0, 0, 255});
  append({0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255});

  EXPECT_EQ(std::vector<uint8_t>(pixels.begin(), pixels.end()), expected);
}

TEST(CanvasTests, StrokesPath) {
  auto canvasResult = Canvas::Create(5, 4);
  ASSERT_TRUE(canvasResult);

  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color::RGB(0, 0, 0));

  svg::PathSpline spline;
  spline.moveTo({1.0, 1.0});
  spline.lineTo({4.0, 1.0});

  Stroke stroke;
  stroke.width = 2.0f;
  stroke.lineCap = LineCap::kSquare;

  Paint paint;
  paint.color = Color::RGB(80, 90, 100);
  auto result = canvas.strokePath(spline, stroke, paint);
  ASSERT_TRUE(result.hasValue()) << result.error();

  const auto pixels = canvas.pixmap().pixels();
  std::vector<uint8_t> expected;
  auto append = [&expected](std::initializer_list<uint8_t> row) {
    expected.insert(expected.end(), row);
  };

  append(
      {80, 90, 100, 255, 80, 90, 100, 255, 80, 90, 100, 255, 80, 90, 100, 255, 80, 90, 100, 255});
  append(
      {80, 90, 100, 255, 80, 90, 100, 255, 80, 90, 100, 255, 80, 90, 100, 255, 80, 90, 100, 255});
  append({0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255});
  append({0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255, 0, 0, 0, 255});

  EXPECT_EQ(std::vector<uint8_t>(pixels.begin(), pixels.end()), expected);
}

TEST(CanvasTests, SavesDrawnContentToPng) {
  auto canvasResult = Canvas::Create(3, 3);
  ASSERT_TRUE(canvasResult);

  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color::RGB(0, 0, 0));

  svg::PathSpline fill;
  fill.moveTo({0.0, 0.0});
  fill.lineTo({3.0, 0.0});
  fill.lineTo({3.0, 3.0});
  fill.lineTo({0.0, 3.0});
  fill.closePath();

  Paint paint;
  paint.color = Color::RGB(30, 40, 50);
  ASSERT_TRUE(canvas.drawPath(fill, paint).hasValue());

  svg::PathSpline strokePath;
  strokePath.moveTo({0.5, 1.5});
  strokePath.lineTo({2.5, 1.5});

  Stroke stroke;
  stroke.width = 1.0f;
  stroke.lineCap = LineCap::kSquare;
  Paint strokePaint;
  strokePaint.color = Color::RGB(200, 10, 20);
  ASSERT_TRUE(canvas.strokePath(strokePath, stroke, strokePaint).hasValue());

  const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
  const std::filesystem::path tempFile =
      std::filesystem::temp_directory_path() /
      ("tiny_skia_cpp_draw_" + std::to_string(timestamp) + ".png");

  auto writeResult = ImageIO::WriteRgbaPng(tempFile.string(), canvas.pixmap());
  ASSERT_TRUE(writeResult.hasValue());

  auto loadResult = ImageIO::LoadRgbaPng(tempFile.string());
  ASSERT_TRUE(loadResult.hasValue());
  const Pixmap& loaded = loadResult.value();

  EXPECT_EQ(loaded.width(), canvas.width());
  EXPECT_EQ(loaded.height(), canvas.height());
  EXPECT_EQ(std::vector<uint8_t>(loaded.pixels().begin(), loaded.pixels().end()),
            std::vector<uint8_t>(canvas.pixmap().pixels().begin(), canvas.pixmap().pixels().end()));

  std::filesystem::remove(tempFile);
}

TEST(CanvasGoldens, LinearGradientMatchesRustTinySkia) {
  auto canvasResult = Canvas::Create(200, 200);
  ASSERT_TRUE(canvasResult);

  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color{0, 0, 0, 0});

  Paint paint;
  paint.antiAlias = false;
  auto shader =
      Shader::MakeLinearGradient({10.0, 10.0}, {190.0, 190.0},
                                 {{0.0f, Color{50, 127, 150, 200}},
                                  {1.0f, Color{220, 140, 75, 180}}},
                                 SpreadMode::kPad);
  ASSERT_TRUE(shader.hasValue()) << shader.error();
  paint.shader = shader.value();

  svg::PathSpline spline;
  spline.moveTo({10.0, 10.0});
  spline.lineTo({190.0, 10.0});
  spline.lineTo({190.0, 190.0});
  spline.lineTo({10.0, 190.0});
  spline.closePath();

  auto drawResult = canvas.drawPath(spline, paint);
  ASSERT_TRUE(drawResult.hasValue()) << drawResult.error();

  const std::string goldenPath =
      ResolveRunfile("third_party/tiny-skia/tests/images/gradients/two-stops-linear-pad-lq.png");
  ASSERT_TRUE(std::filesystem::exists(goldenPath));
  auto golden = ImageIO::LoadRgbaPng(goldenPath);
  ASSERT_TRUE(golden.hasValue()) << golden.error().message;

  ExpectNearGolden(canvas.pixmap(), golden.value(), 120000u, 255);
}

TEST(CanvasGoldens, LinearGradientRepeatMatchesRustTinySkia) {
  auto canvasResult = Canvas::Create(200, 200);
  ASSERT_TRUE(canvasResult);

  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color{0, 0, 0, 0});

  Paint paint;
  paint.antiAlias = false;
  auto shader =
      Shader::MakeLinearGradient({10.0, 10.0}, {190.0, 190.0},
                                 {{0.0f, Color{50, 127, 150, 200}},
                                  {1.0f, Color{220, 140, 75, 180}}},
                                 SpreadMode::kRepeat);
  ASSERT_TRUE(shader.hasValue()) << shader.error();
  paint.shader = shader.value();

  svg::PathSpline spline;
  spline.moveTo({10.0, 10.0});
  spline.lineTo({190.0, 10.0});
  spline.lineTo({190.0, 190.0});
  spline.lineTo({10.0, 190.0});
  spline.closePath();

  auto drawResult = canvas.drawPath(spline, paint);
  ASSERT_TRUE(drawResult.hasValue()) << drawResult.error();

  const std::string goldenPath =
      ResolveRunfile("third_party/tiny-skia/tests/images/gradients/two-stops-linear-repeat-lq.png");
  ASSERT_TRUE(std::filesystem::exists(goldenPath));
  auto golden = ImageIO::LoadRgbaPng(goldenPath);
  ASSERT_TRUE(golden.hasValue()) << golden.error().message;

  ExpectNearGolden(canvas.pixmap(), golden.value(), 120000u, 255);
}

TEST(CanvasGoldens, LinearGradientReflectMatchesRustTinySkia) {
  auto canvasResult = Canvas::Create(200, 200);
  ASSERT_TRUE(canvasResult);

  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color{0, 0, 0, 0});

  Paint paint;
  paint.antiAlias = false;
  auto shader =
      Shader::MakeLinearGradient({10.0, 10.0}, {100.0, 100.0},
                                 {{0.0f, Color{50, 127, 150, 200}},
                                  {1.0f, Color{220, 140, 75, 180}}},
                                 SpreadMode::kReflect);
  ASSERT_TRUE(shader.hasValue()) << shader.error();
  paint.shader = shader.value();

  svg::PathSpline spline;
  spline.moveTo({10.0, 10.0});
  spline.lineTo({190.0, 10.0});
  spline.lineTo({190.0, 190.0});
  spline.lineTo({10.0, 190.0});
  spline.closePath();

  auto drawResult = canvas.drawPath(spline, paint);
  ASSERT_TRUE(drawResult.hasValue()) << drawResult.error();

  const std::string goldenPath = ResolveRunfile(
      "third_party/tiny-skia/tests/images/gradients/two-stops-linear-reflect-lq.png");
  ASSERT_TRUE(std::filesystem::exists(goldenPath));
  auto golden = ImageIO::LoadRgbaPng(goldenPath);
  ASSERT_TRUE(golden.hasValue()) << golden.error().message;

  ExpectNearGolden(canvas.pixmap(), golden.value(), 120000u, 255);
}

TEST(CanvasGoldens, StrokeMatchesRustTinySkia) {
  auto canvasResult = Canvas::Create(200, 200);
  ASSERT_TRUE(canvasResult);

  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color{0, 0, 0, 0});

  Paint paint;
  paint.color = Color{50, 127, 150, 200};
  paint.antiAlias = true;

  svg::PathSpline spline;
  spline.moveTo({60.0 / 16.0, 100.0 / 16.0});
  spline.lineTo({140.0 / 16.0, 100.0 / 16.0});

  Stroke stroke;
  stroke.width = 6.0f;
  stroke.lineCap = LineCap::kRound;

  const Transform transform = Transform::Scale(16.0, 16.0);

  auto drawResult = canvas.strokePath(spline, stroke, paint, transform);
  ASSERT_TRUE(drawResult.hasValue()) << drawResult.error();

  const std::string goldenPath =
      ResolveRunfile("third_party/tiny-skia/tests/images/stroke/round-caps-and-large-scale.png");
  ASSERT_TRUE(std::filesystem::exists(goldenPath));
  auto golden = ImageIO::LoadRgbaPng(goldenPath);
  ASSERT_TRUE(golden.hasValue()) << golden.error().message;

  ExpectNearGolden(canvas.pixmap(), golden.value(), 4000u, 255);
}

TEST(CanvasGoldens, ClipMaskMatchesRustTinySkia) {
  auto canvasResult = Canvas::Create(100, 100);
  ASSERT_TRUE(canvasResult);

  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color{0, 0, 0, 0});

  svg::PathSpline clipSpline;
  clipSpline.moveTo({10.0, 10.0});
  clipSpline.lineTo({90.0, 10.0});
  clipSpline.lineTo({90.0, 90.0});
  clipSpline.lineTo({10.0, 90.0});
  clipSpline.closePath();

  Mask clipMask =
      RasterizeFill(clipSpline, canvas.width(), canvas.height(), FillRule::kNonZero, false,
                    Transform());
  ASSERT_TRUE(clipMask.isValid());

  Paint paint;
  paint.color = Color{50, 127, 150, 200};
  paint.antiAlias = false;

  svg::PathSpline fillSpline;
  fillSpline.moveTo({0.0, 0.0});
  fillSpline.lineTo({100.0, 0.0});
  fillSpline.lineTo({100.0, 100.0});
  fillSpline.lineTo({0.0, 100.0});
  fillSpline.closePath();

  auto drawResult =
      canvas.drawPath(fillSpline, paint, FillRule::kNonZero, Transform(), &clipMask);
  ASSERT_TRUE(drawResult.hasValue()) << drawResult.error();

  const std::string goldenPath = ResolveRunfile("third_party/tiny-skia/tests/images/mask/rect.png");
  ASSERT_TRUE(std::filesystem::exists(goldenPath));
  auto golden = ImageIO::LoadRgbaPng(goldenPath);
  ASSERT_TRUE(golden.hasValue()) << golden.error().message;

  ExpectNearGolden(canvas.pixmap(), golden.value());
}

TEST(CanvasGoldens, DrawPixmapMatchesRustTinySkia) {
  Pixmap source = Pixmap::Create(100, 100);
  ASSERT_TRUE(source.isValid());

  Paint fillPaint;
  fillPaint.color = Color{50, 127, 150, 200};
  fillPaint.antiAlias = false;

  svg::PathSpline rect;
  rect.moveTo({0.0, 50.0});
  rect.lineTo({100.0, 50.0});
  rect.lineTo({100.0, 100.0});
  rect.lineTo({0.0, 100.0});
  rect.closePath();

  auto fillResult = FillPath(rect, fillPaint, source);
  ASSERT_TRUE(fillResult.hasValue()) << fillResult.error();

  auto canvasResult = Canvas::Create(200, 200);
  ASSERT_TRUE(canvasResult);
  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color{0, 0, 0, 0});

  PixmapPaint pixmapPaint;
  pixmapPaint.quality = FilterQuality::kBicubic;

  auto drawResult = canvas.drawPixmap(20, 20, source, pixmapPaint);
  ASSERT_TRUE(drawResult.hasValue()) << drawResult.error();

  const std::string goldenPath =
      ResolveRunfile("third_party/tiny-skia/tests/images/canvas/draw-pixmap.png");
  ASSERT_TRUE(std::filesystem::exists(goldenPath));
  auto golden = ImageIO::LoadRgbaPng(goldenPath);
  ASSERT_TRUE(golden.hasValue()) << golden.error().message;

  ExpectNearGolden(canvas.pixmap(), golden.value(), 20000u, 255);
}

TEST(CanvasGoldens, DrawPixmapTransformMatchesRustTinySkia) {
  Pixmap source = Pixmap::Create(100, 100);
  ASSERT_TRUE(source.isValid());

  Paint fillPaint;
  fillPaint.color = Color{50, 127, 150, 200};
  fillPaint.antiAlias = true;

  svg::PathSpline triangle;
  triangle.moveTo({0.0, 100.0});
  triangle.lineTo({100.0, 100.0});
  triangle.lineTo({50.0, 0.0});
  triangle.closePath();

  auto fillResult = FillPath(triangle, fillPaint, source);
  ASSERT_TRUE(fillResult.hasValue()) << fillResult.error();

  auto canvasResult = Canvas::Create(200, 200);
  ASSERT_TRUE(canvasResult);
  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color{0, 0, 0, 0});

  PixmapPaint pixmapPaint;
  pixmapPaint.quality = FilterQuality::kBicubic;

  Transform transform;
  transform.data[0] = 1.2;
  transform.data[1] = 0.5;
  transform.data[2] = 0.5;
  transform.data[3] = 1.2;
  transform.data[4] = 0.0;
  transform.data[5] = 0.0;

  auto drawResult = canvas.drawPixmap(5, 10, source, pixmapPaint, transform);
  ASSERT_TRUE(drawResult.hasValue()) << drawResult.error();

  const std::string goldenPath =
      ResolveRunfile("third_party/tiny-skia/tests/images/canvas/draw-pixmap-ts.png");
  ASSERT_TRUE(std::filesystem::exists(goldenPath));
  auto golden = ImageIO::LoadRgbaPng(goldenPath);
  ASSERT_TRUE(golden.hasValue()) << golden.error().message;

  ExpectNearGolden(canvas.pixmap(), golden.value(), 40000u, 255, 8);
}

TEST(CanvasGoldens, DrawPixmapOpacityMatchesRustTinySkia) {
  Pixmap source = Pixmap::Create(100, 100);
  ASSERT_TRUE(source.isValid());

  Paint fillPaint;
  fillPaint.color = Color{50, 127, 150, 200};
  fillPaint.antiAlias = true;

  svg::PathSpline triangle;
  triangle.moveTo({0.0, 100.0});
  triangle.lineTo({100.0, 100.0});
  triangle.lineTo({50.0, 0.0});
  triangle.closePath();

  auto fillResult = FillPath(triangle, fillPaint, source);
  ASSERT_TRUE(fillResult.hasValue()) << fillResult.error();

  auto canvasResult = Canvas::Create(200, 200);
  ASSERT_TRUE(canvasResult);
  Canvas canvas = std::move(canvasResult.value());
  canvas.clear(Color{0, 0, 0, 0});

  PixmapPaint pixmapPaint;
  pixmapPaint.quality = FilterQuality::kBicubic;
  pixmapPaint.opacity = 0.5f;

  Transform transform;
  transform.data[0] = 1.2;
  transform.data[1] = 0.5;
  transform.data[2] = 0.5;
  transform.data[3] = 1.2;
  transform.data[4] = 0.0;
  transform.data[5] = 0.0;

  auto drawResult = canvas.drawPixmap(5, 10, source, pixmapPaint, transform);
  ASSERT_TRUE(drawResult.hasValue()) << drawResult.error();

  const std::string goldenPath =
      ResolveRunfile("third_party/tiny-skia/tests/images/canvas/draw-pixmap-opacity.png");
  ASSERT_TRUE(std::filesystem::exists(goldenPath));
  auto golden = ImageIO::LoadRgbaPng(goldenPath);
  ASSERT_TRUE(golden.hasValue()) << golden.error().message;

  ExpectNearGolden(canvas.pixmap(), golden.value(), 40000u, 255, 8);
}

}  // namespace donner::backends::tiny_skia_cpp
