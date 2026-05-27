/// @file

#include "donner/editor/repro/GlRnrReplay.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "donner/base/Vector2.h"
#include "donner/editor/EditorApp.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/tests/RendererImageTestUtils.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

struct PixelCrop {
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
};

std::filesystem::path DiagnosticOutputDir() {
  if (const char* dir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR"); dir != nullptr) {
    return std::filesystem::path(dir);
  }
  return std::filesystem::temp_directory_path();
}

std::filesystem::path RunfilePath(std::string_view path) {
  const std::filesystem::path direct(path);
  if (std::filesystem::exists(direct)) {
    return direct;
  }

  const char* testSrcDir = std::getenv("TEST_SRCDIR");
  if (testSrcDir == nullptr) {
    return direct;
  }

  if (const char* testWorkspace = std::getenv("TEST_WORKSPACE"); testWorkspace != nullptr) {
    const std::filesystem::path workspacePath =
        std::filesystem::path(testSrcDir) / testWorkspace / direct;
    if (std::filesystem::exists(workspacePath)) {
      return workspacePath;
    }
  }

  const std::filesystem::path bzlmodMainPath = std::filesystem::path(testSrcDir) / "_main" / direct;
  if (std::filesystem::exists(bzlmodMainPath)) {
    return bzlmodMainPath;
  }

  return std::filesystem::path(testSrcDir) / direct;
}

const repro::GlRnrReplayCapture* FindCapture(const repro::GlRnrReplayResult& result,
                                             std::uint64_t frameIndex) {
  for (const repro::GlRnrReplayCapture& capture : result.captures) {
    if (capture.frameIndex == frameIndex) {
      return &capture;
    }
  }
  return nullptr;
}

const repro::GlRnrReplayFrameDiagnostics* FindFrameDiagnostics(
    const repro::GlRnrReplayResult& result, std::uint64_t frameIndex) {
  for (const repro::GlRnrReplayFrameDiagnostics& diagnostics : result.frameDiagnostics) {
    if (diagnostics.frameIndex == frameIndex) {
      return &diagnostics;
    }
  }
  return nullptr;
}
svg::RendererBitmap BitmapFromImage(const svg::Image& image) {
  svg::RendererBitmap bitmap;
  bitmap.dimensions = Vector2i(image.width, image.height);
  bitmap.rowBytes = image.strideInPixels * 4u;
  bitmap.alphaType = svg::AlphaType::Premultiplied;
  bitmap.pixels = image.data;
  return bitmap;
}

std::optional<svg::RendererBitmap> LoadCaptureBitmap(const repro::GlRnrReplayResult& result,
                                                     std::uint64_t frameIndex) {
  const repro::GlRnrReplayCapture* capture = FindCapture(result, frameIndex);
  if (capture == nullptr) {
    ADD_FAILURE() << "GL replay did not capture frame " << frameIndex;
    return std::nullopt;
  }

  std::optional<svg::Image> image =
      svg::RendererImageTestUtils::readRgbaImageFromPngFile(capture->path.string().c_str());
  if (!image.has_value()) {
    return std::nullopt;
  }
  return BitmapFromImage(*image);
}

svg::RendererBitmap NormalizeBitmap(svg::RendererBitmap bitmap) {
  const std::size_t tightRowBytes = static_cast<std::size_t>(bitmap.dimensions.x) * 4u;
  if (bitmap.rowBytes == tightRowBytes) {
    return bitmap;
  }

  svg::RendererBitmap normalized;
  normalized.dimensions = bitmap.dimensions;
  normalized.rowBytes = tightRowBytes;
  normalized.alphaType = bitmap.alphaType;
  normalized.pixels.resize(tightRowBytes * static_cast<std::size_t>(bitmap.dimensions.y));
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    std::memcpy(normalized.pixels.data() + static_cast<std::size_t>(y) * tightRowBytes,
                bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes,
                tightRowBytes);
  }
  return normalized;
}

