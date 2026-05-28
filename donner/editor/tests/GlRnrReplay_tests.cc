/// @file

#include "donner/editor/repro/GlRnrReplay.h"

#include <algorithm>
#include <array>
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
                            int mouseButtonMask, std::vector<repro::ReproEvent> events = {}) {
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
  file.frames.push_back(std::move(frame));
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
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;
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
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;
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
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;

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
  options.visible = false;

  repro::GlRnrReplayResult result;
  std::string error;
  ASSERT_TRUE(repro::RunGlRnrReplay(options, &result, &error)) << error;
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

  const PixelCrop broadCrop{
      .x = expect.cropRect->x,
      .y = expect.cropRect->y,
      .width = expect.cropRect->width,
      .height = expect.cropRect->height,
  };
  // Keep the identity compare focused on the dragged target. The recorded repro crop also contains
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

  tests::CompareBitmapToBitmap(firstRClickTarget, settledTarget,
                               "gl_o_then_r_frame_153_o_target_vs_frame_155",
                               tests::PixelmatchIdentityParams());

  const std::optional<double> beforeCentroidY =
      YellowCentroidY(CropBitmap(*beforeRClick, broadCrop));
  const std::optional<double> firstCentroidY =
      YellowCentroidY(CropBitmap(*firstRClickFrame, broadCrop));
  const std::optional<double> settledCentroidY =
      YellowCentroidY(CropBitmap(*settledRClickFrame, broadCrop));
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
