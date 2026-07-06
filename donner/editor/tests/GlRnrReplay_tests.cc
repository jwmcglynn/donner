/// @file

#include "donner/editor/repro/GlRnrReplay.h"

#include <algorithm>
#include <array>
#include <cmath>
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
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

// Runs a GL replay and, on failure, either GTEST_SKIP()s or FAIL()s. The skip
// path fires only when the host genuinely cannot provide a GL context (a
// headless / GPU-less environment with no software-GL fallback, e.g.
// GitHub-hosted macOS, whose NSGL path reports "Failed to find a suitable pixel
// format"). Every other failure is a real error and still FAILs. Linux CI runs
// these same tests on Mesa llvmpipe, so a genuine GL-path regression is caught
// there - this only skips where GL is truly unavailable.
#define ASSERT_GL_REPLAY_OR_SKIP(optionsExpr, resultVar, errorVar)                                 \
  do {                                                                                             \
    if (!repro::RunGlRnrReplay((optionsExpr), &(resultVar), &(errorVar))) {                        \
      if ((resultVar).glUnavailable) {                                                             \
        GTEST_SKIP() << "GL context unavailable on this host; skipping GL replay: " << (errorVar); \
      }                                                                                            \
      FAIL() << (errorVar);                                                                        \
    }                                                                                              \
  } while (false)

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

TEST(GlRnrReplayTest, CropModeParsingAcceptsAliasesAndRejectsUnknownValues) {
  EXPECT_EQ(repro::ParseGlRnrReplayCropMode("full"), repro::GlRnrReplayCropMode::Full);
  EXPECT_EQ(repro::ParseGlRnrReplayCropMode("render-pane"), repro::GlRnrReplayCropMode::RenderPane);
  EXPECT_EQ(repro::ParseGlRnrReplayCropMode("document-canvas"),
            repro::GlRnrReplayCropMode::DocumentCanvas);
  EXPECT_EQ(repro::ParseGlRnrReplayCropMode("canvas"), repro::GlRnrReplayCropMode::DocumentCanvas);
  EXPECT_EQ(repro::ParseGlRnrReplayCropMode("unknown"), std::nullopt);

  EXPECT_EQ(repro::GlRnrReplayCropModeSuffix(repro::GlRnrReplayCropMode::Full), "");
  EXPECT_EQ(repro::GlRnrReplayCropModeSuffix(repro::GlRnrReplayCropMode::RenderPane),
            "render_pane");
  EXPECT_EQ(repro::GlRnrReplayCropModeSuffix(repro::GlRnrReplayCropMode::DocumentCanvas), "canvas");
}

TEST(GlRnrReplayTest, RunGlRnrReplayValidatesOptionsBeforeOpeningWindow) {
  repro::GlRnrReplayOptions options;
  repro::GlRnrReplayResult result;
  std::string error;

  EXPECT_FALSE(repro::RunGlRnrReplay(options, nullptr, nullptr));

  EXPECT_FALSE(repro::RunGlRnrReplay(options, &result, &error));
  EXPECT_EQ(error, "rnrPath is required");

  options.rnrPath = DiagnosticOutputDir() / "missing.rnr";
  error.clear();
  EXPECT_FALSE(repro::RunGlRnrReplay(options, &result, &error));
  EXPECT_EQ(error, "at least one GL capture selector is required");

  options.captureFrames = {1};
  options.holdFramesBehind = -1;
  error.clear();
  EXPECT_FALSE(repro::RunGlRnrReplay(options, &result, &error));
  EXPECT_EQ(error, "holdFramesBehind must be non-negative");

  options.holdFramesBehind = 0;
  options.workerRenderDelayMsForTesting = -1;
  error.clear();
  EXPECT_FALSE(repro::RunGlRnrReplay(options, &result, &error));
  EXPECT_EQ(error, "workerRenderDelayMsForTesting must be non-negative");

  options.workerRenderDelayMsForTesting = 0;
  options.holdFramesBehind = 1;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::Realtime;
  error.clear();
  EXPECT_FALSE(repro::RunGlRnrReplay(options, &result, &error));
  EXPECT_EQ(error, "holdFramesBehind requires HoldFramesBehind worker scheduling");

  options.holdFramesBehind = 0;
  error.clear();
  EXPECT_FALSE(repro::RunGlRnrReplay(options, &result, &error));
  EXPECT_NE(error.find("failed to read .rnr file"), std::string::npos);
}

constexpr std::string_view kStaticContentOnlySvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"200\" height=\"120\" "
    "viewBox=\"0 0 200 120\"><rect width=\"200\" height=\"120\" "
    "fill=\"#102030\"/></svg>";
constexpr std::string_view kDocumentSpaceDragSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"120\" height=\"80\" "
    "viewBox=\"0 0 120 80\"><rect id=\"target\" x=\"10\" y=\"10\" width=\"20\" "
    "height=\"20\" fill=\"#00ff00\"/></svg>";
constexpr std::string_view kBlankPenCanvasSvg =
    "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"80\" height=\"80\" "
    "viewBox=\"0 0 80 80\"></svg>";

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

std::optional<std::filesystem::path> WriteDocumentSpaceDragReplay(
    const std::filesystem::path& outputDir, std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_document_space_drag.svg";
  file.metadata.svgBasename = "document_space_drag.svg";
  file.metadata.svgContentHash = "fnv1a64:document-space-drag";
  file.metadata.svgSource = std::string(kDocumentSpaceDragSvg);
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto addFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                            std::vector<repro::ReproEvent> events) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  repro::ReproEvent mouseDown;
  mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  mouseDown.mouseButton = 0;
  mouseDown.hit = repro::ReproHit{.id = "target", .tag = "rect"};
  addFrame(0, Vector2d(15.0, 15.0), /*mouseButtonMask=*/1, {mouseDown});
  addFrame(1, Vector2d(35.0, 25.0), /*mouseButtonMask=*/1, {});
  repro::ReproEvent mouseUp;
  mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  mouseUp.mouseButton = 0;
  addFrame(2, Vector2d(35.0, 25.0), /*mouseButtonMask=*/0, {mouseUp});

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

std::optional<std::filesystem::path> WriteDirectPresentationDragReplay(
    const std::filesystem::path& outputDir, std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_direct_presentation_drag.svg";
  file.metadata.svgBasename = "direct_presentation_drag.svg";
  file.metadata.svgContentHash = "fnv1a64:direct-presentation-drag";
  file.metadata.svgSource = std::string(kDocumentSpaceDragSvg);
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto addFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                            std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  const Vector2d kStartDoc(15.0, 15.0);
  for (std::uint64_t frame = 0; frame <= 8; ++frame) {
    addFrame(frame, kStartDoc, 0);
  }

  repro::ReproEvent mouseDown;
  mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  mouseDown.mouseButton = 0;
  mouseDown.hit = repro::ReproHit{.id = "target", .tag = "rect"};
  addFrame(9, kStartDoc, 1, {mouseDown});

  for (std::uint64_t frame = 10; frame <= 20; ++frame) {
    const double step = static_cast<double>(frame - 9);
    addFrame(frame, kStartDoc + Vector2d(step * 4.0, step * 2.0), 1);
  }

  repro::ReproEvent mouseUp;
  mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  mouseUp.mouseButton = 0;
  addFrame(21, kStartDoc + Vector2d(48.0, 24.0), 0, {mouseUp});

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

std::optional<std::filesystem::path> WriteSemanticPenPaintReplay(
    const std::filesystem::path& outputDir, std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_semantic_pen_paint.svg";
  file.metadata.svgBasename = "semantic_pen_paint.svg";
  file.metadata.svgContentHash = "fnv1a64:semantic-pen-paint";
  file.metadata.svgSource = std::string(kBlankPenCanvasSvg);
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                             std::vector<repro::ReproAction> actions,
                             std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.actions = std::move(actions);
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  pushFrame(0, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetActiveTool,
                .tool = "pen",
            }});
  pushFrame(1, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetStyleProperty,
                .propertyName = "fill",
                .propertyValue = "#ff0000",
            }});
  pushFrame(2, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetStyleProperty,
                .propertyName = "stroke",
                .propertyValue = "black",
            }});

  const auto pushClick = [&](std::uint64_t downFrame, Vector2d mouseDoc) {
    repro::ReproEvent mouseDown;
    mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
    mouseDown.mouseButton = 0;
    pushFrame(downFrame, mouseDoc, 1, {}, {mouseDown});

    repro::ReproEvent mouseUp;
    mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
    mouseUp.mouseButton = 0;
    pushFrame(downFrame + 1, mouseDoc, 0, {}, {mouseUp});
  };

  pushClick(3, Vector2d(10.0, 10.0));
  pushClick(5, Vector2d(70.0, 10.0));
  pushClick(7, Vector2d(40.0, 70.0));
  pushClick(9, Vector2d(10.0, 10.0));
  for (std::uint64_t index = 11; index <= 20; ++index) {
    pushFrame(index, Vector2d::Zero(), 0, {});
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

std::optional<std::filesystem::path> WritePenDragReplay(const std::filesystem::path& outputDir,
                                                        std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_pen_drag.svg";
  file.metadata.svgBasename = "pen_drag.svg";
  file.metadata.svgContentHash = "fnv1a64:pen-drag";
  file.metadata.svgSource = std::string(kBlankPenCanvasSvg);
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                             std::vector<repro::ReproAction> actions = {},
                             std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.actions = std::move(actions);
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  pushFrame(0, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetActiveTool,
                .tool = "pen",
            }});
  pushFrame(1, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetStyleProperty,
                .propertyName = "fill",
                .propertyValue = "none",
            }});
  pushFrame(2, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetStyleProperty,
                .propertyName = "stroke",
                .propertyValue = "#ff0000",
            }});

  repro::ReproEvent firstMouseDown;
  firstMouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  firstMouseDown.mouseButton = 0;
  pushFrame(3, Vector2d(10.0, 10.0), 1, {}, {firstMouseDown});
  repro::ReproEvent firstMouseUp;
  firstMouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  firstMouseUp.mouseButton = 0;
  pushFrame(4, Vector2d(10.0, 10.0), 0, {}, {firstMouseUp});
  pushFrame(5, Vector2d(10.0, 10.0), 0);

  repro::ReproEvent secondMouseDown;
  secondMouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  secondMouseDown.mouseButton = 0;
  pushFrame(6, Vector2d(40.0, 10.0), 1, {}, {secondMouseDown});
  pushFrame(7, Vector2d(40.0, 30.0), 1);
  pushFrame(8, Vector2d(40.0, 30.0), 1);
  pushFrame(9, Vector2d(40.0, 30.0), 1);

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