std::optional<svg::RendererBitmap> RenderGroundTruth(std::string_view svgSource,
                                                     const Vector2i& canvasSize) {
  EditorApp referenceApp;
  if (!referenceApp.loadFromString(svgSource)) {
    ADD_FAILURE() << "Failed to load replay test SVG";
    return std::nullopt;
  }

  referenceApp.document().document().setCanvasSize(canvasSize.x, canvasSize.y);
  svg::Renderer renderer;
  renderer.draw(referenceApp.document().document());
  return NormalizeBitmap(renderer.takeSnapshot());
}
constexpr std::string_view kStaticContentOnlySvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"120\" "
    "viewBox=\"0 0 200 120\"><rect width=\"200\" height=\"120\" "
    "fill=\"#102030\"/></svg>";

std::optional<std::filesystem::path> WriteStaticContentReplay(
    const std::filesystem::path& outputDir, std::string_view name, std::uint64_t lastFrame) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_static_content_input.svg";
  file.metadata.svgBasename = "static_content_input.svg";
  file.metadata.svgContentHash = "fnv1a64:test";
  file.metadata.svgSource = std::string(kStaticContentOnlySvg);
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  for (std::uint64_t frameIndex = 0; frameIndex <= lastFrame; ++frameIndex) {
    repro::ReproFrame frame;
    frame.index = frameIndex;
    frame.timestampSeconds = static_cast<double>(frameIndex) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = 320.0;
    frame.mouseY = 240.0;
    file.frames.push_back(frame);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

std::string CanonicalReplayDiagnostics(const repro::GlRnrReplayResult& result) {
  std::ostringstream out;
  const auto writeVec = [&out](const Vector2i& value) { out << value.x << ',' << value.y; };
  const auto writeVecD = [&out](const Vector2d& value) { out << value.x << ',' << value.y; };

  for (const repro::GlRnrReplayFrameDiagnostics& frame : result.frameDiagnostics) {
    out << "frame=" << frame.frameIndex << ";fresh=" << static_cast<int>(frame.canvasFreshness)
        << ";status=" << frame.statusSuffix << ";viewport=";
    writeVec(frame.viewportDesiredCanvas);
    out << ";document=";
    writeVec(frame.documentCanvas);
    out << ";compositor=";
    writeVec(frame.compositorCanvas);
    out << ";metadata_miss=" << frame.metadataOnlyMissCount
        << ";duplicate_textures=" << frame.duplicateLiveTextureCount << ";overlay_dims=";
    writeVec(frame.overlayDimsPx);
    out << ";scheduling=" << static_cast<int>(frame.replayWorkerScheduling)
        << ";hold=" << frame.replayHoldFramesBehind
        << ";withheld=" << frame.replayResultHoldPollsThisFrame << ";tiles=" << frame.tiles.size();
    for (const repro::GlRnrReplayTileDiagnostics& tile : frame.tiles) {
      out << "|" << tile.id << ",kind=" << static_cast<int>(tile.kind)
          << ",generation=" << tile.generation << ",bitmap_px=";
      writeVec(tile.bitmapDimsPx);
      out << ",raster=";
      writeVec(tile.rasterCanvasSize);
      out << ",offset=";
      writeVecD(tile.canvasOffsetDoc);
      out << ",dims=";
      writeVecD(tile.bitmapDimsDoc);
      out << ",drag=";
      writeVecD(tile.dragTranslationDoc);
      out << ",presented_drag=";
      writeVecD(tile.presentedDragTranslationDoc);
      out << ",metadata=" << tile.metadataOnly << ",drag_target=" << tile.isDragTarget;
    }
    out << '\n';
  }
  return out.str();
}

svg::RendererBitmap CropBitmap(const svg::RendererBitmap& bitmap, const PixelCrop& crop) {
  const int x = std::clamp(crop.x, 0, bitmap.dimensions.x);
  const int y = std::clamp(crop.y, 0, bitmap.dimensions.y);
  const int width = std::clamp(crop.width, 0, bitmap.dimensions.x - x);
  const int height = std::clamp(crop.height, 0, bitmap.dimensions.y - y);

  svg::RendererBitmap cropped;
  cropped.dimensions = Vector2i(width, height);
  cropped.rowBytes = static_cast<std::size_t>(width) * 4u;
  cropped.alphaType = bitmap.alphaType;
  cropped.pixels.resize(cropped.rowBytes * static_cast<std::size_t>(height));

  for (int row = 0; row < height; ++row) {
    const std::uint8_t* source = bitmap.pixels.data() +
                                 static_cast<std::size_t>(y + row) * bitmap.rowBytes +
                                 static_cast<std::size_t>(x) * 4u;
    std::uint8_t* target = cropped.pixels.data() + static_cast<std::size_t>(row) * cropped.rowBytes;
    std::memcpy(target, source, cropped.rowBytes);
  }

  return cropped;
}

std::optional<double> YellowCentroidY(const svg::RendererBitmap& bitmap) {
  double totalY = 0.0;
  int count = 0;

  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const std::uint8_t red = bitmap.pixels[offset];
      const std::uint8_t green = bitmap.pixels[offset + 1];
      const std::uint8_t blue = bitmap.pixels[offset + 2];
      const std::uint8_t alpha = bitmap.pixels[offset + 3];
      if (alpha > 200 && red > 180 && green > 130 && blue < 90 && red - green < 110) {
        totalY += static_cast<double>(y);
        ++count;
      }
    }
  }

  if (count == 0) {
    return std::nullopt;
  }
  return totalY / static_cast<double>(count);
}

int CountBrightGreenPixels(const svg::RendererBitmap& bitmap) {
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const std::uint8_t red = bitmap.pixels[offset];
      const std::uint8_t green = bitmap.pixels[offset + 1];
      const std::uint8_t blue = bitmap.pixels[offset + 2];
      const std::uint8_t alpha = bitmap.pixels[offset + 3];
      if (alpha > 200 && red < 40 && green > 200 && blue < 40) {
        ++count;
      }
    }
  }
  return count;
}

TEST(GlRnrReplayTest, ContentOnlyDocumentCanvasCaptureMatchesRendererGroundTruth) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "content_only_ground_truth";
  const std::optional<std::filesystem::path> replayPath =
      WriteStaticContentReplay(outputDir, "content_only_ground_truth.rnr", 1);
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(1);
  options.maxFrame = 1;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.contentOnlyCapture = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  std::optional<svg::RendererBitmap> actual = LoadCaptureBitmap(result, 1);
  ASSERT_TRUE(actual.has_value());
  std::optional<svg::RendererBitmap> fullExpected =
      RenderGroundTruth(kStaticContentOnlySvg, Vector2i(200, 120));
  ASSERT_TRUE(fullExpected.has_value());
  const svg::RendererBitmap expected = CropBitmap(
      *fullExpected,
      PixelCrop{.x = 0, .y = 0, .width = actual->dimensions.x, .height = actual->dimensions.y});
  tests::CompareBitmapToBitmap(NormalizeBitmap(*actual), expected,
                               "gl_content_only_capture_vs_renderer",
                               tests::PixelmatchIdentityParams());

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, DrainEachFrameContentCaptureIsDeterministicAcrossPaceAndDelay) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "deterministic_replay_matrix";
  const std::optional<std::filesystem::path> replayPath =
      WriteStaticContentReplay(outputDir, "deterministic_replay_matrix.rnr", 1);
  ASSERT_TRUE(replayPath.has_value());

  std::optional<svg::RendererBitmap> baselineCapture;
  std::optional<std::string> baselineDiagnostics;
  constexpr int kDelayMatrixMs[] = {0, 5, 10, 20, 50};
  for (const bool pace : {false, true}) {
    for (const int delayMs : kDelayMatrixMs) {
      repro::GlRnrReplayOptions options;
      options.rnrPath = *replayPath;
      options.outputDir = outputDir / (std::string(pace ? "paced" : "unpaced") + "_delay_" +
                                       std::to_string(delayMs));
      options.captureFrames.insert(1);
      options.maxFrame = 1;
      options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
      options.pace = pace;
      options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
      options.workerRenderDelayMsForTesting = delayMs;
      options.contentOnlyCapture = true;

      repro::GlRnrReplayResult result;
      std::string error;
      ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

      std::optional<svg::RendererBitmap> capture = LoadCaptureBitmap(result, 1);
      ASSERT_TRUE(capture.has_value());
      const svg::RendererBitmap normalizedCapture = NormalizeBitmap(*capture);
      const std::string diagnostics = CanonicalReplayDiagnostics(result);
      const std::string label = std::string("gl_replay_matrix_") + (pace ? "paced" : "unpaced") +
                                "_delay_" + std::to_string(delayMs);

      if (!baselineCapture.has_value()) {
        baselineCapture = normalizedCapture;
        baselineDiagnostics = diagnostics;
        continue;
      }

      tests::CompareBitmapToBitmap(normalizedCapture, *baselineCapture, label,
                                   tests::PixelmatchIdentityParams());
      EXPECT_EQ(diagnostics, *baselineDiagnostics) << label;
    }
  }

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, HoldFramesBehindRecordsWithheldReplayDiagnostics) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "hold_frames_behind";
  const std::optional<std::filesystem::path> replayPath =
      WriteStaticContentReplay(outputDir, "hold_frames_behind.rnr", 2);
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(2);
  options.maxFrame = 2;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::HoldFramesBehind;
  options.holdFramesBehind = 1;
  options.workerRenderDelayMsForTesting = 1;
  options.contentOnlyCapture = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  const repro::GlRnrReplayFrameDiagnostics* withheld = FindFrameDiagnostics(result, 1);
  ASSERT_NE(withheld, nullptr);
  EXPECT_EQ(withheld->replayWorkerScheduling, repro::GlRnrReplayWorkerScheduling::HoldFramesBehind);
  EXPECT_EQ(withheld->replayWorkerRenderDelayMsForTesting, 1);
  EXPECT_EQ(withheld->replayHoldFramesBehind, 1);
  EXPECT_EQ(withheld->replayResultHoldPollsThisFrame, 1u);
  EXPECT_TRUE(withheld->replayResultWithheld);

  const repro::GlRnrReplayFrameDiagnostics* released = FindFrameDiagnostics(result, 2);
  ASSERT_NE(released, nullptr);
  EXPECT_EQ(released->replayResultHoldPollsThisFrame, 0u);
  EXPECT_FALSE(released->replayResultWithheld);
  EXPECT_NE(FindCapture(result, 2), nullptr);

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, UsesEmbeddedSvgSourceWhenOriginalPathIsMissing) {
  const std::filesystem::path outputDir = DiagnosticOutputDir();
  const std::filesystem::path reproPath = outputDir / "embedded_svg_source_replay.rnr";

  repro::ReproFile file;
  file.metadata.svgPath = "missing_original_input.svg";
  file.metadata.svgBasename = "embedded_input.svg";
  file.metadata.svgContentHash = "fnv1a64:test";
  file.metadata.svgSource =
      R"(<svg viewBox="0 0 10 10"><rect width="10" height="10" fill="#ffcc00"/></svg>)";
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  repro::ReproFrame frame;
  frame.index = 0;
  frame.deltaMs = 16.667;
  frame.mouseX = 20.0;
  frame.mouseY = 20.0;
  file.frames.push_back(frame);

  ASSERT_TRUE(repro::WriteReproFile(reproPath, file));

  repro::GlRnrReplayOptions options;
  options.rnrPath = reproPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(0);
  options.pace = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;
  ASSERT_NE(FindCapture(result, 0), nullptr);

  std::error_code ec;
  std::filesystem::remove(reproPath, ec);
}