// Two pen anchors placed as plain clicks, then a single key press (frame 5
// down / frame 6 up), then idle frames through 10. Shared by the Escape and
// Backspace contract tests.
std::optional<std::filesystem::path> WritePenTwoAnchorsThenKeyReplay(
    const std::filesystem::path& outputDir, std::string_view name, int imguiKey) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_pen_key.svg";
  file.metadata.svgBasename = "pen_key.svg";
  file.metadata.svgContentHash = "fnv1a64:pen-key";
  file.metadata.svgSource = std::string(kBlankPenCanvasSvg);
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                             std::vector<repro::ReproAction> actions = {},
                             std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.actions = std::move(actions);
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  pushFrame(0, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetActiveTool,
                .tool = "pen",
            }});

  const auto pushClick = [&](std::uint64_t downFrame, Vector2d mouseDoc) {
    repro::ReproEvent mouseDown;
    mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
    mouseDown.mouseButton = 0;
    pushFrame(downFrame, mouseDoc, 1, {}, {mouseDown});

    repro::ReproEvent mouseUp;
    mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
    mouseUp.mouseButton = 0;
    pushFrame(downFrame + 1, mouseDoc, 0, {}, {mouseUp});
  };

  pushClick(1, Vector2d(10.0, 10.0));
  pushClick(3, Vector2d(40.0, 10.0));

  repro::ReproEvent keyDown;
  keyDown.kind = repro::ReproEvent::Kind::KeyDown;
  keyDown.key = imguiKey;
  pushFrame(5, Vector2d(40.0, 10.0), 0, {}, {keyDown});
  repro::ReproEvent keyUp;
  keyUp.kind = repro::ReproEvent::Kind::KeyUp;
  keyUp.key = imguiKey;
  pushFrame(6, Vector2d(40.0, 10.0), 0, {}, {keyUp});
  for (std::uint64_t index = 7; index <= 10; ++index) {
    pushFrame(index, Vector2d(40.0, 10.0), 0);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

// Text-tool session driven end-to-end through the shell event loop: switch
// to the text tool, click to open a point-text session, type "Hi" via Char
// events, then press Escape to commit the session.
std::optional<std::filesystem::path> WriteTextTypingReplay(const std::filesystem::path& outputDir,
                                                           std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_text_typing.svg";
  file.metadata.svgBasename = "text_typing.svg";
  file.metadata.svgContentHash = "fnv1a64:text-typing";
  file.metadata.svgSource = std::string(kBlankPenCanvasSvg);
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                             std::vector<repro::ReproAction> actions = {},
                             std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.actions = std::move(actions);
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  pushFrame(0, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetActiveTool,
                .tool = "text",
            }});

  // Double-click on empty canvas opens the point-text session: two rapid
  // press/release pairs at the same document point.
  const Vector2d clickDoc(10.0, 40.0);
  repro::ReproEvent mouseDown;
  mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  mouseDown.mouseButton = 0;
  repro::ReproEvent mouseUp;
  mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  mouseUp.mouseButton = 0;
  pushFrame(1, clickDoc, 1, {}, {mouseDown});
  pushFrame(2, clickDoc, 0, {}, {mouseUp});
  pushFrame(3, clickDoc, 1, {}, {mouseDown});
  pushFrame(4, clickDoc, 0, {}, {mouseUp});

  std::vector<repro::ReproEvent> typing;
  for (const char c : std::string_view("Hi")) {
    repro::ReproEvent event;
    event.kind = repro::ReproEvent::Kind::Char;
    event.codepoint = static_cast<std::uint32_t>(c);
    typing.push_back(event);
  }
  pushFrame(5, clickDoc, 0, {}, std::move(typing));
  pushFrame(6, clickDoc, 0);

  repro::ReproEvent escapeDown;
  escapeDown.kind = repro::ReproEvent::Kind::KeyDown;
  escapeDown.key = static_cast<int>(ImGuiKey_Escape);
  pushFrame(7, clickDoc, 0, {}, {escapeDown});
  repro::ReproEvent escapeUp;
  escapeUp.kind = repro::ReproEvent::Kind::KeyUp;
  escapeUp.key = static_cast<int>(ImGuiKey_Escape);
  pushFrame(8, clickDoc, 0, {}, {escapeUp});
  for (std::uint64_t index = 9; index <= 12; ++index) {
    pushFrame(index, clickDoc, 0);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

// Typing into an existing text element one character per frame, like real
// keyboard input: switch to the text tool, click into the green text to open
// an editing session on it, then deliver one Char event per frame followed by
// idle settle frames. Document-space input; the async worker scheduling is
// controlled by the test's replay options.
std::optional<std::filesystem::path> WriteTextTypingIntoExistingTextReplay(
    const std::filesystem::path& outputDir, std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_text_typing_existing.svg";
  file.metadata.svgBasename = "text_typing_existing.svg";
  file.metadata.svgContentHash = "fnv1a64:text-typing-existing";
  file.metadata.svgSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">)"
      R"(<text id="msg" x="20" y="60" font-family="sans-serif" font-size="32" )"
      R"(fill="#00cc00">Hello</text></svg>)";
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                             std::vector<repro::ReproAction> actions = {},
                             std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.actions = std::move(actions);
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  pushFrame(0, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetActiveTool,
                .tool = "text",
            }});
  // Let the initial document render land before interacting.
  const Vector2d clickDoc(60.0, 50.0);
  for (std::uint64_t index = 1; index <= 9; ++index) {
    pushFrame(index, clickDoc, 0);
  }

  // Click inside the existing text: opens the session with the caret at the
  // clicked character.
  repro::ReproEvent mouseDown;
  mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  mouseDown.mouseButton = 0;
  mouseDown.hit = repro::ReproHit{.id = "msg", .tag = "text"};
  repro::ReproEvent mouseUp;
  mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  mouseUp.mouseButton = 0;
  pushFrame(10, clickDoc, 1, {}, {mouseDown});
  pushFrame(11, clickDoc, 0, {}, {mouseUp});
  for (std::uint64_t index = 12; index <= 17; ++index) {
    pushFrame(index, clickDoc, 0);
  }

  // One character per frame, like real typing.
  std::uint64_t index = 18;
  for (const char c : std::string_view("altogether")) {
    repro::ReproEvent event;
    event.kind = repro::ReproEvent::Kind::Char;
    event.codepoint = static_cast<std::uint32_t>(c);
    pushFrame(index++, clickDoc, 0, {}, {event});
  }
  for (; index <= 40; ++index) {
    pushFrame(index, clickDoc, 0);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

// Text-tool session driven through the LIVE ImGui pointer path (screen-space
// mouse events, no document-space replay input): switch to the text tool,
// click the canvas, type "Hi" via Char events, then Escape to commit. This is
// the path the real editor uses; `driveDocumentSpaceInput` bypasses it by
// parking the ImGui mouse offscreen.
std::optional<std::filesystem::path> WriteTextTypingLivePointerReplay(
    const std::filesystem::path& outputDir, std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_text_typing_live.svg";
  file.metadata.svgBasename = "text_typing_live.svg";
  file.metadata.svgContentHash = "fnv1a64:text-typing-live";
  file.metadata.svgSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200"></svg>)";
  file.metadata.windowWidth = 1400;
  file.metadata.windowHeight = 600;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, double mouseX, double mouseY, int buttons,
                             std::vector<repro::ReproAction> actions = {},
                             std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseX;
    frame.mouseY = mouseY;
    frame.mouseButtonMask = buttons;
    frame.actions = std::move(actions);
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  // Screen point inside the render pane (window 1400x600; the render pane
  // occupies the region right of the source pane, x >= ~560).
  const double clickX = 800.0;
  const double clickY = 300.0;

  pushFrame(0, clickX, clickY, 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetActiveTool,
                .tool = "text",
            }});
  pushFrame(1, clickX, clickY, 0);

  // Double-click on empty canvas opens the point-text session: two rapid
  // press/release pairs at the same screen point (well within ImGui's
  // double-click time/distance thresholds), holding the second press across
  // a few frames so the pending click is processed while the button is still
  // down, like a real click.
  repro::ReproEvent mouseDown;
  mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  mouseDown.mouseButton = 0;
  repro::ReproEvent mouseUp;
  mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  mouseUp.mouseButton = 0;
  pushFrame(2, clickX, clickY, 1, {}, {mouseDown});
  pushFrame(3, clickX, clickY, 0, {}, {mouseUp});
  pushFrame(4, clickX, clickY, 1, {}, {mouseDown});
  pushFrame(5, clickX, clickY, 1);
  pushFrame(6, clickX, clickY, 1);
  pushFrame(7, clickX, clickY, 0, {}, {mouseUp});
  pushFrame(8, clickX, clickY, 0);

  std::vector<repro::ReproEvent> typing;
  for (const char c : std::string_view("Hi")) {
    repro::ReproEvent event;
    event.kind = repro::ReproEvent::Kind::Char;
    event.codepoint = static_cast<std::uint32_t>(c);
    typing.push_back(event);
  }
  pushFrame(9, clickX, clickY, 0, {}, std::move(typing));
  pushFrame(10, clickX, clickY, 0);

  repro::ReproEvent escapeDown;
  escapeDown.kind = repro::ReproEvent::Kind::KeyDown;
  escapeDown.key = static_cast<int>(ImGuiKey_Escape);
  pushFrame(11, clickX, clickY, 0, {}, {escapeDown});
  repro::ReproEvent escapeUp;
  escapeUp.kind = repro::ReproEvent::Kind::KeyUp;
  escapeUp.key = static_cast<int>(ImGuiKey_Escape);
  pushFrame(12, clickX, clickY, 0, {}, {escapeUp});
  for (std::uint64_t index = 13; index <= 16; ++index) {
    pushFrame(index, clickX, clickY, 0);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

// First resize of a freshly selected <g> - the state convert-text-to-outlines
// leaves the editor in (selection jumps to a group whose own layer raster has
// never been produced). Selects the group via the mouse-down hit checkpoint,
// grabs the bottom-right transform handle at (100,120) in the same press, and
// drags it to (130,150) while the worker lags. The presented content must
// track the resize in lockstep with the overlay the whole way.
std::optional<std::filesystem::path> WriteGroupFirstResizeReplay(
    const std::filesystem::path& outputDir, std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_group_first_resize.svg";
  file.metadata.svgBasename = "group_first_resize.svg";
  file.metadata.svgContentHash = "fnv1a64:group-first-resize";
  file.metadata.svgSource =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200" viewBox="0 0 200 200">
  <rect width="200" height="200" fill="#ffffff"/>
  <g id="g">
    <rect id="r" x="40" y="40" width="60" height="40" fill="#ff0000"/>
    <rect x="60" y="90" width="40" height="30" fill="#0000ff"/>
  </g>
</svg>)svg";
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                             std::vector<repro::ReproAction> actions = {},
                             std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.actions = std::move(actions);
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  pushFrame(0, Vector2d(10.0, 10.0), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetActiveTool,
                .tool = "select",
            }});
  for (std::uint64_t index = 1; index <= 5; ++index) {
    pushFrame(index, Vector2d(10.0, 10.0), 0);
  }

  // Press exactly on the group's bottom-right selection corner. The hit
  // checkpoint selects #g first (the state a conversion leaves behind), so
  // the same press lands on the freshly computed transform handle.
  repro::ReproEvent mouseDown;
  mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  mouseDown.mouseButton = 0;
  mouseDown.hit = repro::ReproHit{.id = "g", .tag = "g"};
  pushFrame(6, Vector2d(100.0, 120.0), 1, {}, {mouseDown});

  // Drag the corner to (130,150) in 5-unit steps, then hold one frame.
  for (int step = 1; step <= 6; ++step) {
    pushFrame(6 + static_cast<std::uint64_t>(step),
              Vector2d(100.0 + 5.0 * step, 120.0 + 5.0 * step), 1);
  }
  pushFrame(13, Vector2d(130.0, 150.0), 1);

  repro::ReproEvent mouseUp;
  mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  mouseUp.mouseButton = 0;
  pushFrame(14, Vector2d(130.0, 150.0), 0, {}, {mouseUp});
  for (std::uint64_t index = 15; index <= 40; ++index) {
    pushFrame(index, Vector2d(130.0, 150.0), 0);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

// Two pen anchors placed as plain clicks, then Escape. Under the committed
// contract Escape ends the session keeping the open path (same as Enter);
// only a segmentless draft is discarded.
std::optional<std::filesystem::path> WritePenEscapeReplay(const std::filesystem::path& outputDir,
                                                          std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_pen_escape.svg";
  file.metadata.svgBasename = "pen_escape.svg";
  file.metadata.svgContentHash = "fnv1a64:pen-escape";
  file.metadata.svgSource = std::string(kBlankPenCanvasSvg);
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                             std::vector<repro::ReproAction> actions = {},
                             std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.actions = std::move(actions);
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  pushFrame(0, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetActiveTool,
                .tool = "pen",
            }});

  const auto pushClick = [&](std::uint64_t downFrame, Vector2d mouseDoc) {
    repro::ReproEvent mouseDown;
    mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
    mouseDown.mouseButton = 0;
    pushFrame(downFrame, mouseDoc, 1, {}, {mouseDown});

    repro::ReproEvent mouseUp;
    mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
    mouseUp.mouseButton = 0;
    pushFrame(downFrame + 1, mouseDoc, 0, {}, {mouseUp});
  };

  pushClick(1, Vector2d(10.0, 10.0));
  pushClick(3, Vector2d(40.0, 10.0));

  repro::ReproEvent escapeDown;
  escapeDown.kind = repro::ReproEvent::Kind::KeyDown;
  escapeDown.key = static_cast<int>(ImGuiKey_Escape);
  pushFrame(5, Vector2d(40.0, 10.0), 0, {}, {escapeDown});
  repro::ReproEvent escapeUp;
  escapeUp.kind = repro::ReproEvent::Kind::KeyUp;
  escapeUp.key = static_cast<int>(ImGuiKey_Escape);
  pushFrame(6, Vector2d(40.0, 10.0), 0, {}, {escapeUp});
  for (std::uint64_t index = 7; index <= 10; ++index) {
    pushFrame(index, Vector2d(40.0, 10.0), 0);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

// One pen anchor placed at (10, 40), then the mouse hovers (no buttons) to
// (70, 40) and rests there. Used to pin the rubber-band preview of the
// segment that a click would commit.
std::optional<std::filesystem::path> WritePenHoverReplay(const std::filesystem::path& outputDir,
                                                         std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_pen_hover.svg";
  file.metadata.svgBasename = "pen_hover.svg";
  file.metadata.svgContentHash = "fnv1a64:pen-hover";
  file.metadata.svgSource = std::string(kBlankPenCanvasSvg);
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                             std::vector<repro::ReproAction> actions = {},
                             std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.actions = std::move(actions);
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  pushFrame(0, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetActiveTool,
                .tool = "pen",
            }});
  repro::ReproEvent mouseDown;
  mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  mouseDown.mouseButton = 0;
  pushFrame(1, Vector2d(10.0, 40.0), 1, {}, {mouseDown});
  repro::ReproEvent mouseUp;
  mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  mouseUp.mouseButton = 0;
  pushFrame(2, Vector2d(10.0, 40.0), 0, {}, {mouseUp});
  pushFrame(3, Vector2d(30.0, 40.0), 0);
  pushFrame(4, Vector2d(50.0, 40.0), 0);
  for (std::uint64_t index = 5; index <= 10; ++index) {
    pushFrame(index, Vector2d(70.0, 40.0), 0);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

// Pen click-drag that shapes a Bezier segment with a large downward bulge:
// first anchor at (10, 40), second anchor mouse-down at (70, 40), then the
// held drag to (70, 0) mirrors the in-handle to (70, 80) so the fill bulges
// down to y≈58 - a region no earlier geometry version touches.
std::optional<std::filesystem::path> WritePenCurveDragReplay(const std::filesystem::path& outputDir,
                                                             std::string_view name,
                                                             std::uint64_t trailingHoldFrames) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_pen_curve_drag.svg";
  file.metadata.svgBasename = "pen_curve_drag.svg";
  file.metadata.svgContentHash = "fnv1a64:pen-curve-drag";
  file.metadata.svgSource = std::string(kBlankPenCanvasSvg);
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                             std::vector<repro::ReproAction> actions = {},
                             std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.actions = std::move(actions);
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  pushFrame(0, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetActiveTool,
                .tool = "pen",
            }});
  pushFrame(1, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetStyleProperty,
                .propertyName = "fill",
                .propertyValue = "#ff0000",
            }});
  pushFrame(2, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetStyleProperty,
                .propertyName = "stroke",
                .propertyValue = "#ff0000",
            }});

  repro::ReproEvent firstMouseDown;
  firstMouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  firstMouseDown.mouseButton = 0;
  pushFrame(3, Vector2d(10.0, 40.0), 1, {}, {firstMouseDown});
  repro::ReproEvent firstMouseUp;
  firstMouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  firstMouseUp.mouseButton = 0;
  pushFrame(4, Vector2d(10.0, 40.0), 0, {}, {firstMouseUp});
  pushFrame(5, Vector2d(10.0, 40.0), 0);

  repro::ReproEvent secondMouseDown;
  secondMouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  secondMouseDown.mouseButton = 0;
  pushFrame(6, Vector2d(70.0, 40.0), 1, {}, {secondMouseDown});
  pushFrame(7, Vector2d(70.0, 0.0), 1);
  for (std::uint64_t index = 0; index < trailingHoldFrames; ++index) {
    pushFrame(8 + index, Vector2d(70.0, 0.0), 1);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

std::optional<std::filesystem::path> WriteSemanticPenFillAfterCommitReplay(
    const std::filesystem::path& outputDir, std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_semantic_pen_fill_after_commit.svg";
  file.metadata.svgBasename = "semantic_pen_fill_after_commit.svg";
  file.metadata.svgContentHash = "fnv1a64:semantic-pen-fill-after-commit";
  file.metadata.svgSource = std::string(kBlankPenCanvasSvg);
  file.metadata.windowWidth = 640;
  file.metadata.windowHeight = 480;
  file.metadata.displayScale = 1.0;

  const auto pushFrame = [&](std::uint64_t index, Vector2d mouseDoc, int mouseButtonMask,
                             std::vector<repro::ReproAction> actions = {},
                             std::vector<repro::ReproEvent> events = {}) {
    repro::ReproFrame frame;
    frame.index = index;
    frame.timestampSeconds = static_cast<double>(index) / 60.0;
    frame.deltaMs = 1000.0 / 60.0;
    frame.mouseX = mouseDoc.x;
    frame.mouseY = mouseDoc.y;
    frame.mouseDocX = mouseDoc.x;
    frame.mouseDocY = mouseDoc.y;
    frame.mouseButtonMask = mouseButtonMask;
    frame.actions = std::move(actions);
    frame.events = std::move(events);
    file.frames.push_back(std::move(frame));
  };

  const auto pushClick = [&](std::uint64_t downFrame, Vector2d mouseDoc) {
    repro::ReproEvent mouseDown;
    mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
    mouseDown.mouseButton = 0;
    pushFrame(downFrame, mouseDoc, 1, {}, {mouseDown});

    repro::ReproEvent mouseUp;
    mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
    mouseUp.mouseButton = 0;
    pushFrame(downFrame + 1, mouseDoc, 0, {}, {mouseUp});
  };

  pushFrame(0, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetActiveTool,
                .tool = "pen",
            }});
  pushFrame(1, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetStyleProperty,
                .propertyName = "fill",
                .propertyValue = "none",
            }});
  pushFrame(2, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetStyleProperty,
                .propertyName = "stroke",
                .propertyValue = "black",
            }});
  pushClick(3, Vector2d(10.0, 10.0));
  pushClick(5, Vector2d(70.0, 10.0));
  pushClick(7, Vector2d(40.0, 70.0));
  pushClick(9, Vector2d(10.0, 10.0));
  for (std::uint64_t index = 11; index < 18; ++index) {
    pushFrame(index, Vector2d::Zero(), 0);
  }
  pushFrame(18, Vector2d::Zero(), 0,
            {repro::ReproAction{
                .kind = repro::ReproAction::Kind::SetStyleProperty,
                .propertyName = "fill",
                .propertyValue = "#36c317",
            }});
  for (std::uint64_t index = 19; index <= 42; ++index) {
    pushFrame(index, Vector2d::Zero(), 0);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

repro::ReproViewport DonnerDViewport(double zoom) {
  repro::ReproViewport viewport;
  viewport.paneOriginX = 568.0;
  viewport.paneOriginY = 29.0;
  viewport.paneSizeW = 604.0;
  viewport.paneSizeH = 863.0;
  viewport.devicePixelRatio = 2.0;
  viewport.zoom = zoom;
  viewport.panDocX = 302.0;
  viewport.panDocY = 390.0;
  viewport.panScreenX = 870.0;
  viewport.panScreenY = 460.5;
  viewport.viewBoxX = 0.0;
  viewport.viewBoxY = 0.0;
  viewport.viewBoxW = 892.0;
  viewport.viewBoxH = 512.0;
  return viewport;
}

repro::ReproViewport DonnerNViewport(double zoom) {
  repro::ReproViewport viewport = DonnerDViewport(zoom);
  viewport.panDocX = 505.0;
  viewport.panDocY = 335.0;
  return viewport;
}

Vector2d ScreenFromDoc(const repro::ReproViewport& viewport, const Vector2d& docPoint) {
  return Vector2d(viewport.panScreenX + (docPoint.x - viewport.panDocX) * viewport.zoom,
                  viewport.panScreenY + (docPoint.y - viewport.panDocY) * viewport.zoom);
}

void PushDonnerDReplayFrame(repro::ReproFile& file, std::uint64_t index,
                            const repro::ReproViewport& viewport, const Vector2d& mouseDoc,
                            int mouseButtonMask, std::vector<repro::ReproEvent> events = {},
                            std::vector<repro::ReproAction> actions = {}) {
  const Vector2d mouseScreen = ScreenFromDoc(viewport, mouseDoc);
  repro::ReproFrame frame;
  frame.index = index;
  frame.timestampSeconds = static_cast<double>(index) / 60.0;
  frame.deltaMs = 1000.0 / 60.0;
  frame.mouseX = mouseScreen.x;
  frame.mouseY = mouseScreen.y;
  frame.mouseDocX = mouseDoc.x;
  frame.mouseDocY = mouseDoc.y;
  frame.mouseButtonMask = mouseButtonMask;
  frame.viewport = viewport;
  frame.events = std::move(events);
  frame.actions = std::move(actions);
  file.frames.push_back(std::move(frame));
}

// High-zoom rapid pan: a solid-green document viewed at zoom 4 (well past the
// viewport-bounded raster threshold), settled, then panned 400 screen px over
// 10 frames while completed worker results are withheld 6 polls - the async
// pipeline lags the gesture like a real high-zoom pan. Every pane pixel maps
// to a green document point the whole time, so any non-green pane region in a
// mid-pan capture is presentation clipping (evicted or missing fallback
// coverage), never actual document transparency.
std::optional<std::filesystem::path> WriteHighZoomPanReplay(const std::filesystem::path& outputDir,
                                                            std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "missing_high_zoom_pan.svg";
  file.metadata.svgBasename = "high_zoom_pan.svg";
  file.metadata.svgContentHash = "fnv1a64:high-zoom-pan";
  // Alternating green/blue vertical stripes so panned content visibly moves;
  // any pane pixel that is neither stripe color is missing coverage.
  {
    std::string svg =
        R"(<svg xmlns="http://www.w3.org/2000/svg" width="892" height="512" viewBox="0 0 892 512">)";
    for (int x = 0; x < 892; x += 100) {
      svg += "<rect x=\"" + std::to_string(x) +
             "\" y=\"0\" width=\"50\" height=\"512\" fill=\"#00cc44\"/>";
      svg += "<rect x=\"" + std::to_string(x + 50) +
             "\" y=\"0\" width=\"50\" height=\"512\" fill=\"#2255ee\"/>";
    }
    svg += "</svg>";
    file.metadata.svgSource = svg;
  }
  file.metadata.windowWidth = 1600;
  file.metadata.windowHeight = 900;
  file.metadata.displayScale = 2.0;

  const auto viewportAt = [&](double zoom, double panScreenX) {
    repro::ReproViewport viewport = DonnerDViewport(zoom);
    viewport.panDocX = 446.0;
    viewport.panDocY = 256.0;
    viewport.panScreenX = panScreenX;
    viewport.panScreenY = 460.5;
    return viewport;
  };
  const Vector2d mouseDoc(446.0, 256.0);

  // Settle at the initial viewport so the bounded raster and the overview
  // infill both land.
  for (std::uint64_t index = 0; index <= 11; ++index) {
    PushDonnerDReplayFrame(file, index, viewportAt(4.0, 870.0), mouseDoc, 0);
  }
  // Frames 12-19: grab the pane's right-edge scrollbar strip and drag it
  // down. The canvas scrollbar must pan the DOCUMENT (or do nothing), never
  // window-scroll the pane's overlay chrome (toolbar, perf HUD) - dragging
  // the old ImGui window scrollbar here scrolled the chrome away from the
  // canvas. Screen x=1165 sits in the scrollbar strip (pane right edge is
  // 568+604=1172); the doc points below map to (1165, 300..420) at this
  // viewport via ScreenFromDoc.
  const auto docAtScreen = [&](double screenX, double screenY) {
    return Vector2d(446.0 + (screenX - 870.0) / 4.0, 256.0 + (screenY - 460.5) / 4.0);
  };
  {
    repro::ReproEvent mouseDown;
    mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
    mouseDown.mouseButton = 0;
    PushDonnerDReplayFrame(file, 12, viewportAt(4.0, 870.0), docAtScreen(1165.0, 300.0), 1,
                           {mouseDown});
    for (std::uint64_t index = 13; index <= 17; ++index) {
      const double screenY = 300.0 + 24.0 * static_cast<double>(index - 12);
      PushDonnerDReplayFrame(file, index, viewportAt(4.0, 870.0), docAtScreen(1165.0, screenY), 1);
    }
    repro::ReproEvent mouseUp;
    mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
    mouseUp.mouseButton = 0;
    PushDonnerDReplayFrame(file, 18, viewportAt(4.0, 870.0), docAtScreen(1165.0, 420.0), 0,
                           {mouseUp});
  }
  for (std::uint64_t index = 19; index <= 20; ++index) {
    PushDonnerDReplayFrame(file, index, viewportAt(4.0, 870.0), mouseDoc, 0);
  }
  // Rapid pan: +40 screen px per frame for 10 frames (400 px total, ~3x the
  // 128 px high-zoom raster margin).
  for (std::uint64_t index = 21; index <= 30; ++index) {
    const double panScreenX = 870.0 + 40.0 * static_cast<double>(index - 20);
    PushDonnerDReplayFrame(file, index, viewportAt(4.0, panScreenX), mouseDoc, 0);
  }
  // Hold the final pan viewport briefly (still within the withheld window).
  for (std::uint64_t index = 31; index <= 33; ++index) {
    PushDonnerDReplayFrame(file, index, viewportAt(4.0, 1270.0), mouseDoc, 0);
  }
  // Rapid zoom out 4.0 -> 2.0 around the same anchor over 8 frames.
  for (std::uint64_t index = 34; index <= 41; ++index) {
    const double zoom = 4.0 - 0.25 * static_cast<double>(index - 33);
    PushDonnerDReplayFrame(file, index, viewportAt(zoom, 1270.0), mouseDoc, 0);
  }
  for (std::uint64_t index = 42; index <= 44; ++index) {
    PushDonnerDReplayFrame(file, index, viewportAt(2.0, 1270.0), mouseDoc, 0);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

/// Pen clicks placing three anchors, then a close-path click back on the
/// first anchor. After closePath() the tool is no longer shaping an anchor,
/// so the close click exercises the non-drag overlay-refresh path on its
/// flush frame. Driven through document-space input with deterministic
/// worker draining: the Realtime scheduling this replay originally used
/// dropped the clicks entirely on loaded executors (the worker never went
/// idle inside the replay's 40 frames), making the test
/// environment-sensitive.
std::optional<std::filesystem::path> WritePenClosePathClickReplay(
    const std::filesystem::path& outputDir, std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "donner_splash.svg";
  file.metadata.svgBasename = "donner_splash.svg";
  file.metadata.svgContentHash = "fnv1a64:donner-splash-runfile";
  file.metadata.windowWidth = 1600;
  file.metadata.windowHeight = 900;
  file.metadata.displayScale = 2.0;

  const repro::ReproViewport viewport = DonnerDViewport(2.0);
  const Vector2d kAnchor1Doc(302.0, 390.0);
  const Vector2d kAnchor2Doc(322.0, 400.0);
  const Vector2d kAnchor3Doc(302.0, 410.0);

  PushDonnerDReplayFrame(file, 0, viewport, kAnchor1Doc, 0, {},
                         {repro::ReproAction{
                             .kind = repro::ReproAction::Kind::SetActiveTool,
                             .tool = "pen",
                         }});
  for (std::uint64_t index = 1; index <= 20; ++index) {
    PushDonnerDReplayFrame(file, index, viewport, kAnchor1Doc, 0);
  }

  const auto pushClick = [&](std::uint64_t pressFrame, const Vector2d& mouseDoc) {
    repro::ReproEvent mouseDown;
    mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
    mouseDown.mouseButton = 0;
    PushDonnerDReplayFrame(file, pressFrame, viewport, mouseDoc, 1, {mouseDown});
    repro::ReproEvent mouseUp;
    mouseUp.kind = repro::ReproEvent::Kind::MouseUp;
    mouseUp.mouseButton = 0;
    PushDonnerDReplayFrame(file, pressFrame + 1, viewport, mouseDoc, 0, {mouseUp});
  };

  pushClick(21, kAnchor1Doc);
  PushDonnerDReplayFrame(file, 23, viewport, kAnchor2Doc, 0);
  pushClick(24, kAnchor2Doc);
  PushDonnerDReplayFrame(file, 26, viewport, kAnchor3Doc, 0);
  pushClick(27, kAnchor3Doc);
  PushDonnerDReplayFrame(file, 29, viewport, kAnchor1Doc, 0);
  // Close-path click back on the first anchor.
  pushClick(30, kAnchor1Doc);
  for (std::uint64_t index = 32; index <= 40; ++index) {
    PushDonnerDReplayFrame(file, index, viewport, kAnchor1Doc, 0);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

std::optional<std::filesystem::path> WriteDonnerDDragZoomReplay(
    const std::filesystem::path& outputDir, std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "donner_splash.svg";
  file.metadata.svgBasename = "donner_splash.svg";
  file.metadata.svgContentHash = "fnv1a64:donner-splash-runfile";
  file.metadata.windowWidth = 1600;
  file.metadata.windowHeight = 900;
  file.metadata.displayScale = 2.0;
  file.metadata.expect = repro::ReproExpectation{
      .proofKind = repro::ReproExpectationProofKind::Selection,
      .leftMouseDownOrdinal = 1,
      .frameOffsetAfterLeftMouseDown = 18,
      .minFrameIndex = 40,
      .maxFrameIndex = 40,
      .targetSelector = "#Donner_D",
      .cropMode = "document-canvas",
      .expectedSelectionLabel = std::string("<path> #Donner_D"),
  };

  const Vector2d kDonnerDLeftStemDoc(282.0, 390.0);
  for (std::uint64_t frame = 0; frame <= 20; ++frame) {
    PushDonnerDReplayFrame(file, frame, DonnerDViewport(2.0), kDonnerDLeftStemDoc, 0);
  }

  repro::ReproEvent mouseDown;
  mouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  mouseDown.mouseButton = 0;
  mouseDown.hit = repro::ReproHit{
      .id = "Donner_D",
      .tag = "path",
  };
  PushDonnerDReplayFrame(file, 21, DonnerDViewport(2.0), kDonnerDLeftStemDoc, 1, {mouseDown});

  for (std::uint64_t frame = 22; frame <= 30; ++frame) {
    const double dragOffset = static_cast<double>(frame - 21) * 1.5;
    PushDonnerDReplayFrame(file, frame, DonnerDViewport(2.0),
                           kDonnerDLeftStemDoc + Vector2d(dragOffset, -dragOffset * 0.25), 1);
  }

  for (std::uint64_t frame = 31; frame <= 40; ++frame) {
    const double t = static_cast<double>(frame - 31);
    const double zoom = 2.0 + t * 0.16;
    const Vector2d dragDoc = kDonnerDLeftStemDoc + Vector2d(15.0 + t * 0.6, -3.0 - t * 0.35);
    PushDonnerDReplayFrame(file, frame, DonnerDViewport(zoom), dragDoc, 1);
  }

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

std::optional<std::filesystem::path> WriteDonnerDZoomThenDragReplay(
    const std::filesystem::path& outputDir, std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "donner_splash.svg";
  file.metadata.svgBasename = "donner_splash.svg";
  file.metadata.svgContentHash = "fnv1a64:donner-splash-runfile";
  file.metadata.windowWidth = 1600;
  file.metadata.windowHeight = 900;
  file.metadata.displayScale = 2.0;
  file.metadata.expect = repro::ReproExpectation{
      .proofKind = repro::ReproExpectationProofKind::Selection,
      .leftMouseDownOrdinal = 2,
      .frameOffsetAfterLeftMouseDown = 2,
      .minFrameIndex = 38,
      .maxFrameIndex = 39,
      .targetSelector = "#Donner_D",
      .cropMode = "document-canvas",
      .expectedSelectionLabel = std::string("<path> #Donner_D"),
  };

  const Vector2d kDonnerDLeftStemDoc(282.0, 390.0);
  for (std::uint64_t frame = 0; frame <= 4; ++frame) {
    PushDonnerDReplayFrame(file, frame, DonnerDViewport(2.0), kDonnerDLeftStemDoc, 0);
  }

  repro::ReproEvent selectMouseDown;
  selectMouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  selectMouseDown.mouseButton = 0;
  selectMouseDown.hit = repro::ReproHit{
      .id = "Donner_D",
      .tag = "path",
  };
  PushDonnerDReplayFrame(file, 5, DonnerDViewport(2.0), kDonnerDLeftStemDoc, 1, {selectMouseDown});

  repro::ReproEvent selectMouseUp;
  selectMouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  selectMouseUp.mouseButton = 0;
  PushDonnerDReplayFrame(file, 6, DonnerDViewport(2.0), kDonnerDLeftStemDoc, 0, {selectMouseUp});

  for (std::uint64_t frame = 7; frame <= 18; ++frame) {
    PushDonnerDReplayFrame(file, frame, DonnerDViewport(2.0), kDonnerDLeftStemDoc, 0);
  }

  for (std::uint64_t frame = 19; frame <= 35; ++frame) {
    const double t = static_cast<double>(frame - 18);
    PushDonnerDReplayFrame(file, frame, DonnerDViewport(2.0 + t * (1.55 / 17.0)),
                           kDonnerDLeftStemDoc, 0);
  }

  repro::ReproEvent dragMouseDown;
  dragMouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  dragMouseDown.mouseButton = 0;
  dragMouseDown.hit = repro::ReproHit{
      .id = "Donner_D",
      .tag = "path",
  };
  PushDonnerDReplayFrame(file, 36, DonnerDViewport(3.55), kDonnerDLeftStemDoc, 1, {dragMouseDown});

  for (std::uint64_t frame = 37; frame <= 42; ++frame) {
    const double t = static_cast<double>(frame - 36);
    PushDonnerDReplayFrame(file, frame, DonnerDViewport(3.55),
                           kDonnerDLeftStemDoc + Vector2d(t * 3.2, t * -0.75), 1);
  }

  repro::ReproEvent dragMouseUp;
  dragMouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  dragMouseUp.mouseButton = 0;
  PushDonnerDReplayFrame(file, 43, DonnerDViewport(3.55),
                         kDonnerDLeftStemDoc + Vector2d(7.0 * 3.2, 7.0 * -0.75), 0, {dragMouseUp});

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

std::optional<std::filesystem::path> WriteDonnerNFarZoomThenDragReplay(
    const std::filesystem::path& outputDir, std::string_view name) {
  std::error_code createDirError;
  std::filesystem::create_directories(outputDir, createDirError);
  if (createDirError) {
    ADD_FAILURE() << "failed to create " << outputDir << ": " << createDirError.message();
    return std::nullopt;
  }

  repro::ReproFile file;
  file.metadata.svgPath = "donner_splash.svg";
  file.metadata.svgBasename = "donner_splash.svg";
  file.metadata.svgContentHash = "fnv1a64:donner-splash-runfile";
  file.metadata.windowWidth = 1600;
  file.metadata.windowHeight = 900;
  file.metadata.displayScale = 2.0;
  file.metadata.expect = repro::ReproExpectation{
      .proofKind = repro::ReproExpectationProofKind::Selection,
      .leftMouseDownOrdinal = 2,
      .frameOffsetAfterLeftMouseDown = 2,
      .minFrameIndex = 55,
      .maxFrameIndex = 56,
      .targetSelector = "#Donner_N_2",
      .cropMode = "document-canvas",
      .expectedSelectionLabel = std::string("<polygon> #Donner_N_2"),
  };

  const Vector2d kDonnerNDoc(505.0, 335.0);
  for (std::uint64_t frame = 0; frame <= 4; ++frame) {
    PushDonnerDReplayFrame(file, frame, DonnerNViewport(2.0), kDonnerNDoc, 0);
  }

  repro::ReproEvent selectMouseDown;
  selectMouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  selectMouseDown.mouseButton = 0;
  selectMouseDown.hit = repro::ReproHit{
      .id = "Donner_N_2",
      .tag = "polygon",
  };
  PushDonnerDReplayFrame(file, 5, DonnerNViewport(2.0), kDonnerNDoc, 1, {selectMouseDown});

  repro::ReproEvent selectMouseUp;
  selectMouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  selectMouseUp.mouseButton = 0;
  PushDonnerDReplayFrame(file, 6, DonnerNViewport(2.0), kDonnerNDoc, 0, {selectMouseUp});

  for (std::uint64_t frame = 7; frame <= 12; ++frame) {
    PushDonnerDReplayFrame(file, frame, DonnerNViewport(2.0), kDonnerNDoc, 0);
  }

  for (std::uint64_t frame = 13; frame <= 26; ++frame) {
    const double t = static_cast<double>(frame - 13) / 13.0;
    PushDonnerDReplayFrame(file, frame, DonnerNViewport(2.0 + t * 6.0), kDonnerNDoc, 0);
  }
  for (std::uint64_t frame = 27; frame <= 38; ++frame) {
    const double t = static_cast<double>(frame - 27) / 11.0;
    PushDonnerDReplayFrame(file, frame, DonnerNViewport(8.0 - t * 4.5), kDonnerNDoc, 0);
  }
  for (std::uint64_t frame = 39; frame <= 52; ++frame) {
    const double t = static_cast<double>(frame - 39) / 13.0;
    PushDonnerDReplayFrame(file, frame, DonnerNViewport(3.5 + t * 8.5), kDonnerNDoc, 0);
  }

  repro::ReproEvent dragMouseDown;
  dragMouseDown.kind = repro::ReproEvent::Kind::MouseDown;
  dragMouseDown.mouseButton = 0;
  dragMouseDown.hit = repro::ReproHit{
      .id = "Donner_N_2",
      .tag = "polygon",
  };
  PushDonnerDReplayFrame(file, 53, DonnerNViewport(12.0), kDonnerNDoc, 1, {dragMouseDown});

  for (std::uint64_t frame = 54; frame <= 60; ++frame) {
    const double t = static_cast<double>(frame - 53);
    PushDonnerDReplayFrame(file, frame, DonnerNViewport(12.0),
                           kDonnerNDoc + Vector2d(t * 1.4, t * -0.45), 1);
  }

  repro::ReproEvent dragMouseUp;
  dragMouseUp.kind = repro::ReproEvent::Kind::MouseUp;
  dragMouseUp.mouseButton = 0;
  PushDonnerDReplayFrame(file, 61, DonnerNViewport(12.0),
                         kDonnerNDoc + Vector2d(8.0 * 1.4, 8.0 * -0.45), 0, {dragMouseUp});

  const std::filesystem::path replayPath = outputDir / std::string(name);
  if (!repro::WriteReproFile(replayPath, file)) {
    ADD_FAILURE() << "failed to write " << replayPath;
    return std::nullopt;
  }
  return replayPath;
}

Vector2d PresentedDragTargetTranslationOrZero(
    const repro::GlRnrReplayFrameDiagnostics& diagnostics) {
  for (const repro::GlRnrReplayTileDiagnostics& tile : diagnostics.tiles) {
    if (tile.isDragTarget) {
      return tile.presentedDragTranslationDoc;
    }
  }
  return Vector2d::Zero();
}

std::string CanonicalReplayDiagnostics(const repro::GlRnrReplayResult& result,
                                       std::optional<std::uint64_t> firstFrame = std::nullopt,
                                       std::optional<std::uint64_t> lastFrame = std::nullopt) {
  std::ostringstream out;
  const auto writeVec = [&out](const Vector2i& value) { out << value.x << ',' << value.y; };
  const auto writeVecD = [&out](const Vector2d& value) { out << value.x << ',' << value.y; };

  for (const repro::GlRnrReplayFrameDiagnostics& frame : result.frameDiagnostics) {
    if ((firstFrame.has_value() && frame.frameIndex < *firstFrame) ||
        (lastFrame.has_value() && frame.frameIndex > *lastFrame)) {
      continue;
    }
    out << "frame=" << frame.frameIndex << ";fresh=" << static_cast<int>(frame.canvasFreshness)
        << ";status=" << frame.statusSuffix << ";viewport=";
    writeVec(frame.viewportDesiredCanvas);
    out << ";document=";
    writeVec(frame.documentCanvas);
    out << ";compositor=";
    writeVec(frame.compositorCanvas);
    out << ";metadata_miss=" << frame.metadataOnlyMissCount
        << ";duplicate_textures=" << frame.duplicateLiveTextureCount;
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

svg::RendererBitmap SolidBitmapWithCenterPixelColor(const svg::RendererBitmap& source,
                                                    const Vector2i& dimensions) {
  const svg::RendererBitmap normalizedSource = NormalizeBitmap(source);
  svg::RendererBitmap result;
  result.dimensions = dimensions;
  result.rowBytes = static_cast<std::size_t>(std::max(dimensions.x, 0)) * 4u;
  result.alphaType = normalizedSource.alphaType;
  if (normalizedSource.empty() || dimensions.x <= 0 || dimensions.y <= 0) {
    return result;
  }

  const int sampleX = normalizedSource.dimensions.x / 2;
  const int sampleY = normalizedSource.dimensions.y / 2;
  const std::size_t sampleOffset = static_cast<std::size_t>(sampleY) * normalizedSource.rowBytes +
                                   static_cast<std::size_t>(sampleX) * 4u;
  const std::array<std::uint8_t, 4> rgba{
      normalizedSource.pixels[sampleOffset],
      normalizedSource.pixels[sampleOffset + 1],
      normalizedSource.pixels[sampleOffset + 2],
      normalizedSource.pixels[sampleOffset + 3],
  };

  result.pixels.resize(result.rowBytes * static_cast<std::size_t>(dimensions.y));
  for (std::size_t i = 0; i < result.pixels.size(); i += 4u) {
    result.pixels[i] = rgba[0];
    result.pixels[i + 1] = rgba[1];
    result.pixels[i + 2] = rgba[2];
    result.pixels[i + 3] = rgba[3];
  }
  return result;
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

int CountPenFillGreenPixelsInDocumentCanvas(const svg::RendererBitmap& bitmap) {
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const std::uint8_t red = bitmap.pixels[offset];
      const std::uint8_t green = bitmap.pixels[offset + 1];
      const std::uint8_t blue = bitmap.pixels[offset + 2];
      const std::uint8_t alpha = bitmap.pixels[offset + 3];
      if (alpha > 180 && green > 135 && red < 135 && blue < 120 && green > red + 45 &&
          green > blue + 60) {
        ++count;
      }
    }
  }
  return count;
}

int CountBrightRedPixels(const svg::RendererBitmap& bitmap) {
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const std::uint8_t red = bitmap.pixels[offset];
      const std::uint8_t green = bitmap.pixels[offset + 1];
      const std::uint8_t blue = bitmap.pixels[offset + 2];
      const std::uint8_t alpha = bitmap.pixels[offset + 3];
      if (alpha > 200 && red > 200 && green < 40 && blue < 40) {
        ++count;
      }
    }
  }
  return count;
}

// Counts bright-red pixels in the capture rows strictly below the given
// document-space Y threshold. The document-canvas crop maps the 80x80 pen
// canvas onto the full bitmap, so the row cutoff is `docY / 80` of the height.
int CountBrightRedPixelsBelowDocY(const svg::RendererBitmap& bitmap, double docY) {
  int count = 0;
  const int firstRow =
      static_cast<int>(std::ceil(static_cast<double>(bitmap.dimensions.y) * (docY / 80.0)));
  for (int y = firstRow; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const std::uint8_t red = bitmap.pixels[offset];
      const std::uint8_t green = bitmap.pixels[offset + 1];
      const std::uint8_t blue = bitmap.pixels[offset + 2];
      const std::uint8_t alpha = bitmap.pixels[offset + 3];
      if (alpha > 200 && red > 200 && green < 40 && blue < 40) {
        ++count;
      }
    }
  }
  return count;
}

// Counts cyan-tinted chrome pixels in the horizontal corridor doc-x in
// [xMinDoc, xMaxDoc], doc-y within ±2 of yDoc, on the 80x80 pen canvas
// mapped over the full capture. Matches the semi-transparent cyan the pen
// rubber-band preview strokes with, over any checkerboard shade.
int CountPenPreviewCyanPixelsInCorridor(const svg::RendererBitmap& bitmap, double xMinDoc,
                                        double xMaxDoc, double yDoc) {
  const auto docToPxX = [&](double x) {
    return static_cast<int>(std::lround(static_cast<double>(bitmap.dimensions.x) * (x / 80.0)));
  };
  const auto docToPxY = [&](double y) {
    return static_cast<int>(std::lround(static_cast<double>(bitmap.dimensions.y) * (y / 80.0)));
  };
  int count = 0;
  const int y0 = std::max(0, docToPxY(yDoc - 2.0));
  const int y1 = std::min(bitmap.dimensions.y - 1, docToPxY(yDoc + 2.0));
  const int x0 = std::max(0, docToPxX(xMinDoc));
  const int x1 = std::min(bitmap.dimensions.x - 1, docToPxX(xMaxDoc));
  for (int y = y0; y <= y1; ++y) {
    for (int x = x0; x <= x1; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const std::uint8_t red = bitmap.pixels[offset];
      const std::uint8_t green = bitmap.pixels[offset + 1];
      const std::uint8_t blue = bitmap.pixels[offset + 2];
      // The 0xa0-alpha cyan stroke blends over the dark canvas checkerboard
      // to roughly (15-25, 140-150, 175-185).
      if (blue > 120 && green > 100 && red < 100 && blue > red + 80 && green > red + 60) {
        ++count;
      }
    }
  }
  return count;
}

int CountLegacyBluePenPixels(const svg::RendererBitmap& bitmap) {
  int count = 0;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
      const std::uint8_t red = bitmap.pixels[offset];
      const std::uint8_t green = bitmap.pixels[offset + 1];
      const std::uint8_t blue = bitmap.pixels[offset + 2];
      const std::uint8_t alpha = bitmap.pixels[offset + 3];
      if (alpha > 120 && red < 80 && green >= 60 && green < 170 && blue > 180 &&
          blue > green + 70) {
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
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  std::optional<svg::RendererBitmap> actual = LoadCaptureBitmap(result, 1);
  ASSERT_TRUE(actual.has_value());
  std::optional<svg::RendererBitmap> rendererReference =
      RenderGroundTruth(kStaticContentOnlySvg, Vector2i(200, 120));
  ASSERT_TRUE(rendererReference.has_value());
  const svg::RendererBitmap expected =
      SolidBitmapWithCenterPixelColor(*rendererReference, actual->dimensions);
  tests::CompareBitmapToBitmap(NormalizeBitmap(*actual), expected,
                               "gl_content_only_capture_vs_renderer",
                               tests::PixelmatchIdentityParams());

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, DirectDocumentCanvasCaptureIsNotDimmedByRenderPaneBackground) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "direct_document_not_dimmed";
  const std::optional<std::filesystem::path> replayPath =
      WriteStaticContentReplay(outputDir, "direct_document_not_dimmed.rnr", 1);
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(1);
  options.maxFrame = 1;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.contentOnlyCapture = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  std::optional<svg::RendererBitmap> actual = LoadCaptureBitmap(result, 1);
  ASSERT_TRUE(actual.has_value());
  ASSERT_GT(actual->dimensions.x, 0);
  ASSERT_GT(actual->dimensions.y, 0);

  const int centerX = actual->dimensions.x / 2;
  const int centerY = actual->dimensions.y / 2;
  const std::size_t centerOffset =
      static_cast<std::size_t>(centerY) * actual->rowBytes + static_cast<std::size_t>(centerX) * 4u;
  EXPECT_NEAR(static_cast<int>(actual->pixels[centerOffset]), 0x10, 3);
  EXPECT_NEAR(static_cast<int>(actual->pixels[centerOffset + 1]), 0x20, 3);
  EXPECT_NEAR(static_cast<int>(actual->pixels[centerOffset + 2]), 0x30, 3)
      << "Direct framebuffer document presentation must not sit behind an opaque ImGui render-pane "
         "background.";
  EXPECT_EQ(actual->pixels[centerOffset + 3], 255);

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, DirectFramebufferCheckerboardUsesSingleDrawDuringDrag) {
  svg::Renderer renderer;
  if (!renderer.requiresTextureSnapshotPresentation()) {
    GTEST_SKIP() << "Direct framebuffer presentation requires the Geode renderer backend.";
  }

  const std::filesystem::path outputDir = DiagnosticOutputDir() / "direct_checkerboard_single_draw";
  const std::optional<std::filesystem::path> replayPath =
      WriteDirectPresentationDragReplay(outputDir, "direct_checkerboard_single_draw.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(20);
  options.maxFrame = 20;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  int directDragFrames = 0;
  for (const repro::GlRnrReplayFrameDiagnostics& diagnostics : result.frameDiagnostics) {
    if (!diagnostics.frameCost.overlay.hasLiveDragPreview ||
        diagnostics.frameCost.directPresentation.totalMs <= 0.0) {
      continue;
    }

    ++directDragFrames;
    EXPECT_EQ(diagnostics.frameCost.directPresentation.checkerboardDrawCount, 1)
        << "frame " << diagnostics.frameIndex
        << " should render the direct framebuffer checkerboard with one GPU draw";
  }
  EXPECT_GT(directDragFrames, 0) << "test setup must exercise direct drag presentation";

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, DocumentSpaceInputDrivesCanvasSelectionThroughEditorShell) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "document_space_input";
  const std::optional<std::filesystem::path> replayPath =
      WriteDocumentSpaceDragReplay(outputDir, "document_space_input.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(2);
  options.maxFrame = 2;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  ASSERT_TRUE(result.finalSelectedElementLabel.has_value());
  EXPECT_EQ(*result.finalSelectedElementLabel, "<rect> #target");
  ASSERT_EQ(result.captures.size(), 1u);
  EXPECT_EQ(result.captures.front().frameIndex, 2u);

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, SemanticPenPaintActionsRenderThroughEditorShell) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "semantic_pen_paint";
  const std::optional<std::filesystem::path> replayPath =
      WriteSemanticPenPaintReplay(outputDir, "semantic_pen_paint.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(20);
  options.maxFrame = 20;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  std::optional<svg::RendererBitmap> actual = LoadCaptureBitmap(result, 20);
  ASSERT_TRUE(actual.has_value());
  EXPECT_GT(CountBrightRedPixels(*actual), 500)
      << "Semantic .rnr PenTool and paint actions must create a visible filled path through the "
         "real EditorShell replay path.";

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, PenDragUpdatesSelectedPathDataBeforeMouseUp) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "pen_drag_live_path";
  const std::optional<std::filesystem::path> replayPath =
      WritePenDragReplay(outputDir, "pen_drag_live_path.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(7);
  options.maxFrame = 7;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  const repro::GlRnrReplayFrameDiagnostics* diagnostics = FindFrameDiagnostics(result, 7);
  ASSERT_NE(diagnostics, nullptr);
  ASSERT_TRUE(diagnostics->selectedPathDataAttribute.has_value());
  EXPECT_EQ(*diagnostics->selectedPathDataAttribute, "M 10 10 C 10 10 40 -10 40 10")
      << "The live EditorShell path data should reflect the in-progress pen drag before mouseup.";
  EXPECT_TRUE(diagnostics->lastFlushAppliedCommands);
  EXPECT_FALSE(diagnostics->lastFlushCacheInvalidatedElements.empty());

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, PenDragUsesCyanOverlayWithoutLegacyBluePath) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "pen_drag_cyan_overlay";
  const std::optional<std::filesystem::path> replayPath =
      WritePenDragReplay(outputDir, "pen_drag_cyan_overlay.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames = {7, 9};
  options.maxFrame = 9;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  const repro::GlRnrReplayFrameDiagnostics* dragFrame = FindFrameDiagnostics(result, 7);
  ASSERT_NE(dragFrame, nullptr);
  EXPECT_GT(dragFrame->documentFrameVersion, dragFrame->displayedDocVersion)
      << "The drag frame should model the real async gap: DOM path data has advanced before the "
         "matching document pixels are presented.";
  ASSERT_TRUE(dragFrame->immediateOverlayDocumentVersion.has_value());
  EXPECT_EQ(*dragFrame->immediateOverlayDocumentVersion, dragFrame->documentFrameVersion)
      << "Active PenTool drags should use the renderer-backed cyan path overlay for the live path "
         "without drawing a second legacy blue path.";

  std::optional<svg::RendererBitmap> dragCapture = LoadCaptureBitmap(result, 7);
  ASSERT_TRUE(dragCapture.has_value());
  EXPECT_EQ(CountLegacyBluePenPixels(*dragCapture), 0)
      << "The document-canvas capture should contain no legacy blue PenTool path pixels.";

  // The moving-drag frame (7) defers its crisp render - the live preview
  // presents that geometry - so the raster catches up one hold frame after
  // the pointer pauses: frame 8 issues the request, frame 9 presents it.
  const repro::GlRnrReplayFrameDiagnostics* presentedFrame = FindFrameDiagnostics(result, 9);
  ASSERT_NE(presentedFrame, nullptr);
  ASSERT_TRUE(presentedFrame->immediateOverlayDocumentVersion.has_value());
  EXPECT_EQ(*presentedFrame->immediateOverlayDocumentVersion, presentedFrame->displayedDocVersion);
  EXPECT_EQ(presentedFrame->documentFrameVersion, presentedFrame->displayedDocVersion)
      << "Holding the mouse at the same drag point should not queue another identical path "
         "mutation after the previous drag render lands; otherwise the overlay gate never catches "
         "up to the presented shape.";

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, PenClosePathClickRefreshesOverlayOnFlushFrame) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "pen_close_path_click";
  const std::optional<std::filesystem::path> replayPath =
      WritePenClosePathClickReplay(outputDir, "pen_close_path_click.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = outputDir;
  options.captureFrames.insert(40);
  options.maxFrame = 40;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  // Find the frame where the close-path click committed the trailing Z.
  const repro::GlRnrReplayFrameDiagnostics* closeFrame = nullptr;
  std::string frameDump;
  for (const repro::GlRnrReplayFrameDiagnostics& diagnostics : result.frameDiagnostics) {
    frameDump += "frame " + std::to_string(diagnostics.frameIndex) +
                 ": docVersion=" + std::to_string(diagnostics.documentFrameVersion) +
                 " displayed=" + std::to_string(diagnostics.displayedDocVersion) +
                 " overlayVersion=" +
                 (diagnostics.immediateOverlayDocumentVersion.has_value()
                      ? std::to_string(*diagnostics.immediateOverlayDocumentVersion)
                      : std::string("none")) +
                 " d=" + diagnostics.selectedPathDataAttribute.value_or("(none)") + "\n";
    if (closeFrame == nullptr && diagnostics.selectedPathDataAttribute.has_value() &&
        diagnostics.selectedPathDataAttribute->find(" Z") != std::string::npos) {
      closeFrame = &diagnostics;
    }
  }
  SCOPED_TRACE(frameDump);
  ASSERT_NE(closeFrame, nullptr) << "The close-path click never committed the trailing Z.";

  // Closing the contour ends the anchor drag before the flush runs, so this
  // pins the non-drag pen flush path: the same frame that flushed the close
  // must re-capture overlay chrome from the closed geometry. A stale snapshot
  // here is the user-visible "overlay only updates on the next mousemove" bug.
  ASSERT_TRUE(closeFrame->immediateOverlayDocumentVersion.has_value())
      << "Overlay snapshot was dropped on the close-path flush frame instead of being refreshed.";
  EXPECT_EQ(*closeFrame->immediateOverlayDocumentVersion, closeFrame->documentFrameVersion)
      << "Overlay chrome must be re-captured on the same frame the close-path click flushed, "
         "even though the pen is no longer shaping an anchor after closePath().";

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, PenEscapeCommitsOpenPathInsteadOfDiscarding) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "pen_escape_commits";
  const std::optional<std::filesystem::path> replayPath =
      WritePenEscapeReplay(outputDir, "pen_escape_commits.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(10);
  options.maxFrame = 10;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  // Escape ends the pen session by committing the placed anchors as an open
  // path - the placed segment must survive as a normal selected <path>, not
  // be discarded. (Only a segmentless single-anchor draft is discarded.)
  const repro::GlRnrReplayFrameDiagnostics* finalFrame = FindFrameDiagnostics(result, 10);
  ASSERT_NE(finalFrame, nullptr);
  ASSERT_TRUE(finalFrame->selectedPathDataAttribute.has_value())
      << "Escape must keep (and leave selected) the committed open path; the draft was discarded.";
  EXPECT_EQ(*finalFrame->selectedPathDataAttribute, "M 10 10 L 40 10");

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, PenHoverShowsRubberBandSegmentPreview) {
  svg::Renderer renderer;
  if (!renderer.requiresTextureSnapshotPresentation()) {
    GTEST_SKIP() << "Chrome pixels require the Geode direct presentation path.";
  }

  const std::filesystem::path outputDir = DiagnosticOutputDir() / "pen_hover_preview";
  const std::optional<std::filesystem::path> replayPath =
      WritePenHoverReplay(outputDir, "pen_hover_preview.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(10);
  options.maxFrame = 10;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  // With one anchor placed at (10, 40) and the pointer resting at (70, 40),
  // the presented frame must preview the segment a click would commit: a
  // rubber-band stroke through the corridor between them. The corridor
  // window starts past the anchor chrome so anchor boxes cannot satisfy it.
  std::optional<svg::RendererBitmap> capture = LoadCaptureBitmap(result, 10);
  ASSERT_TRUE(capture.has_value());
  EXPECT_GT(CountPenPreviewCyanPixelsInCorridor(*capture, 25.0, 55.0, 40.0), 20)
      << "hovering after placing an anchor must rubber-band the pending segment. Capture: "
      << FindCapture(result, 10)->path;

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, PenBackspaceRemovesLastAnchorNotWholeDraft) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "pen_backspace";
  const std::optional<std::filesystem::path> replayPath = WritePenTwoAnchorsThenKeyReplay(
      outputDir, "pen_backspace.rnr", static_cast<int>(ImGuiKey_Backspace));
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(10);
  options.maxFrame = 10;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  // Backspace while drafting removes only the LAST placed anchor - the draft
  // path (which is the selection) must survive with its remaining anchor, not
  // fall through to delete-selection and vanish wholesale.
  const repro::GlRnrReplayFrameDiagnostics* finalFrame = FindFrameDiagnostics(result, 10);
  ASSERT_NE(finalFrame, nullptr);
  ASSERT_TRUE(finalFrame->selectedPathDataAttribute.has_value())
      << "Backspace during a pen draft must not delete the whole in-progress path.";
  EXPECT_EQ(*finalFrame->selectedPathDataAttribute, "M 10 10");
}

// The LIVE ImGui pointer path: a plain canvas click (button held across
// frames, then released) must open the text session so typing lands in a new
// <text> element. Reproduces the report that selecting the Text tool and
// clicking showed nothing - the pending-click path started the gesture but
// the live pointer path never delivered the release.
// During a rapid pan at high zoom, every pane pixel must keep showing
// document content: the still-covered part of the previous bounded raster
// stays put, and newly-exposed regions fall back to the overview infill
// until the crisp render catches up. Blank (checkerboard) pane regions are
// the clipping bug this reproduces.
TEST(GlRnrReplayTest, HighZoomRapidPanKeepsPaneCoveredByContent) {
  svg::Renderer probeRenderer;
  if (!probeRenderer.requiresTextureSnapshotPresentation()) {
    GTEST_SKIP() << "Presented-tile coverage requires the Geode direct presentation path.";
  }

  const std::filesystem::path outputDir = DiagnosticOutputDir() / "high_zoom_pan";
  const std::optional<std::filesystem::path> replayPath =
      WriteHighZoomPanReplay(outputDir, "high_zoom_pan.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(20);
  options.captureFrames.insert(26);
  options.captureFrames.insert(33);
  options.captureFrames.insert(38);
  options.captureFrames.insert(44);
  options.maxFrame = 44;
  options.cropMode = repro::GlRnrReplayCropMode::Full;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::HoldFramesBehind;
  options.holdFramesBehind = 6;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  // Pane rect in device pixels (logical origin 568,29 size 604x863 at DPR 2).
  // Inset by 8 logical px so pane-border chrome never counts.
  const auto paneCrop = [&](const svg::RendererBitmap& bitmap) {
    return CropBitmap(*&bitmap, PixelCrop{.x = (568 + 8) * 2,
                                          .y = (29 + 8) * 2,
                                          .width = (604 - 16) * 2,
                                          .height = (863 - 16) * 2});
  };
  // Ratio of pane pixels showing document content (either stripe color).
  // Non-content pixels are the checkerboard/background plus the in-pane
  // overlay chrome (toolbar, perf HUD), which stays constant across frames.
  const auto contentRatio = [&](const svg::RendererBitmap& crop) {
    int content = 0;
    int total = 0;
    for (std::size_t i = 0; i + 3 < crop.pixels.size(); i += 4) {
      ++total;
      const std::uint8_t r = crop.pixels[i];
      const std::uint8_t g = crop.pixels[i + 1];
      const std::uint8_t b = crop.pixels[i + 2];
      const bool green = g > 120 && r < 100 && b < 110;
      const bool blue = b > 150 && r < 110 && g < 130;
      if (green || blue) {
        ++content;
      }
    }
    return total > 0 ? static_cast<double>(content) / static_cast<double>(total) : 0.0;
  };

  // Baseline: the settled pre-pan frame. The in-pane overlay chrome
  // (toolbar, perf HUD) accounts for ~5% of the crop, so full coverage
  // measures ~0.95 rather than 1.0; every later frame is held to the same
  // coverage as this settled baseline (minus a small tolerance).
  std::optional<svg::RendererBitmap> settled = LoadCaptureBitmap(result, 20);
  ASSERT_TRUE(settled.has_value());
  const double settledRatio = contentRatio(paneCrop(*settled));
  EXPECT_GT(settledRatio, 0.90)
      << "settled high-zoom frame must show the document across the whole pane";

  const auto expectCovered = [&](std::uint64_t frame, std::string_view what) {
    std::optional<svg::RendererBitmap> bitmap = LoadCaptureBitmap(result, frame);
    ASSERT_TRUE(bitmap.has_value());
    EXPECT_GT(contentRatio(paneCrop(*bitmap)), settledRatio - 0.02)
        << what << " lost document coverage (clipped): " << FindCapture(result, frame)->path;
  };

  // The canvas pane must never window-scroll: the wheel spin over the canvas
  // during settle pans the document, and a non-zero ImGui scroll position
  // here means it ALSO scrolled the in-pane overlay chrome (toolbar, perf
  // HUD) away from the canvas.
  if (const repro::GlRnrReplayFrameDiagnostics* settledDiag = FindFrameDiagnostics(result, 20)) {
    EXPECT_EQ(settledDiag->renderPaneScrollY, 0.0f)
        << "render pane window-scrolled its overlay chrome (scrollMaxY="
        << settledDiag->renderPaneScrollMaxY << ")";
  }

  // Mid-pan and end-of-pan: the pane must stay covered by document content
  // even though no fresh bounded raster has landed for the new viewport.
  expectCovered(26, "mid-pan frame");
  expectCovered(33, "post-pan frame");
  // Mid-zoom-out and settled zoom-out: same contract while the scale changes.
  expectCovered(38, "mid-zoom frame");
  expectCovered(44, "post-zoom frame");
}

TEST(GlRnrReplayTest, TextToolLivePointerClickOpensSessionAndTypes) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "text_typing_live";
  const std::optional<std::filesystem::path> replayPath =
      WriteTextTypingLivePointerReplay(outputDir, "text_typing_live.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(16);
  options.maxFrame = 16;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  const repro::GlRnrReplayFrameDiagnostics* finalFrame = FindFrameDiagnostics(result, 16);
  ASSERT_NE(finalFrame, nullptr);
  ASSERT_TRUE(finalFrame->selectedTextContent.has_value())
      << "a live canvas click with the Text tool must open an editing session; "
         "the committed text element must remain selected after Escape";
  EXPECT_EQ(*finalFrame->selectedTextContent, "Hi");

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

// Typing must never blank the text being edited NOR its editing chrome (the
// caret bar and the session's selection outline). Each keystroke rewrites the
// session <text> element's DOM content; until the async render of the new
// content lands, the presenter must keep showing the previous text pixels AND
// keep drawing the caret/selection chrome from the live post-flush DOM. Runs
// with the worker lagging a couple of frames behind (like a real machine
// under fast typing) and counts green text pixels plus cyan chrome pixels in
// the document region on every frame: a frame where either population
// collapses and later recovers is the per-keystroke flicker this reproduces.
//
// Observed failure mode (2026-07-06, Geode presentation): the text pixels
// hold steady, but the cyan chrome collapses to zero for the entire typing
// burst because `RenderCoordinator::rasterizeOverlayForPresentation` resets
// the immediate overlay snapshot whenever the flushed document version is
// ahead of the displayed version with no drag projection - and fast typing
// keeps the document perpetually ahead of the async renderer.
TEST(GlRnrReplayTest, TypingIntoTextKeepsTextPixelsPresentEveryFrame) {
  svg::Renderer probeRenderer;
  if (!probeRenderer.requiresTextureSnapshotPresentation()) {
    GTEST_SKIP() << "Chrome pixels require the Geode direct presentation path.";
  }

  const std::filesystem::path outputDir = DiagnosticOutputDir() / "text_typing_flicker";
  const std::optional<std::filesystem::path> replayPath =
      WriteTextTypingIntoExistingTextReplay(outputDir, "text_typing_flicker.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  constexpr std::uint64_t kBaselineFrame = 17;
  constexpr std::uint64_t kLastFrame = 40;
  for (std::uint64_t frame = kBaselineFrame; frame <= kLastFrame; ++frame) {
    options.captureFrames.insert(frame);
  }
  options.maxFrame = kLastFrame;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.driveDocumentSpaceInput = true;
  // The worker stays busy across several keystroke frames, like a real
  // machine under fast typing: keystrokes flush while the previous
  // keystroke's render is still in flight.
  options.workerRenderDelayMsForTesting = 25;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

  const auto greenPixels = [](const svg::RendererBitmap& bitmap) {
    int count = 0;
    for (std::size_t i = 0; i + 3 < bitmap.pixels.size(); i += 4) {
      const std::uint8_t r = bitmap.pixels[i];
      const std::uint8_t g = bitmap.pixels[i + 1];
      const std::uint8_t b = bitmap.pixels[i + 2];
      if (g > 110 && r < 110 && b < 110) {
        ++count;
      }
    }
    return count;
  };
  // Bright-cyan selection/caret chrome (MakeSelectionStrokePaint 0,200,255).
  // Counted only in the top half of the document-canvas crop: the text (and
  // its chrome) sits at the top of the 200x200 document while the perf HUD's
  // frame graph - which also renders cyan-ish pixels - overlays the bottom.
  const auto cyanChromePixels = [](const svg::RendererBitmap& bitmap) {
    int count = 0;
    const int chromeRows = bitmap.dimensions.y / 2;
    for (int y = 0; y < chromeRows; ++y) {
      const std::uint8_t* row = bitmap.pixels.data() + static_cast<std::size_t>(y) * bitmap.rowBytes;
      for (int x = 0; x < bitmap.dimensions.x; ++x) {
        const std::uint8_t r = row[x * 4];
        const std::uint8_t g = row[x * 4 + 1];
        const std::uint8_t b = row[x * 4 + 2];
        if (b > 180 && g > 140 && r < 110) {
          ++count;
        }
      }
    }
    return count;
  };

  for (std::uint64_t frame = kBaselineFrame; frame <= kLastFrame; ++frame) {
    const repro::GlRnrReplayFrameDiagnostics* diag = FindFrameDiagnostics(result, frame);
    if (diag == nullptr) continue;
    std::optional<svg::RendererBitmap> bmp = LoadCaptureBitmap(result, frame);
    std::cerr << "[diag] f=" << frame << " docV=" << diag->documentFrameVersion
              << " dispV=" << diag->displayedDocVersion
              << " fresh=" << static_cast<int>(diag->canvasFreshness)
              << " pendRasterE=" << static_cast<std::uint64_t>(diag->pendingSelectedLayerRasterizationEntity)
              << " removed=" << diag->lastFlushRemovedElements
              << " invalidated=" << diag->lastFlushCacheInvalidatedElements.size()
              << " green=" << (bmp.has_value() ? greenPixels(*bmp) : -1)
              << " cyanChrome=" << (bmp.has_value() ? cyanChromePixels(*bmp) : -1)
              << " tiles=";
    for (const auto& tile : diag->tiles) {
      std::cerr << tile.id << "(g" << tile.generation << ",grn" << tile.textureGreenPixels << ") ";
    }
    std::cerr << "\n";
  }

  std::optional<svg::RendererBitmap> baseline = LoadCaptureBitmap(result, kBaselineFrame);
  ASSERT_TRUE(baseline.has_value());
  const int baselineGreen = greenPixels(*baseline);
  ASSERT_GT(baselineGreen, 200) << "settled session frame must show the green text";
  const int baselineChrome = cyanChromePixels(*baseline);
  ASSERT_GT(baselineChrome, 100)
      << "settled session frame must show the caret + selection chrome around the text";

  // Typing only appends glyphs, so the settled baseline is a floor for every
  // subsequent frame (with headroom for antialiasing differences). The chrome
  // floor is looser: the caret moves and the selection outline tracks the
  // growing text, but the chrome must never collapse to (near) nothing.
  const int floorGreen = baselineGreen / 2;
  const int floorChrome = baselineChrome / 4;
  for (std::uint64_t frame = kBaselineFrame + 1; frame <= kLastFrame; ++frame) {
    std::optional<svg::RendererBitmap> bitmap = LoadCaptureBitmap(result, frame);
    ASSERT_TRUE(bitmap.has_value());
    const int green = greenPixels(*bitmap);
    if (green < floorGreen) {
      ADD_FAILURE() << "frame " << frame << " dropped the edited text: " << green
                    << " green pixels (settled baseline " << baselineGreen
                    << "): " << FindCapture(result, frame)->path;
    }
    const int chrome = cyanChromePixels(*bitmap);
    if (chrome < floorChrome) {
      ADD_FAILURE() << "frame " << frame << " dropped the caret/selection chrome: " << chrome
                    << " cyan chrome pixels (settled baseline " << baselineChrome
                    << "): " << FindCapture(result, frame)->path;
    }
  }
}

/// Inclusive pixel bounds of the bright-red pixels in @p bitmap, or nullopt
/// when none are present.
struct RedPixelBounds {
  int minX = 0;
  int minY = 0;
  int maxX = 0;
  int maxY = 0;
};
std::optional<RedPixelBounds> BrightRedPixelBounds(const svg::RendererBitmap& source) {
  const svg::RendererBitmap bitmap = NormalizeBitmap(source);
  std::optional<RedPixelBounds> bounds;
  for (int y = 0; y < bitmap.dimensions.y; ++y) {
    for (int x = 0; x < bitmap.dimensions.x; ++x) {
      const std::size_t offset =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(bitmap.dimensions.x) +
           static_cast<std::size_t>(x)) *
          4u;
      const std::uint8_t r = bitmap.pixels[offset];
      const std::uint8_t g = bitmap.pixels[offset + 1];
      const std::uint8_t b = bitmap.pixels[offset + 2];
      if (r < 150 || g > 80 || b > 80) {
        continue;
      }
      if (!bounds.has_value()) {
        bounds = RedPixelBounds{x, y, x, y};
      } else {
        bounds->minX = std::min(bounds->minX, x);
        bounds->minY = std::min(bounds->minY, y);
        bounds->maxX = std::max(bounds->maxX, x);
        bounds->maxY = std::max(bounds->maxY, y);
      }
    }
  }
  return bounds;
}

TEST(GlRnrReplayTest, FirstResizeOfFreshlySelectedGroupKeepsContentLockstepWithOverlay) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "group_first_resize";
  const std::optional<std::filesystem::path> replayPath =
      WriteGroupFirstResizeReplay(outputDir, "group_first_resize.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(5);
  options.captureFrames.insert(13);
  options.captureFrames.insert(23);
  options.captureFrames.insert(40);
  options.maxFrame = 40;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.contentOnlyCapture = true;
  options.driveDocumentSpaceInput = true;
  // The worker lags a couple of frames, like a real machine mid-gesture: the
  // presenter has to bridge with cached tiles + the drag's affine transform.
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::HoldFramesBehind;
  options.holdFramesBehind = 2;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  // Calibrate canvasFromDoc from the settled pre-drag frame: the red rect
  // occupies (40,40)-(100,80) in document space.
  const std::optional<svg::RendererBitmap> baseline = LoadCaptureBitmap(result, 5);
  ASSERT_TRUE(baseline.has_value());
  const std::optional<RedPixelBounds> baselineRed = BrightRedPixelBounds(*baseline);
  ASSERT_TRUE(baselineRed.has_value()) << "red rect not visible in the settled pre-drag frame";
  const double scaleX = (baselineRed->maxX - baselineRed->minX + 1) / 60.0;
  const double scaleY = (baselineRed->maxY - baselineRed->minY + 1) / 40.0;
  const double offsetX = baselineRed->minX - scaleX * 40.0;
  const double offsetY = baselineRed->minY - scaleY * 40.0;

  // The bottom-right corner handle was dragged from (100,120) to (130,150),
  // anchored at the opposite corner (40,40): scale 1.5 in x, 1.375 in y. The
  // red rect (40,40)-(100,80) must therefore present at (40,40)-(130,95) -
  // both mid-drag (bridged from the cached tile) and settled.
  const auto expectRedBoundsNear = [&](std::uint64_t frameIndex, std::string_view label) {
    const std::optional<svg::RendererBitmap> capture = LoadCaptureBitmap(result, frameIndex);
    ASSERT_TRUE(capture.has_value());
    const std::optional<RedPixelBounds> red = BrightRedPixelBounds(*capture);
    ASSERT_TRUE(red.has_value()) << label << ": red rect not visible";
    const double tolerancePx = 6.0;
    EXPECT_NEAR(red->minX, offsetX + scaleX * 40.0, tolerancePx) << label;
    EXPECT_NEAR(red->minY, offsetY + scaleY * 40.0, tolerancePx) << label;
    EXPECT_NEAR(red->maxX, offsetX + scaleX * 130.0, tolerancePx)
        << label << ": presented content is not tracking the resize";
    EXPECT_NEAR(red->maxY, offsetY + scaleY * 95.0, tolerancePx)
        << label << ": presented content is not tracking the resize";
  };
  // The resize must durably reach the DOM + source: the group carries the
  // written transform after release.
  ASSERT_TRUE(result.finalDocumentSource.has_value());
  EXPECT_THAT(*result.finalDocumentSource, testing::HasSubstr("transform="))
      << "resize writeback never reached the document source:\n"
      << *result.finalDocumentSource;

  expectRedBoundsNear(13, "mid-drag frame");
  expectRedBoundsNear(23, "settled post-release frame");
  expectRedBoundsNear(40, "long-settled frame");

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, TextToolClickTypeEscapeCommitsTextElement) {
  const std::filesystem::path outputDir = DiagnosticOutputDir() / "text_typing";
  const std::optional<std::filesystem::path> replayPath =
      WriteTextTypingReplay(outputDir, "text_typing.rnr");
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(12);
  options.maxFrame = 12;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  // The click opens an in-canvas session, the Char events type "Hi", and
  // Escape commits the session leaving the new <text> element selected.
  const repro::GlRnrReplayFrameDiagnostics* finalFrame = FindFrameDiagnostics(result, 12);
  ASSERT_NE(finalFrame, nullptr);
  ASSERT_TRUE(finalFrame->selectedTextContent.has_value())
      << "the committed text element must remain selected after Escape";
  EXPECT_EQ(*finalFrame->selectedTextContent, "Hi");

  std::error_code ec;
  std::filesystem::remove_all(outputDir, ec);
}

TEST(GlRnrReplayTest, PenAnchorHandleDragPresentsLiveGeometryInLockstep) {
  svg::Renderer renderer;
  if (!renderer.requiresTextureSnapshotPresentation()) {
    GTEST_SKIP() << "Presented-pixels lockstep requires the Geode direct presentation path.";
  }

  const std::filesystem::path outputDir = DiagnosticOutputDir() / "pen_drag_lockstep";
  const std::optional<std::filesystem::path> replayPath =
      WritePenCurveDragReplay(outputDir, "pen_drag_lockstep.rnr", /*trailingHoldFrames=*/2);
  ASSERT_TRUE(replayPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames = {7, 9};
  options.maxFrame = 9;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  // Frame 7 is the drag frame: the DOM has flushed the dragged curve while the
  // async raster of that geometry has not landed yet. The presented frame must
  // still show the live curve - the path pixels and the overlay chrome are
  // captured from the same post-flush DOM, never from a stale raster.
  const repro::GlRnrReplayFrameDiagnostics* dragFrame = FindFrameDiagnostics(result, 7);
  ASSERT_NE(dragFrame, nullptr);
  EXPECT_GT(dragFrame->documentFrameVersion, dragFrame->displayedDocVersion)
      << "Test setup: the drag frame must model the real async gap.";
  ASSERT_TRUE(dragFrame->immediateOverlayDocumentVersion.has_value());
  EXPECT_EQ(*dragFrame->immediateOverlayDocumentVersion, dragFrame->documentFrameVersion);

  std::optional<svg::RendererBitmap> dragCapture = LoadCaptureBitmap(result, 7);
  ASSERT_TRUE(dragCapture.has_value());
  EXPECT_GT(CountBrightRedPixelsBelowDocY(*dragCapture, 45.0), 150)
      << "Mid-drag presented pixels must show the live dragged curve (red fill bulge below "
         "y=45), not the stale pre-drag raster. The overlay chrome tracks the live DOM, so a "
         "stale raster underneath reads as the outline detaching from the shape. Capture: "
      << FindCapture(result, 7)->path;

  // Sanity: once the pointer holds still, the crisp composited raster catches
  // up and renders the same bulge without the live preview.
  const repro::GlRnrReplayFrameDiagnostics* settledFrame = FindFrameDiagnostics(result, 9);
  ASSERT_NE(settledFrame, nullptr);
  EXPECT_EQ(settledFrame->documentFrameVersion, settledFrame->displayedDocVersion)
      << "Holding the pointer still must let the async raster catch up to the drag geometry.";
  std::optional<svg::RendererBitmap> settledCapture = LoadCaptureBitmap(result, 9);
  ASSERT_TRUE(settledCapture.has_value());
  EXPECT_GT(CountBrightRedPixelsBelowDocY(*settledCapture, 45.0), 150)
      << "The settled post-drag frame should render the same bulge through the composited "
         "raster. Capture: "
      << FindCapture(result, 9)->path;
}

TEST(GlRnrReplayTest, ColorPickerFillOnPenPathRerendersGeodePromotedLayer) {
  svg::Renderer renderer;
  if (!renderer.requiresTextureSnapshotPresentation()) {
    GTEST_SKIP() << "This regression exercises Geode texture-snapshot presentation.";
  }

  const std::filesystem::path outputDir = DiagnosticOutputDir() / "semantic_pen_fill_after_commit";
  constexpr std::uint64_t kCaptureFrame = 42;
  const std::optional<std::filesystem::path> replayPath =
      WriteSemanticPenFillAfterCommitReplay(outputDir, "semantic_pen_fill_after_commit.rnr");
  ASSERT_TRUE(replayPath.has_value());
  repro::GlRnrReplayOptions options;
  options.rnrPath = *replayPath;
  options.outputDir = outputDir;
  options.captureFrames.insert(kCaptureFrame);
  options.maxFrame = kCaptureFrame;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.driveDocumentSpaceInput = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  std::optional<svg::RendererBitmap> actual = LoadCaptureBitmap(result, kCaptureFrame);
  ASSERT_TRUE(actual.has_value());
  EXPECT_GT(CountPenFillGreenPixelsInDocumentCanvas(*actual), 500)
      << "Changing fill through the paint UI action on a Pen-created, selected path must refresh "
         "the Geode promoted layer texture. A low green-pixel count means the canvas is still "
         "presenting the stale no-fill layer. Capture: "
      << FindCapture(result, kCaptureFrame)->path;
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
      ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

      std::optional<svg::RendererBitmap> capture = LoadCaptureBitmap(result, 1);
      ASSERT_TRUE(capture.has_value());
      const svg::RendererBitmap normalizedCapture = NormalizeBitmap(*capture);
      // Frame 0 observes initial async-renderer warm-up and can report a transient stale canvas on
      // slower software GL hosts. The invariant for this matrix is the explicit capture frame.
      const std::string captureFrameDiagnostics = CanonicalReplayDiagnostics(result, 1u, 1u);
      const std::string label = std::string("gl_replay_matrix_") + (pace ? "paced" : "unpaced") +
                                "_delay_" + std::to_string(delayMs);

      if (!baselineCapture.has_value()) {
        baselineCapture = normalizedCapture;
        baselineDiagnostics = captureFrameDiagnostics;
        continue;
      }

      tests::CompareBitmapToBitmap(normalizedCapture, *baselineCapture, label,
                                   tests::PixelmatchIdentityParams());
      EXPECT_EQ(captureFrameDiagnostics, *baselineDiagnostics) << label;
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
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

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
  options.maxFrame = 0;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::DrainEachFrame;
  options.contentOnlyCapture = true;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);
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
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

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
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

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
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

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

  for (std::uint64_t frame = 39; frame <= kLastCaptureFrame; ++frame) {
    const repro::GlRnrReplayFrameDiagnostics* diagnostics = FindFrameDiagnostics(result, frame);
    ASSERT_NE(diagnostics, nullptr) << "missing diagnostics for replay frame " << frame;
    if (!diagnostics->frameCost.overlay.hasLiveDragPreview) {
      continue;
    }
    EXPECT_EQ(diagnostics->frameCost.documentCanvasCommitCount, 0u)
        << "Zoom-driven canvas commits during active drag force full cached-span rerasterization "
           "on replay frame "
        << frame;
    EXPECT_EQ(diagnostics->frameCost.compositedRender.cachedTileCount, 0)
        << "Active zoom+drag should keep using presenter-transformed cache instead of "
           "rerasterizing cached compositor tiles on replay frame "
        << frame;
    EXPECT_DOUBLE_EQ(diagnostics->frameCost.compositedRender.cachedMs, 0.0)
        << "Unexpected cached compositor raster cost on active zoom+drag replay frame " << frame;
  }
}

TEST(GlRnrReplayTest, GeodeDragZoomRerasterizesDonnerDOverlayEveryPresentedFrame) {
  constexpr std::uint64_t kFirstZoomFrame = 31;
  constexpr std::uint64_t kLastZoomFrame = 40;

  const std::filesystem::path outputDir = DiagnosticOutputDir() / "gl_geode_drag_zoom_d_repro";
  const std::optional<std::filesystem::path> rnrPath =
      WriteDonnerDDragZoomReplay(outputDir, "donner_d_drag_zoom_overlay_repro.rnr");
  ASSERT_TRUE(rnrPath.has_value());
  std::optional<repro::ReproFile> reproFile = repro::ReadReproFile(*rnrPath);
  ASSERT_TRUE(reproFile.has_value());
  ASSERT_TRUE(reproFile->metadata.expect.has_value());
  const repro::ReproExpectation& expect = *reproFile->metadata.expect;
  ASSERT_EQ(expect.proofKind, repro::ReproExpectationProofKind::Selection);
  ASSERT_EQ(expect.cropMode, "document-canvas");
  ASSERT_EQ(expect.targetSelector, "#Donner_D");

  repro::GlRnrReplayOptions options;
  options.rnrPath = *rnrPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = outputDir;
  options.captureFrames = {kLastZoomFrame};
  options.maxFrame = kLastZoomFrame;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::Realtime;
  options.workerRenderDelayMsForTesting = 2;
  options.driveDocumentSpaceInput = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);
  ASSERT_EQ(result.finalSelectedElementLabel, expect.expectedSelectionLabel);

  for (std::uint64_t frame = kFirstZoomFrame; frame <= kLastZoomFrame; ++frame) {
    const repro::GlRnrReplayFrameDiagnostics* diagnostics = FindFrameDiagnostics(result, frame);
    ASSERT_NE(diagnostics, nullptr) << "missing diagnostics for replay frame " << frame;
    EXPECT_EQ(diagnostics->frameCost.overlay.selectedElementCount, 1)
        << "Selection overlay was not rebuilt for presented zoom frame " << frame;
    EXPECT_EQ(diagnostics->frameCost.overlay.pathCount, 1)
        << "Selection path overlay was not rebuilt for presented zoom frame " << frame;
    EXPECT_EQ(diagnostics->frameCost.overlay.handleCount, 4)
        << "Selection transform handles were not rebuilt for presented zoom frame " << frame;
    EXPECT_GT(diagnostics->frameCost.overlay.payloadBytes, 0u)
        << "Overlay payload did not refresh for presented zoom frame " << frame;
  }
}

TEST(GlRnrReplayTest, GeodeZoomThenDragKeepsDonnerDOverlayLockedToPresentedContent) {
  const std::filesystem::path outputDir =
      DiagnosticOutputDir() / "gl_geode_zoom_then_drag_d_lockstep";
  const std::optional<std::filesystem::path> rnrPath =
      WriteDonnerDZoomThenDragReplay(outputDir, "donner_d_zoom_then_drag_overlay_repro.rnr");
  ASSERT_TRUE(rnrPath.has_value());
  std::optional<repro::ReproFile> reproFile = repro::ReadReproFile(*rnrPath);
  ASSERT_TRUE(reproFile.has_value());
  ASSERT_TRUE(reproFile->metadata.expect.has_value());
  const repro::ReproExpectation& expect = *reproFile->metadata.expect;
  ASSERT_EQ(expect.proofKind, repro::ReproExpectationProofKind::Selection);
  ASSERT_EQ(expect.cropMode, "document-canvas");
  ASSERT_EQ(expect.targetSelector, "#Donner_D");

  repro::GlRnrReplayOptions options;
  options.rnrPath = *rnrPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = outputDir;
  options.captureFrames = {37, 38, 39, 40};
  options.maxFrame = 43;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::Realtime;
  options.workerRenderDelayMsForTesting = 40;
  options.driveDocumentSpaceInput = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);
  ASSERT_EQ(result.finalSelectedElementLabel, expect.expectedSelectionLabel);

  int checkedDragFrames = 0;
  for (const std::uint64_t frame : {37u, 38u, 39u, 40u}) {
    const repro::GlRnrReplayFrameDiagnostics* diagnostics = FindFrameDiagnostics(result, frame);
    ASSERT_NE(diagnostics, nullptr) << "missing diagnostics for replay frame " << frame;
    if (!diagnostics->frameCost.overlay.hasLiveDragPreview ||
        !(diagnostics->frameCost.overlay.liveDragTranslationDoc.x > 0.0)) {
      continue;
    }
    ++checkedDragFrames;
    ASSERT_TRUE(diagnostics->frameCost.overlay.hasRepresentedDragPreview)
        << "Overlay presentation must record the drag transform it actually used.";

    const Vector2d presentedContentTranslation = PresentedDragTargetTranslationOrZero(*diagnostics);
    EXPECT_NEAR(diagnostics->frameCost.overlay.representedDragTranslationDoc.x,
                presentedContentTranslation.x, 1e-6)
        << "Overlay drag presentation must stay lockstep with the content tile presented in frame "
        << frame;
    EXPECT_NEAR(diagnostics->frameCost.overlay.representedDragTranslationDoc.y,
                presentedContentTranslation.y, 1e-6)
        << "Overlay drag presentation must stay lockstep with the content tile presented in frame "
        << frame;
  }
  EXPECT_GT(checkedDragFrames, 0) << "Repro did not enter the second Donner D drag window.";
}

TEST(GlRnrReplayTest, GeodeZoomThenDragDoesNotFreezeLiveDragPreviewWhileWorkerBusy) {
  const std::filesystem::path outputDir =
      DiagnosticOutputDir() / "gl_geode_zoom_then_drag_d_busy_live_preview";
  const std::optional<std::filesystem::path> rnrPath =
      WriteDonnerDZoomThenDragReplay(outputDir, "donner_d_zoom_then_drag_busy_live_preview.rnr");
  ASSERT_TRUE(rnrPath.has_value());

  repro::GlRnrReplayOptions options;
  options.rnrPath = *rnrPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = outputDir;
  options.captureFrames = {42};
  options.maxFrame = 42;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::Realtime;
  options.workerRenderDelayMsForTesting = 500;
  options.driveDocumentSpaceInput = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  std::optional<Vector2d> previousTranslation;
  for (std::uint64_t frame = 37; frame <= 42; ++frame) {
    const repro::GlRnrReplayFrameDiagnostics* diagnostics = FindFrameDiagnostics(result, frame);
    ASSERT_NE(diagnostics, nullptr) << "missing diagnostics for replay frame " << frame;
    ASSERT_TRUE(diagnostics->frameCost.overlay.hasLiveDragPreview)
        << "Drag preview disappeared on replay frame " << frame;

    const Vector2d translation = diagnostics->frameCost.overlay.liveDragTranslationDoc;
    EXPECT_GT(translation.x, 0.0) << "Live drag preview did not move on replay frame " << frame;
    EXPECT_LT(translation.y, 0.0) << "Live drag preview did not move on replay frame " << frame;
    if (previousTranslation.has_value()) {
      EXPECT_GT(translation.x, previousTranslation->x + 2.0)
          << "Live drag preview froze instead of following the mouse on replay frame " << frame;
      EXPECT_LT(translation.y, previousTranslation->y - 0.4)
          << "Live drag preview froze instead of following the mouse on replay frame " << frame;
    }
    previousTranslation = translation;
  }
}

TEST(GlRnrReplayTest, GeodeFarZoomThenDragKeepsDonnerNOverlayLockedToPresentedContent) {
  const std::filesystem::path outputDir =
      DiagnosticOutputDir() / "gl_geode_far_zoom_then_drag_n_lockstep";
  const std::optional<std::filesystem::path> rnrPath =
      WriteDonnerNFarZoomThenDragReplay(outputDir, "donner_n_far_zoom_then_drag_overlay_repro.rnr");
  ASSERT_TRUE(rnrPath.has_value());
  std::optional<repro::ReproFile> reproFile = repro::ReadReproFile(*rnrPath);
  ASSERT_TRUE(reproFile.has_value());
  ASSERT_TRUE(reproFile->metadata.expect.has_value());
  const repro::ReproExpectation& expect = *reproFile->metadata.expect;
  ASSERT_EQ(expect.proofKind, repro::ReproExpectationProofKind::Selection);
  ASSERT_EQ(expect.cropMode, "document-canvas");
  ASSERT_EQ(expect.targetSelector, "#Donner_N_2");

  repro::GlRnrReplayOptions options;
  options.rnrPath = *rnrPath;
  options.svgPathOverride = RunfilePath("donner_splash.svg");
  options.outputDir = outputDir;
  options.captureFrames = {54, 55, 56, 57, 58, 59, 60};
  options.maxFrame = 61;
  options.cropMode = repro::GlRnrReplayCropMode::DocumentCanvas;
  options.pace = false;
  options.workerScheduling = repro::GlRnrReplayWorkerScheduling::Realtime;
  options.workerRenderDelayMsForTesting = 500;
  options.driveDocumentSpaceInput = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);
  ASSERT_EQ(result.finalSelectedElementLabel, expect.expectedSelectionLabel);

  const std::string dragDiagnostics = CanonicalReplayDiagnostics(result, 50u, 61u);
  int checkedDragFrames = 0;
  for (std::uint64_t frame = 54; frame <= 60; ++frame) {
    const repro::GlRnrReplayFrameDiagnostics* diagnostics = FindFrameDiagnostics(result, frame);
    ASSERT_NE(diagnostics, nullptr) << "missing diagnostics for replay frame " << frame;
    ASSERT_TRUE(diagnostics->frameCost.overlay.hasLiveDragPreview)
        << "Drag preview disappeared on replay frame " << frame;
    if (!(diagnostics->frameCost.overlay.liveDragTranslationDoc.x > 0.0)) {
      continue;
    }
    ++checkedDragFrames;
    ASSERT_TRUE(diagnostics->frameCost.overlay.hasRepresentedDragPreview)
        << "Overlay presentation must record the drag transform it actually used.";

    const Vector2d presentedContentTranslation = PresentedDragTargetTranslationOrZero(*diagnostics);
    ASSERT_NEAR(diagnostics->frameCost.overlay.representedDragTranslationDoc.x,
                diagnostics->frameCost.overlay.liveDragTranslationDoc.x, 1e-6)
        << "Drag chrome froze instead of following the live Donner N drag in frame " << frame
        << "\n"
        << dragDiagnostics;
    ASSERT_NEAR(diagnostics->frameCost.overlay.representedDragTranslationDoc.y,
                diagnostics->frameCost.overlay.liveDragTranslationDoc.y, 1e-6)
        << "Drag chrome froze instead of following the live Donner N drag in frame " << frame
        << "\n"
        << dragDiagnostics;
    ASSERT_NEAR(presentedContentTranslation.x,
                diagnostics->frameCost.overlay.liveDragTranslationDoc.x, 1e-6)
        << "Presented Donner N pixels froze instead of following the live drag in frame " << frame
        << "\n"
        << dragDiagnostics;
    ASSERT_NEAR(presentedContentTranslation.y,
                diagnostics->frameCost.overlay.liveDragTranslationDoc.y, 1e-6)
        << "Presented Donner N pixels froze instead of following the live drag in frame " << frame
        << "\n"
        << dragDiagnostics;
    ASSERT_NEAR(diagnostics->frameCost.overlay.representedDragTranslationDoc.x,
                presentedContentTranslation.x, 1e-6)
        << "Path overlay must stay lockstep with the content tile presented in frame " << frame
        << "\n"
        << dragDiagnostics;
    ASSERT_NEAR(diagnostics->frameCost.overlay.representedDragTranslationDoc.y,
                presentedContentTranslation.y, 1e-6)
        << "Path overlay must stay lockstep with the content tile presented in frame " << frame
        << "\n"
        << dragDiagnostics;
  }
  EXPECT_GT(checkedDragFrames, 0) << "Repro did not enter the Donner N drag window.";
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
  options.driveDocumentSpaceInput = true;
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_GL_REPLAY_OR_SKIP(options, result, error);

  std::optional<svg::RendererBitmap> beforeRClick = LoadCaptureBitmap(result, beforeClickFrame);
  std::optional<svg::RendererBitmap> firstRClickFrame = LoadCaptureBitmap(result, firstClickFrame);
  std::optional<svg::RendererBitmap> settledRClickFrame =
      LoadCaptureBitmap(result, settledClickFrame);
  ASSERT_TRUE(beforeRClick.has_value());
  ASSERT_TRUE(firstRClickFrame.has_value());
  ASSERT_TRUE(settledRClickFrame.has_value());

  const PixelCrop broadCrop{
      .x = expect.cropRect->x,
      .y = expect.cropRect->y,
      .width = expect.cropRect->width,
      .height = expect.cropRect->height,
  };
  // Keep the identity compare focused on the dragged target. The broader repro crop also contains
  // neighboring Donner letters and lightning highlights; those pixels are useful for centroid
  // context below, but they are not the behavior this regression is pinning.
  const PixelCrop selectedOCrop{
      .x = broadCrop.x + 118,
      .y = broadCrop.y + 45,
      .width = 140,
      .height = 180,
  };
  const svg::RendererBitmap firstRClickTarget = CropBitmap(*firstRClickFrame, selectedOCrop);
  const svg::RendererBitmap settledTarget = CropBitmap(*settledRClickFrame, selectedOCrop);
  const bool hasArchivedTargetCrop = !firstRClickTarget.empty() && !settledTarget.empty();

  tests::CompareBitmapToBitmap(hasArchivedTargetCrop ? firstRClickTarget : *firstRClickFrame,
                               hasArchivedTargetCrop ? settledTarget : *settledRClickFrame,
                               "gl_o_then_r_frame_153_o_target_vs_frame_155",
                               tests::PixelmatchIdentityParams());

  svg::RendererBitmap beforeCentroidTarget = CropBitmap(*beforeRClick, broadCrop);
  svg::RendererBitmap firstCentroidTarget = CropBitmap(*firstRClickFrame, broadCrop);
  svg::RendererBitmap settledCentroidTarget = CropBitmap(*settledRClickFrame, broadCrop);
  if (beforeCentroidTarget.empty() || firstCentroidTarget.empty() ||
      settledCentroidTarget.empty()) {
    beforeCentroidTarget = *beforeRClick;
    firstCentroidTarget = *firstRClickFrame;
    settledCentroidTarget = *settledRClickFrame;
  }

  const std::optional<double> beforeCentroidY = YellowCentroidY(beforeCentroidTarget);
  const std::optional<double> firstCentroidY = YellowCentroidY(firstCentroidTarget);
  const std::optional<double> settledCentroidY = YellowCentroidY(settledCentroidTarget);
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