// Regression coverage for #601: deterministic worker draining keeps the async renderer out
// of ConcurrentDom while replayed source-pane input reparses the document.
TEST(GlRnrReplayTest, ReplaysSourcePaneCharacterInput) {
  const std::filesystem::path outputDir = DiagnosticOutputDir();
  const std::filesystem::path reproPath = outputDir / "source_pane_character_input_replay.rnr";
  // The document renders at actual size in the replay (no zoom is applied), so a
  // tiny viewBox would paint fewer pixels than the bright-green threshold below.
  // Use a 200x200 document so the recolored rect dominates the render pane crop.
  constexpr std::string_view kInitialSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"><rect width="200" height="200" fill="#ff0000"/></svg>)";
  constexpr std::string_view kEditedSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 200 200"><rect width="200" height="200" fill="#00ff00"/></svg>)";

  repro::ReproFile file;
  file.metadata.svgPath = "missing_text_edit_input.svg";
  file.metadata.svgBasename = "text_edit_input.svg";
  file.metadata.svgContentHash = "fnv1a64:test";
  file.metadata.svgSource = std::string(kInitialSource);
  file.metadata.windowWidth = 1400;
  file.metadata.windowHeight = 600;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, double mouseX, double mouseY, int buttons,
                             int modifiers, std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) * 0.01;
    frame.deltaMs = 10.0;
    frame.mouseX = mouseX;
    frame.mouseY = mouseY;
    frame.mouseButtonMask = buttons;
    frame.modifiers = modifiers;
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  pushFrame(0, 30.0, 70.0, 0, 0);
  repro::ReproEvent mouseDown;
  mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  mouseDown.mouseButton = 0;
  pushFrame(1, 30.0, 70.0, 1, 0, {mouseDown});
  repro::ReproEvent mouseUp;
  mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  mouseUp.mouseButton = 0;
  pushFrame(2, 30.0, 70.0, 0, 0, {mouseUp});
  repro::ReproEvent selectAllDown;
  selectAllDown.kind = repro::ReproEvent::Kind::KeyDown;
  selectAllDown.key = static_cast<int>(ImGuiKey_A);
  selectAllDown.modifiers = 1 << 0;
  pushFrame(3, 30.0, 70.0, 0, 1 << 0, {selectAllDown});
  repro::ReproEvent selectAllUp;
  selectAllUp.kind = repro::ReproEvent::Kind::KeyUp;
  selectAllUp.key = static_cast<int>(ImGuiKey_A);
  pushFrame(4, 30.0, 70.0, 0, 0, {selectAllUp});

  std::vector<repro::ReproEvent> characterEvents;
  for (const unsigned char c : kEditedSource) {
    repro::ReproEvent event;
    event.kind = repro::ReproEvent::Kind::Char;
    event.codepoint = c;
    characterEvents.push_back(event);
  }
  pushFrame(5, 30.0, 70.0, 0, 0, std::move(characterEvents));
  for (std::uint64_t index = 6; index <= 60; ++index) {
    pushFrame(index, 30.0, 70.0, 0, 0);
  }

  ASSERT_TRUE(repro::WriteReproFile(reproPath, file));

  repro::GlRnrReplayOptions options;
  options.rnrPath = reproPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(60);
  options.cropMode = repro::GlRnrReplayCropMode::Full;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.maxFrame = 60;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  std::optional<svg::RendererBitmap> bitmap = LoadCaptureBitmap(result, 60);
  ASSERT_TRUE(bitmap.has_value());
  const svg::RendererBitmap renderPaneCrop =
      CropBitmap(*bitmap, PixelCrop{.x = 560, .y = 0, .width = 500, .height = 600});
  EXPECT_GT(CountBrightGreenPixels(renderPaneCrop), 100);

  std::error_code ec;
  std::filesystem::remove(reproPath, ec);
}

// Regression coverage for #601: deterministic draining fixes the worker landing frame, and
// content-only capture removes intentional selection-chrome settle from the pixel assertion.
TEST(GlRnrReplayTest, SecondDragActiveFrameMatchesMouseUpFrame) {
  constexpr std::string_view kRnrPath = "donner/editor/tests/filter_post_drag_jump.rnr";
  const std::filesystem::path rnrPath = RunfilePath(kRnrPath);
  std::optional<repro::ReproFile> reproFile = repro::ReadReproFile(rnrPath);
  ASSERT_TRUE(reproFile.has_value());
  ASSERT_TRUE(reproFile->metadata.expect.has_value());
  const repro::ReproExpectation& expect = *reproFile->metadata.expect;
  ASSERT_EQ(expect.proofKind, repro::ReproExpectationProofKind::ActiveDragAlignment);
  ASSERT_TRUE(expect.activeFrameIndex.has_value());
  ASSERT_TRUE(expect.comparisonFrameIndex.has_value());
  ASSERT_EQ(expect.cropMode, "document-canvas");

  const std::uint64_t activeFrame = static_cast<std::uint64_t>(*expect.activeFrameIndex);
  const std::uint64_t comparisonFrame = static_cast<std::uint64_t>(*expect.comparisonFrameIndex);

  repro::GlRnrReplayOptions options;
  options.rnrPath = rnrPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = DiagnosticOutputDir() / "gl_second_drag_alignment_repro";
  options.captureFrames = {activeFrame, comparisonFrame};
  options.maxFrame = comparisonFrame;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.workerRenderDelayMsForTesting = 2;
  options.contentOnlyCapture = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  std::optional<svg::RendererBitmap> active = LoadCaptureBitmap(result, activeFrame);
  std::optional<svg::RendererBitmap> comparison = LoadCaptureBitmap(result, comparisonFrame);
  ASSERT_TRUE(active.has_value());
  ASSERT_TRUE(comparison.has_value());

  tests::CompareBitmapToBitmap(*active, *comparison,
                               "gl_second_drag_frame_249_active_vs_250_mouseup",
                               tests::PixelmatchIdentityParams());
}

// Regression coverage for #601: deterministic worker draining prevents ConcurrentDom
// UI-thread reads while this replay covers the texture-reuse diagnostic window.
TEST(GlRnrReplayTest, GeodeDragZoomOReplayCoversTextureReuseWindow) {
  constexpr std::string_view kRnrPath = "donner/editor/tests/geode_drag_zoom_o_pop.rnr";
  constexpr std::uint64_t kFirstCaptureFrame = 78;
  constexpr std::uint64_t kLastCaptureFrame = 81;

  const std::filesystem::path rnrPath = RunfilePath(kRnrPath);
  std::optional<repro::ReproFile> reproFile = repro::ReadReproFile(rnrPath);
  ASSERT_TRUE(reproFile.has_value());
  ASSERT_TRUE(reproFile->metadata.expect.has_value());
  const repro::ReproExpectation& expect = *reproFile->metadata.expect;
  ASSERT_EQ(expect.proofKind, repro::ReproExpectationProofKind::PresentedPixels);
  ASSERT_EQ(expect.cropMode, "document-canvas");
  ASSERT_EQ(expect.targetSelector, "#Donner path:nth-of-type(2)");

  repro::GlRnrReplayOptions options;
  options.rnrPath = rnrPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = DiagnosticOutputDir() / "gl_geode_drag_zoom_o_pop";
  options.captureFrames = {kFirstCaptureFrame, 79, 80, kLastCaptureFrame};
  options.maxFrame = kLastCaptureFrame;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  for (std::uint64_t frame = kFirstCaptureFrame; frame <= kLastCaptureFrame; ++frame) {
    const repro::GlRnrReplayFrameDiagnostics* diagnostics = FindFrameDiagnostics(result, frame);
    ASSERT_NE(diagnostics, nullptr) << "missing diagnostics for replay frame " << frame;
    EXPECT_EQ(diagnostics->metadataOnlyMissCount, 0)
        << "metadata-only reuse unexpectedly missed in root-cause replay frame " << frame;
    EXPECT_EQ(diagnostics->duplicateLiveTextureCount, 0)
        << "duplicate live texture handles in root-cause replay frame " << frame;

    std::optional<svg::RendererBitmap> capture = LoadCaptureBitmap(result, frame);
    ASSERT_TRUE(capture.has_value()) << "missing capture for replay frame " << frame;
    EXPECT_GT(capture->dimensions.x, 0);
    EXPECT_GT(capture->dimensions.y, 0);
  }
}

// Regression coverage for #601: deterministic worker draining makes the filtered drag replay
// stable while content-only capture keeps the assertion focused on document pixels.
TEST(GlRnrReplayTest, FilteredElementOThenRDragDoesNotPopOBackOnRClick) {
  constexpr std::string_view kRnrPath =
      "donner/editor/tests/filtered-element-flash-after-drags-2.rnr";
  const std::filesystem::path rnrPath = RunfilePath(kRnrPath);
  std::optional<repro::ReproFile> reproFile = repro::ReadReproFile(rnrPath);
  ASSERT_TRUE(reproFile.has_value());
  ASSERT_TRUE(reproFile->metadata.expect.has_value());
  ASSERT_TRUE(reproFile->metadata.expect->cropRect.has_value());
  const repro::ReproExpectation& expect = *reproFile->metadata.expect;
  ASSERT_EQ(expect.cropMode, "document-canvas");
  const std::uint64_t beforeClickFrame = static_cast<std::uint64_t>(expect.minFrameIndex - 1);
  const std::uint64_t firstClickFrame = static_cast<std::uint64_t>(expect.minFrameIndex);
  const std::uint64_t settledClickFrame = static_cast<std::uint64_t>(expect.minFrameIndex + 2);

  repro::GlRnrReplayOptions options;
  options.rnrPath = rnrPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = DiagnosticOutputDir() / "gl_o_then_r_popback_repro";
  options.captureFrames = {beforeClickFrame, firstClickFrame, settledClickFrame};
  options.maxFrame = static_cast<std::uint64_t>(expect.maxFrameIndex);
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.contentOnlyCapture = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  std::optional<svg::RendererBitmap> beforeRClick = LoadCaptureBitmap(result, beforeClickFrame);
  std::optional<svg::RendererBitmap> firstRClickFrame = LoadCaptureBitmap(result, firstClickFrame);
  std::optional<svg::RendererBitmap> settledRClickFrame =
      LoadCaptureBitmap(result, settledClickFrame);
  ASSERT_TRUE(beforeRClick.has_value());
  ASSERT_TRUE(firstRClickFrame.has_value());
  ASSERT_TRUE(settledRClickFrame.has_value());

  const PixelCrop oCrop{
      .x = expect.cropRect->x,
      .y = expect.cropRect->y,
      .width = expect.cropRect->width,
      .height = expect.cropRect->height,
  };
  const svg::RendererBitmap firstRClickO = CropBitmap(*firstRClickFrame, oCrop);
  const svg::RendererBitmap settledRClickO = CropBitmap(*settledRClickFrame, oCrop);

  tests::CompareBitmapToBitmap(firstRClickO, settledRClickO,
                               "gl_o_then_r_frame_153_o_crop_vs_frame_155",
                               tests::PixelmatchIdentityParams());

  const std::optional<double> beforeCentroidY = YellowCentroidY(CropBitmap(*beforeRClick, oCrop));
  const std::optional<double> firstCentroidY = YellowCentroidY(firstRClickO);
  const std::optional<double> settledCentroidY = YellowCentroidY(settledRClickO);
  ASSERT_TRUE(beforeCentroidY.has_value());
  ASSERT_TRUE(firstCentroidY.has_value());
  ASSERT_TRUE(settledCentroidY.has_value());
  EXPECT_NEAR(*beforeCentroidY, *settledCentroidY, 1.0)
      << "The two stable frames should agree on the post-O-drag position.";
  EXPECT_NEAR(*firstCentroidY, *settledCentroidY, 1.0)
      << "The first R-click frame should keep O at its post-drag position instead of popping "
         "back for one presented frame.";
}

}  // namespace
}  // namespace donner::editor
