#define IMGUI_DEFINE_MATH_OPERATORS
// ImGui internals must load before EditorShell.h brings in imgui.h.
// clang-format off
#include "donner/editor/ImGuiInternalIncludes.h"
// clang-format on

#include "donner/editor/EditorShell.h"

#include <GLFW/glfw3.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "donner/css/Color.h"
#include "donner/editor/EditorCommand.h"
#include "donner/editor/EditorDockLayout.h"
#include "donner/editor/EditorSampleCatalog.h"
#include "donner/editor/EditorShellInternal.h"
#include "donner/editor/EditorShellPresentation.h"
#include "donner/editor/FillStrokeWidget.h"
#include "donner/editor/ImGuiInternalIncludes.h"
#include "donner/editor/InMemoryClipboard.h"
#include "donner/editor/PresentedFrameComposer.h"
#include "donner/editor/gui/EditorWindow.h"
#include "donner/editor/repro/ReplayResourceBudget.h"
#include "donner/editor/repro/ReproFile.h"
#include "donner/editor/tests/AsyncTestPolling.h"
#include "donner/editor/tests/BitmapGoldenCompare.h"
#include "donner/editor/tests/CompositorDebugPanelTestAccess.h"
#include "donner/editor/tests/RenderCoordinatorTestAccess.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/SVGRectElement.h"
#include "donner/svg/renderer/Renderer.h"
#include "donner/svg/renderer/RendererImageIO.h"
#ifdef DONNER_EDITOR_WGPU
#include "donner/svg/renderer/RendererGeode.h"
#include "donner/svg/renderer/geode/GeodeDevice.h"
#endif
#include "donner/svg/properties/PropertyRegistry.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::editor {
namespace {

constexpr std::string_view kInitialSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <rect id="background" width="120" height="80" fill="#ffffff"/>
  <rect id="target" x="10" y="12" width="40" height="24" fill="#3366cc"/>
  <text id="label" x="12" y="60">Donner</text>
</svg>
)svg";

svg::RendererBitmap CropDocumentRect(const svg::RendererBitmap& bitmap,
                                     const ViewportState& viewport, double framebufferScale,
                                     const Box2d& documentRect) {
  svg::RendererBitmap crop;
  if (bitmap.empty() || !std::isfinite(framebufferScale) || framebufferScale <= 0.0) {
    return crop;
  }
  const Vector2d screenMin = viewport.documentToScreen(documentRect.topLeft);
  const Vector2d screenMax = viewport.documentToScreen(documentRect.bottomRight);
  // documentToScreen() uses logical UI pixels. Scale them by the framebuffer scale the caller
  // passes rather than GLFW windowSize() or the viewport's devicePixelRatio, which a native window
  // emulating another scale need not share.
  const Vector2d physicalMin = screenMin * framebufferScale;
  const Vector2d physicalMax = screenMax * framebufferScale;
  if (!std::isfinite(physicalMin.x) || !std::isfinite(physicalMin.y) ||
      !std::isfinite(physicalMax.x) || !std::isfinite(physicalMax.y)) {
    return crop;
  }
  const auto clippedPixel = [](double position, int limit) {
    return static_cast<int>(std::lround(std::clamp(position, 0.0, static_cast<double>(limit))));
  };
  const int x = clippedPixel(physicalMin.x, bitmap.dimensions.x);
  const int y = clippedPixel(physicalMin.y, bitmap.dimensions.y);
  const int right = clippedPixel(physicalMax.x, bitmap.dimensions.x);
  const int bottom = clippedPixel(physicalMax.y, bitmap.dimensions.y);
  if (right <= x || bottom <= y) {
    return crop;
  }
  crop.dimensions = Vector2i(right - x, bottom - y);
  crop.rowBytes = static_cast<std::size_t>(right - x) * 4u;
  crop.alphaType = bitmap.alphaType;
  crop.pixels.resize(crop.rowBytes * static_cast<std::size_t>(bottom - y));
  for (int row = 0; row < bottom - y; ++row) {
    std::memcpy(crop.pixels.data() + static_cast<std::size_t>(row) * crop.rowBytes,
                bitmap.pixels.data() + static_cast<std::size_t>(y + row) * bitmap.rowBytes +
                    static_cast<std::size_t>(x) * 4u,
                crop.rowBytes);
  }
  return crop;
}

TEST(EditorShellTest, DocumentCropUsesEffectiveFramebufferScaleAndClipsVisibleEdges) {
  // The forced 2x offscreen scale may coexist with either a 1x or 2x OS-reported viewport ratio.
  const std::array<std::pair<int, int>, 3> cases = {{{1, 1}, {2, 1}, {2, 2}}};
  for (const auto& [scale, reportedDpr] : cases) {
    svg::RendererBitmap framebuffer;
    framebuffer.dimensions = Vector2i(100 * scale, 80 * scale);
    framebuffer.rowBytes = static_cast<std::size_t>(framebuffer.dimensions.x) * 4u;
    framebuffer.pixels.resize(framebuffer.rowBytes * framebuffer.dimensions.y);
    for (int y = 0; y < framebuffer.dimensions.y; ++y) {
      for (int x = 0; x < framebuffer.dimensions.x; ++x) {
        const std::size_t offset = static_cast<std::size_t>(y) * framebuffer.rowBytes + x * 4u;
        framebuffer.pixels[offset] = static_cast<std::uint8_t>(x);
        framebuffer.pixels[offset + 1] = static_cast<std::uint8_t>(y);
        framebuffer.pixels[offset + 3] = 255;
      }
    }
    ViewportState viewport;
    viewport.zoom = 1.0;
    viewport.devicePixelRatio = static_cast<double>(reportedDpr);
    viewport.panScreenPoint = Vector2d(10.0, 20.0);
    const svg::RendererBitmap crop =
        CropDocumentRect(framebuffer, viewport, scale, Box2d::FromXYWH(4.0, 5.0, 3.0, 2.0));
    ASSERT_EQ(crop.dimensions, Vector2i(3 * scale, 2 * scale));
    ASSERT_FALSE(crop.empty());
    EXPECT_EQ(crop.pixels[0], 14 * scale);
    EXPECT_EQ(crop.pixels[1], 25 * scale);
    const std::size_t last = crop.pixels.size() - 4u;
    EXPECT_EQ(crop.pixels[last], 17 * scale - 1);
    EXPECT_EQ(crop.pixels[last + 1], 27 * scale - 1);

    const svg::RendererBitmap visibleBottom =
        CropDocumentRect(framebuffer, viewport, scale, Box2d::FromXYWH(4.0, 55.0, 3.0, 10.0));
    ASSERT_EQ(visibleBottom.dimensions, Vector2i(3 * scale, 5 * scale));
    EXPECT_EQ(visibleBottom.pixels[1], 75 * scale);
    EXPECT_EQ(visibleBottom.pixels[visibleBottom.pixels.size() - 3u], 80 * scale - 1);
    EXPECT_TRUE(CropDocumentRect(framebuffer, viewport, scale, Box2d::FromXYWH(4.0, 61.0, 3.0, 2.0))
                    .empty());
  }
}

constexpr std::string_view kStyledSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <style>
    .hit { fill: red; stroke: blue; }
  </style>
  <rect id="target" class="hit" x="10" y="12" width="40" height="24"/>
  <circle id="other" cx="80" cy="40" r="10" fill="url(#paint)"/>
  <linearGradient id="paint">
    <stop offset="0" stop-color="red"/>
  </linearGradient>
</svg>
)svg";

constexpr std::string_view kPaintSnapshotSelectionChangeSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <rect id="first" x="10" y="12" width="40" height="24" fill="#ff0000"/>
  <rect id="second" x="65" y="12" width="40" height="24" fill="#0000ff"/>
</svg>
)svg";

constexpr std::string_view kReferencedSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <defs>
    <linearGradient id="paint">
      <stop offset="0" stop-color="red"/>
    </linearGradient>
    <clipPath id="clip">
      <rect width="100" height="60"/>
    </clipPath>
  </defs>
  <rect id="target" x="10" y="12" width="40" height="24" fill="url(#paint)" clip-path="url(#clip)"/>
  <circle id="referrer" cx="80" cy="40" r="10" fill="url(#paint)"/>
</svg>
)svg";

constexpr std::string_view kPaintToolbarSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <defs>
    <linearGradient id="paint">
      <stop offset="0" stop-color="red"/>
    </linearGradient>
  </defs>
  <rect id="local" x="10" y="12" width="20" height="20" fill="url(#paint)" stroke="url(#paint)"/>
  <rect id="missing" x="40" y="12" width="20" height="20" fill="url(#missing)"/>
  <rect id="contextual" x="70" y="12" width="20" height="20" fill="context-fill"/>
  <rect id="styled-none-attribute" x="70" y="42" width="20" height="20"
        fill="context-stroke" style="fill: none"/>
  <rect id="external" x="10" y="42" width="20" height="20"
        fill="url(https://example.invalid/paint.svg#paint)"/>
</svg>
)svg";

constexpr std::string_view kIntrinsicSizeSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="160" height="90">
  <rect id="target" width="160" height="90"/>
</svg>
)svg";

/// Content scale every editor shell test window emulates unless a test asks for another.
///
/// The suite runs at 2x on purpose, on every platform. Without a forced scale a hidden macOS window
/// takes the main display's scale while Linux's windowless platform is always 1x, so whether HiDPI
/// was tested depended on the host's display. `EditorShell1xSmokeTest` keeps a small 1x
/// cross-section so regressions at 1x are still caught.
constexpr double kTestContentScale = 2.0;

/// Return @p options for a hidden test window that emulates @p contentScale regardless of the host
/// display.
gui::EditorWindowOptions TestWindowOptions(gui::EditorWindowOptions options,
                                           double contentScale = kTestContentScale) {
  options.visible = false;
  options.offscreen = true;
  options.offscreenContentScale = contentScale;
  return options;
}

/// Assert that @p window, created for a @p logicalSize UI, renders at @p contentScale, so a silent
/// fallback to the host display's scale fails the test instead of quietly changing what it covers.
void ExpectTestWindowScale(const gui::EditorWindow& window, const Vector2i& logicalSize,
                           double contentScale) {
  EXPECT_DOUBLE_EQ(window.displayScale(), contentScale);
  EXPECT_DOUBLE_EQ(window.contentScale().x, contentScale);
  EXPECT_EQ(window.framebufferSize(),
            Vector2i(static_cast<int>(std::lround(logicalSize.x * contentScale)),
                     static_cast<int>(std::lround(logicalSize.y * contentScale))));
}

gui::EditorWindow MakeHiddenWindow(double contentScale = kTestContentScale) {
  return gui::EditorWindow(TestWindowOptions(
      {
          .title = "Donner EditorShell test",
          .initialWidth = 640,
          .initialHeight = 480,
      },
      contentScale));
}

std::filesystem::path TempPathForTest(std::string_view suffix) {
  const testing::TestInfo* testInfo = testing::UnitTest::GetInstance()->current_test_info();
  const std::string testName = testInfo != nullptr ? testInfo->name() : "unknown";
  const char* testTmpDir = std::getenv("TEST_TMPDIR");
  const std::filesystem::path writableTempDirectory = testTmpDir != nullptr && testTmpDir[0] != '\0'
                                                          ? std::filesystem::path(testTmpDir)
                                                          : std::filesystem::temp_directory_path();
  return writableTempDirectory / ("donner_editor_shell_" + testName + "_" + std::string(suffix));
}

void WriteTextFile(const std::filesystem::path& path, std::string_view text) {
  std::ofstream output(path);
  ASSERT_TRUE(output.good()) << "Could not open " << path;
  output << text;
}

std::string ReadTextFile(const std::filesystem::path& path) {
  std::ifstream input(path);
  std::string result((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  return result;
}

EditorShellOptions OptionsWithSource(std::string_view source,
                                     std::string_view path = "memory.svg") {
  EditorShellOptions options;
  options.initialSource = std::string(source);
  options.initialPath = std::string(path);
  return options;
}

}  // namespace

TEST(EditorShellInternalTest, MapsPresentationResourcesToTelemetrySamples) {
  PresentationResourceStats resources{
      .activeTileBytes = 22,
      .overviewTileBytes = 33,
      .pendingRetiredBytes = 44,
      .agedRetiredBytes = 55,
      .totalTrackedBytes = 66,
      .peakTrackedBytes = 77,
      .wgpuLifetimeTextureCreates = 88,
      .wgpuLifetimeBufferCreates = 99,
  };

  const FrameMemorySample memory = internal::MemorySampleFromPresentationResources(resources);
  EXPECT_EQ(memory.activeTileBytes, 22u);
  EXPECT_EQ(memory.overviewTileBytes, 33u);
  EXPECT_EQ(memory.retiredBytes, 99u);
  EXPECT_EQ(memory.totalTrackedBytes, 66u);
  EXPECT_EQ(memory.peakTrackedBytes, 77u);
  EXPECT_EQ(memory.wgpuLifetimeTextureCreates, 88u);
  EXPECT_EQ(memory.wgpuLifetimeBufferCreates, 99u);

  const FrameMissResourceTelemetry telemetry =
      internal::FrameMissTelemetryFromPresentationResources(resources);
  EXPECT_EQ(telemetry.activeTileBytes, memory.activeTileBytes);
  EXPECT_EQ(telemetry.overviewTileBytes, memory.overviewTileBytes);
  EXPECT_EQ(telemetry.retiredBytes, memory.retiredBytes);
  EXPECT_EQ(telemetry.totalTrackedBytes, memory.totalTrackedBytes);
  EXPECT_EQ(telemetry.peakTrackedBytes, memory.peakTrackedBytes);
  EXPECT_EQ(telemetry.wgpuLifetimeTextureCreates, memory.wgpuLifetimeTextureCreates);
  EXPECT_EQ(telemetry.wgpuLifetimeBufferCreates, memory.wgpuLifetimeBufferCreates);
}

TEST(EditorShellPresentationTest, PresentationUsesFramebufferScaleInsteadOfRasterDpr) {
  ViewportState viewport;
  viewport.devicePixelRatio = 1.0;
  viewport.zoom = 1.0;
  viewport.panScreenPoint = Vector2d(320.0, 240.0);
  viewport.panDocPoint = Vector2d(100.0, 60.0);

  const Transform2d framebufferFromDocument =
      PresentedFramebufferFromDocumentTransform(viewport, Vector2d(2.0, 2.0));
  EXPECT_EQ(framebufferFromDocument.transformPosition(Vector2d(0.0, 0.0)), Vector2d(440.0, 360.0));
  EXPECT_EQ(framebufferFromDocument.transformPosition(Vector2d(200.0, 120.0)),
            Vector2d(840.0, 600.0));
}

TEST(EditorShellInternalTest, CursorForTransformHandleIntentMapsResizeAndRotateHandles) {
  EXPECT_EQ(internal::CursorForTransformHandleIntent(
                SelectionTransformHandleIntent{.kind = SelectionTransformHandleKind::None}),
            ImGuiMouseCursor_Arrow);
  EXPECT_EQ(internal::CursorForTransformHandleIntent(
                SelectionTransformHandleIntent{.kind = SelectionTransformHandleKind::Rotate}),
            ImGuiMouseCursor_ResizeAll);
  EXPECT_EQ(internal::CursorForTransformHandleIntent(SelectionTransformHandleIntent{
                .kind = SelectionTransformHandleKind::Resize,
                .corner = SelectionTransformCorner::TopLeft,
            }),
            ImGuiMouseCursor_ResizeNWSE);
  EXPECT_EQ(internal::CursorForTransformHandleIntent(SelectionTransformHandleIntent{
                .kind = SelectionTransformHandleKind::Resize,
                .corner = SelectionTransformCorner::BottomRight,
            }),
            ImGuiMouseCursor_ResizeNWSE);
  EXPECT_EQ(internal::CursorForTransformHandleIntent(SelectionTransformHandleIntent{
                .kind = SelectionTransformHandleKind::Resize,
                .corner = SelectionTransformCorner::TopRight,
            }),
            ImGuiMouseCursor_ResizeNESW);
  EXPECT_EQ(internal::CursorForTransformHandleIntent(SelectionTransformHandleIntent{
                .kind = SelectionTransformHandleKind::Resize,
                .corner = SelectionTransformCorner::BottomLeft,
            }),
            ImGuiMouseCursor_ResizeNESW);
}

// The hint the shell turns each pen hover intent into. `DragAnchor` deliberately
// has no nib of its own - the shell swaps in the anchor-point cursor for it, so
// this returns the plain nib as an inert default.
TEST(EditorShellInternalTest, PenCursorHintForIntentMapsEachHoverIntentToItsNib) {
  EXPECT_EQ(internal::PenCursorHintForIntent(PenHoverIntent::PlaceAnchor), PenCursorHint::Base);
  EXPECT_EQ(internal::PenCursorHintForIntent(PenHoverIntent::ClosePath), PenCursorHint::Close);
  EXPECT_EQ(internal::PenCursorHintForIntent(PenHoverIntent::InsertAnchor), PenCursorHint::Add);
  EXPECT_EQ(internal::PenCursorHintForIntent(PenHoverIntent::DragAnchor), PenCursorHint::Base);
}

TEST(EditorShellInternalTest, TextToolHintShowsOnlyWhileIdle) {
  // Idle text tool: the double-click / drag affordances are invisible, so
  // the hint must teach them.
  const std::string_view idleHint =
      internal::TextToolHintLabel(/*isEditing=*/false, /*isDraggingBox=*/false);
  EXPECT_NE(idleHint.find("Double-click"), std::string_view::npos) << idleHint;
  EXPECT_NE(idleHint.find("Drag"), std::string_view::npos) << idleHint;

  // While a session or a box drag is active the hint would be noise.
  EXPECT_TRUE(internal::TextToolHintLabel(/*isEditing=*/true, /*isDraggingBox=*/false).empty());
  EXPECT_TRUE(internal::TextToolHintLabel(/*isEditing=*/false, /*isDraggingBox=*/true).empty());
  EXPECT_TRUE(internal::TextToolHintLabel(/*isEditing=*/true, /*isDraggingBox=*/true).empty());
}

TEST(EditorShellInternalTest, ActiveAttributePaintSlotHandlesNoneColorAndCustomValues) {
  const internal::ToolbarPaintSlotState none =
      internal::ToolbarPaintSlotStateForActiveAttribute("none");
  EXPECT_TRUE(none.isNone);
  EXPECT_FALSE(none.isCustom);

  const internal::ToolbarPaintSlotState color =
      internal::ToolbarPaintSlotStateForActiveAttribute("#123456");
  EXPECT_FALSE(color.isNone);
  EXPECT_FALSE(color.isCustom);
  EXPECT_EQ(color.color, css::RGBA::RGB(0x12, 0x34, 0x56));

  const internal::ToolbarPaintSlotState custom =
      internal::ToolbarPaintSlotStateForActiveAttribute("url(#paint)");
  EXPECT_FALSE(custom.isNone);
  EXPECT_TRUE(custom.isCustom);
  EXPECT_EQ(custom.color, internal::PaintServerFallbackColor());
  EXPECT_EQ(custom.customLabel, "url(#paint)");
}

TEST(EditorShellInternalTest, PaintServerSlotHandlesVariantPaintSources) {
  const css::RGBA currentColor = css::RGBA::RGB(0x10, 0x20, 0x30);

  const internal::ToolbarPaintSlotState none = internal::ToolbarPaintSlotStateForPaintServer(
      svg::PaintServer(svg::PaintServer::None{}), currentColor, nullptr, std::nullopt);
  EXPECT_TRUE(none.isNone);

  const internal::ToolbarPaintSlotState solid = internal::ToolbarPaintSlotStateForPaintServer(
      svg::PaintServer(svg::PaintServer::Solid(css::Color(css::Color::CurrentColor{}))),
      currentColor, nullptr, std::nullopt);
  EXPECT_FALSE(solid.isNone);
  EXPECT_FALSE(solid.isCustom);
  EXPECT_EQ(solid.color, currentColor);

  const internal::ToolbarPaintSlotState referenced = internal::ToolbarPaintSlotStateForPaintServer(
      svg::PaintServer(svg::PaintServer::ElementReference(
          svg::Reference("other.svg#paint"), css::Color(css::RGBA::RGB(0x80, 0x40, 0x20)))),
      currentColor, nullptr, std::nullopt);
  EXPECT_FALSE(referenced.isNone);
  EXPECT_TRUE(referenced.isCustom);
  ASSERT_TRUE(referenced.reference.has_value());
  EXPECT_EQ(referenced.reference->href, "other.svg#paint");
  EXPECT_TRUE(referenced.reference->external);
  EXPECT_EQ(referenced.color, css::RGBA::RGB(0x80, 0x40, 0x20));

  const internal::ToolbarPaintSlotState contextFill = internal::ToolbarPaintSlotStateForPaintServer(
      svg::PaintServer(svg::PaintServer::ContextFill{}), currentColor, nullptr, std::nullopt);
  EXPECT_FALSE(contextFill.isNone);
  EXPECT_TRUE(contextFill.isCustom);
  EXPECT_EQ(contextFill.customLabel, "context-fill");

  const internal::ToolbarPaintSlotState contextStroke =
      internal::ToolbarPaintSlotStateForPaintServer(
          svg::PaintServer(svg::PaintServer::ContextStroke{}), currentColor, nullptr, std::nullopt);
  EXPECT_FALSE(contextStroke.isNone);
  EXPECT_TRUE(contextStroke.isCustom);
  EXPECT_EQ(contextStroke.customLabel, "context-stroke");
}

TEST(EditorShellInternalTest, PaintChipLabelUsesReferenceCustomAndFallbackText) {
  internal::ToolbarPaintSlotState referenced;
  referenced.reference = internal::ToolbarPaintReferenceState{.href = "#very_long_gradient_name"};
  EXPECT_EQ(internal::PaintChipLabel("Fill", referenced), "Fill #very_lon...");

  internal::ToolbarPaintSlotState custom;
  custom.customLabel = "context-fill";
  EXPECT_EQ(internal::PaintChipLabel("Stroke", custom), "Stroke context-fill");

  internal::ToolbarPaintSlotState fallback;
  EXPECT_EQ(internal::PaintChipLabel("Fill", fallback), "Fill custom");
}

TEST(EditorShellInternalTest, SelectionChipLabelsClampAndNormalizeValues) {
  EXPECT_EQ(internal::SelectionSizeChipLabel(Box2d::FromXYWH(10.0, 20.0, -13.6, 7.4)), "14 x 7");
  EXPECT_EQ(internal::SelectionPositionChipLabel(Box2d::FromXYWH(-2.4, 3.6, 10.0, 20.0)),
            "(-2, 4)");
  EXPECT_EQ(internal::SelectionAngleChipLabel(Transform2d::Rotate(0.0)), "0 deg");
  EXPECT_EQ(internal::SelectionAngleChipLabel(Transform2d::Rotate(3.5)), "-159 deg");
}

TEST(EditorShellInternalTest, GeometryHelpersClampPaneWidthAndTransformBounds) {
  EXPECT_TRUE(
      internal::ContainsScreenPoint(Box2d::FromXYWH(10.0, 20.0, 30.0, 40.0), ImVec2(40.0f, 60.0f)));
  EXPECT_FALSE(
      internal::ContainsScreenPoint(Box2d::FromXYWH(10.0, 20.0, 30.0, 40.0), ImVec2(40.1f, 60.0f)));

  EXPECT_FLOAT_EQ(internal::ClampSourcePaneWidthForWindow(500.0f, 1100.0f), 500.0f);
  EXPECT_FLOAT_EQ(internal::ClampSourcePaneWidthForWindow(50.0f, 1100.0f), 240.0f);
  EXPECT_FLOAT_EQ(internal::ClampSourcePaneWidthForWindow(1200.0f, 1100.0f), 660.0f);

  const Box2d transformed = internal::TransformDocumentBox(
      Box2d::FromXYWH(0.0, 0.0, 10.0, 20.0), Transform2d::Translate(Vector2d(5.0, -3.0)));
  EXPECT_EQ(transformed.topLeft, Vector2d(5.0, -3.0));
  EXPECT_EQ(transformed.bottomRight, Vector2d(15.0, 17.0));
}

TEST(EditorShellInternalTest, TextFormatBarCapturesCanvasInputWithinItsLayoutRect) {
  const Box2d toolPaletteRect = Box2d::FromXYWH(210.0, 40.0, 180.0, 44.0);
  const std::optional<Box2d> formatBarRect = internal::TextFormatBarScreenRect(
      ImVec2(10.0f, 20.0f), ImVec2(580.0f, 360.0f), toolPaletteRect,
      /*visible=*/true, /*barHeight=*/48.0f);
  ASSERT_TRUE(formatBarRect.has_value());
  EXPECT_DOUBLE_EQ(formatBarRect->topLeft.y, 92.0);
  EXPECT_DOUBLE_EQ(formatBarRect->width(), TextFormatBarPresenter::PreferredWidth());

  const Box2d referenceChipRect = Box2d::FromXYWH(20.0, 300.0, 80.0, 24.0);
  const Box2d editingScopeBreadcrumbRect = Box2d::FromXYWH(20.0, 40.0, 96.0, 32.0);
  const Box2d zoomControlRect = Box2d::FromXYWH(520.0, 320.0, 48.0, 28.0);
  const ImVec2 formatControlPoint(static_cast<float>(formatBarRect->topLeft.x + 20.0),
                                  static_cast<float>(formatBarRect->topLeft.y + 20.0));
  EXPECT_TRUE(internal::CanvasChromeCapturesInput(formatControlPoint, referenceChipRect,
                                                  toolPaletteRect, formatBarRect,
                                                  editingScopeBreadcrumbRect, zoomControlRect));
  EXPECT_TRUE(internal::CanvasChromeCapturesInput(ImVec2(40.0f, 50.0f), referenceChipRect,
                                                  toolPaletteRect, formatBarRect,
                                                  editingScopeBreadcrumbRect, zoomControlRect));
  EXPECT_FALSE(internal::CanvasChromeCapturesInput(ImVec2(150.0f, 220.0f), referenceChipRect,
                                                   toolPaletteRect, formatBarRect,
                                                   editingScopeBreadcrumbRect, zoomControlRect));

  EXPECT_FALSE(internal::TextFormatBarScreenRect(ImVec2(10.0f, 20.0f), ImVec2(580.0f, 360.0f),
                                                 toolPaletteRect,
                                                 /*visible=*/false, /*barHeight=*/48.0f)
                   .has_value());
}

TEST(EditorShellInternalTest, DisabledSourceFocusClearsBeforePendingEditsReachThePane) {
  for (const bool flushBeforeDisable : {false, true}) {
    SCOPED_TRACE(flushBeforeDisable ? "writeback awaiting source pane" : "queued insertion");
    EditorApp app;
    ASSERT_THAT(app.loadFromString(kInitialSvg), testing::Eq(true));
    auto target = app.document().document().querySelector("#target");
    ASSERT_THAT(target, testing::Optional(testing::_));
    app.setSelection(*target);
    TextEditor source;
    source.setText(kInitialSvg);
    source.resetTextChanged();
    source.setFocusPartition(ComputeFocusPartition(app.document().document(), *target));
    ASSERT_THAT(source.hasFocusPartition(), testing::Eq(true));

    svg::SVGDocument& document = app.document().document();
    svg::SVGRectElement inserted = svg::SVGRectElement::Create(document);
    inserted.setAttribute("id", "inserted");
    app.applyMutation(EditorCommand::InsertElementCommand(document.svgElement(), inserted));
    if (flushBeforeDisable) {
      ASSERT_THAT(app.flushFrame(), testing::Eq(true));
      ASSERT_THAT(app.document().hasUnmirroredSourceDeltas(), testing::Eq(true));
    } else {
      ASSERT_THAT(app.document().hasPendingMutations(), testing::Eq(true));
    }
    const std::string beforeText = source.getText();
    const auto beforeVersion = app.document().currentFrameVersion();
    const bool beforePending = app.document().hasPendingMutations();
    const bool beforeUnmirrored = app.document().hasUnmirroredSourceDeltas();

    EXPECT_THAT(internal::DeferSourceFocusWhileAwaitingEdits(source, /*focusEnabled=*/false),
                testing::Eq(false));
    EXPECT_THAT(source.hasFocusPartition(), testing::Eq(false));
    EXPECT_THAT(source.getText(), testing::Eq(beforeText));
    EXPECT_THAT(source.isTextChanged(), testing::Eq(false));
    EXPECT_THAT(app.document().currentFrameVersion(), testing::Eq(beforeVersion));
    EXPECT_THAT(app.document().hasPendingMutations(), testing::Eq(beforePending));
    EXPECT_THAT(app.document().hasUnmirroredSourceDeltas(), testing::Eq(beforeUnmirrored));
  }
}

TEST(EditorShellInternalTest, EnabledSourceFocusRetainsPartitionWhileAwaitingEdits) {
  TextEditor source;
  source.setText("root\nhidden\ntarget");
  source.resetTextChanged();
  source.setFocusPartition(FocusPartition{
      .fullColor = {{.startLine = 2, .endLine = 3}},
      .hidden = {{.startLine = 1, .endLine = 2}},
  });
  ASSERT_THAT(source.hasFocusPartition(), testing::Eq(true));

  EXPECT_THAT(internal::DeferSourceFocusWhileAwaitingEdits(source, /*focusEnabled=*/true),
              testing::Eq(true));
  EXPECT_THAT(source.hasFocusPartition(), testing::Eq(true));
  EXPECT_THAT(source.isPositionInsideFocusRange(Coordinates(2, 0)), testing::Eq(true));
  EXPECT_THAT(source.isPositionInsideFocusRange(Coordinates(1, 0)), testing::Eq(false));
  EXPECT_THAT(source.getText(), testing::Eq("root\nhidden\ntarget"));
  EXPECT_THAT(source.isTextChanged(), testing::Eq(false));
}

TEST(EditorShellInternalTest, StructuralDocumentActionsWaitForExclusiveDomOwnership) {
  EXPECT_FALSE(internal::GroupOperationCanDispatch(
      /*rendererBusy=*/true, GroupOperationAvailability{.canApply = true}));
  EXPECT_FALSE(internal::GroupOperationCanDispatch(
      /*rendererBusy=*/false, GroupOperationAvailability{.reason = "Select more elements"}));
  EXPECT_TRUE(internal::GroupOperationCanDispatch(
      /*rendererBusy=*/false, GroupOperationAvailability{.canApply = true}));

  EXPECT_FALSE(internal::PendingDocumentReplacementCanProcess(
      /*hasPendingRequest=*/false, /*documentWriteAvailable=*/true,
      /*hasPendingMutations=*/false));
  EXPECT_FALSE(internal::PendingDocumentReplacementCanProcess(
      /*hasPendingRequest=*/true, /*documentWriteAvailable=*/false,
      /*hasPendingMutations=*/false));
  EXPECT_FALSE(internal::PendingDocumentReplacementCanProcess(
      /*hasPendingRequest=*/true, /*documentWriteAvailable=*/true,
      /*hasPendingMutations=*/true));
  EXPECT_TRUE(internal::PendingDocumentReplacementCanProcess(
      /*hasPendingRequest=*/true, /*documentWriteAvailable=*/true,
      /*hasPendingMutations=*/false));
}

TEST(EditorShellInternalTest, SidebarSnapshotRefreshYieldsToInteractionAndRenderRequests) {
  EXPECT_TRUE(internal::ShouldRefreshSidebarSnapshots(
      /*rendererBusy=*/false, /*interactionActive=*/false, /*previewsPaused=*/false));
  EXPECT_FALSE(internal::ShouldRefreshSidebarSnapshots(
      /*rendererBusy=*/true, /*interactionActive=*/false, /*previewsPaused=*/false));
  EXPECT_FALSE(internal::ShouldRefreshSidebarSnapshots(
      /*rendererBusy=*/false, /*interactionActive=*/true, /*previewsPaused=*/false));
  EXPECT_FALSE(internal::ShouldRefreshSidebarSnapshots(
      /*rendererBusy=*/true, /*interactionActive=*/true, /*previewsPaused=*/false));
}

TEST(EditorShellInternalTest, DeferredLayerThumbnailsRetainSidebarRefreshObligation) {
  EXPECT_FALSE(internal::SidebarSnapshotRefreshPendingAfterPass(/*deferredThumbnailCount=*/0u));
  EXPECT_TRUE(internal::SidebarSnapshotRefreshPendingAfterPass(/*deferredThumbnailCount=*/3u))
      << "deferred nested thumbnails need a durable refresh obligation, not a one-shot wake";
}

TEST(EditorShellInternalTest, BusyFrameReplaysDocumentUiBooleansWithoutEvaluatingLiveResolver) {
  bool resolverCalled = false;
  EXPECT_TRUE(internal::ResolveCachedDocumentBoolForFrame(
      /*rendererBusy=*/true, /*cachedValue=*/true, [&] {
        resolverCalled = true;
        return false;
      }));
  EXPECT_FALSE(resolverCalled)
      << "A busy UI frame must not enter document traversal just to refresh menu state.";

  EXPECT_FALSE(internal::ResolveCachedDocumentBoolForFrame(
      /*rendererBusy=*/false, /*cachedValue=*/true, [&] {
        resolverCalled = true;
        return false;
      }));
  EXPECT_TRUE(resolverCalled);
}

TEST(EditorShellInternalTest, LateSamplePickerActionsRequestAHostFollowupFrame) {
  EXPECT_FALSE(internal::SamplePickerActionsNeedFollowupFrame(/*dismiss=*/false,
                                                              /*openFile=*/false,
                                                              /*newDocument=*/false));
  EXPECT_TRUE(internal::SamplePickerActionsNeedFollowupFrame(/*dismiss=*/true,
                                                             /*openFile=*/false,
                                                             /*newDocument=*/false));
  EXPECT_TRUE(internal::SamplePickerActionsNeedFollowupFrame(/*dismiss=*/false,
                                                             /*openFile=*/true,
                                                             /*newDocument=*/false));
  EXPECT_TRUE(internal::SamplePickerActionsNeedFollowupFrame(/*dismiss=*/false,
                                                             /*openFile=*/false,
                                                             /*newDocument=*/true));
}

TEST(EditorShellInternalTest, PendingSamplePresentationOwnsTheRenderWorker) {
  EXPECT_FALSE(internal::ShouldAdvanceSampleThumbnails(/*showSamplePicker=*/false,
                                                       /*samplePresentationPending=*/false));
  EXPECT_TRUE(internal::ShouldAdvanceSampleThumbnails(/*showSamplePicker=*/true,
                                                      /*samplePresentationPending=*/false));
  EXPECT_FALSE(internal::ShouldAdvanceSampleThumbnails(/*showSamplePicker=*/true,
                                                       /*samplePresentationPending=*/true))
      << "A selected sample's first document render must not race a carousel thumbnail.";
}

TEST(EditorShellInternalTest, BusyDeferredRenderWaitsForWorkerCompletionInsteadOfSpinning) {
  EXPECT_EQ(internal::DeferredRenderActionForState(
                /*hasDocument=*/true, /*penDragFlushed=*/false, /*rendererBusy=*/true),
            internal::DeferredRenderAction::WaitForRendererCompletion);
  EXPECT_EQ(internal::DeferredRenderActionForState(
                /*hasDocument=*/true, /*penDragFlushed=*/true, /*rendererBusy=*/true),
            internal::DeferredRenderAction::WakeForPenDrag);
}

TEST(EditorShellInternalTest, CompactChromeCapturesSheetAndUsesTouchHint) {
  const Box2d panelRect = Box2d::FromXYWH(480.0, 52.0, 360.0, 338.0);
  EXPECT_TRUE(internal::CanvasChromeCapturesInput(
      ImVec2(500.0f, 100.0f), std::nullopt, Box2d::FromXYWH(20.0, 70.0, 156.0, 60.0), std::nullopt,
      std::nullopt, Box2d::FromXYWH(20.0, 320.0, 44.0, 44.0), panelRect));
  EXPECT_NE(internal::TextToolHintLabel(/*isEditing=*/false, /*isDraggingBox=*/false,
                                        /*touchPreferred=*/true)
                .find("Double-tap"),
            std::string_view::npos);
}

TEST(EditorShellInternalTest, HiddenCanvasScrollbarsDoNotCaptureCanvasInput) {
  ViewportState viewport;
  viewport.paneOrigin = Vector2d(0.0, 0.0);
  viewport.paneSize = Vector2d(100.0, 100.0);
  viewport.documentViewBox = Box2d::FromXYWH(0.0, 0.0, 400.0, 400.0);
  viewport.zoom = 1.0;
  viewport.panDocPoint = Vector2d(50.0, 50.0);
  viewport.panScreenPoint = Vector2d(50.0, 50.0);

  const Vector2d bottomRailPoint(50.0, 95.0);
  EXPECT_TRUE(internal::CanvasScrollbarsCaptureInput(true, viewport, bottomRailPoint));
  EXPECT_FALSE(internal::CanvasScrollbarsCaptureInput(false, viewport, bottomRailPoint));
}

TEST(EditorShellInternalTest, PendingClickBusyActionPrefersFastRedragThenCancelsBusyRender) {
  EXPECT_EQ(internal::PendingClickBusyActionForState(/*tookFastRedrag=*/true,
                                                     /*documentWriteUnavailable=*/true),
            internal::PendingClickBusyAction::CompleteFastRedrag);
  EXPECT_EQ(internal::PendingClickBusyActionForState(/*tookFastRedrag=*/true,
                                                     /*documentWriteUnavailable=*/false),
            internal::PendingClickBusyAction::CompleteFastRedrag);

  EXPECT_EQ(internal::PendingClickBusyActionForState(/*tookFastRedrag=*/false,
                                                     /*documentWriteUnavailable=*/true),
            internal::PendingClickBusyAction::CancelBusyRender);
  EXPECT_EQ(internal::PendingClickBusyActionForState(/*tookFastRedrag=*/false,
                                                     /*documentWriteUnavailable=*/false),
            internal::PendingClickBusyAction::RunIdleClickPath);
}

TEST(EditorShellInternalTest, PendingClickIdleActionWaitsForMarqueeIntentBeforeSlowPath) {
  EXPECT_EQ(internal::PendingClickIdleActionForState(
                /*leftMouseDown=*/true, /*pendingClickCanStartMarquee=*/true,
                /*selectHoldElapsed=*/true, /*selectDragIntent=*/false),
            internal::PendingClickIdleAction::BeginMarquee);
  EXPECT_EQ(internal::PendingClickIdleActionForState(
                /*leftMouseDown=*/true, /*pendingClickCanStartMarquee=*/true,
                /*selectHoldElapsed=*/false, /*selectDragIntent=*/true),
            internal::PendingClickIdleAction::BeginMarquee);

  EXPECT_EQ(internal::PendingClickIdleActionForState(
                /*leftMouseDown=*/true, /*pendingClickCanStartMarquee=*/true,
                /*selectHoldElapsed=*/false, /*selectDragIntent=*/false),
            internal::PendingClickIdleAction::WaitForMarqueeIntent);

  EXPECT_EQ(internal::PendingClickIdleActionForState(
                /*leftMouseDown=*/false, /*pendingClickCanStartMarquee=*/true,
                /*selectHoldElapsed=*/true, /*selectDragIntent=*/true),
            internal::PendingClickIdleAction::DispatchSlowPath);
  EXPECT_EQ(internal::PendingClickIdleActionForState(
                /*leftMouseDown=*/true, /*pendingClickCanStartMarquee=*/false,
                /*selectHoldElapsed=*/true, /*selectDragIntent=*/true),
            internal::PendingClickIdleAction::DispatchSlowPath);
}

TEST(EditorShellInternalTest, ActivePaintStateUsesFillAndStrokeAttributes) {
  ActivePaintStyle style;
  style.fill = "#ff0000";
  style.stroke = "url(#paint)";

  const internal::ToolbarPaintState state = internal::ToolbarPaintStateForActivePaint(style);

  EXPECT_FALSE(state.fill.isNone);
  EXPECT_FALSE(state.fill.isCustom);
  EXPECT_EQ(state.fill.color, css::RGBA::RGB(0xff, 0x00, 0x00));
  EXPECT_FALSE(state.stroke.isNone);
  EXPECT_TRUE(state.stroke.isCustom);
  EXPECT_EQ(state.stroke.customLabel, "url(#paint)");
}

TEST(EditorShellInternalTest, ActivePaintStyleDefaultsToWhiteFillBlackStroke) {
  const ActivePaintStyle style;
  EXPECT_EQ(style.fill, "white");
  EXPECT_EQ(style.stroke, "black");
}

TEST(EditorShellInternalTest, SwapActivePaintSwapsFillAndStroke) {
  ActivePaintStyle style;
  style.fill = "#112233";
  style.stroke = "#445566";
  internal::SwapActivePaint(style);
  EXPECT_EQ(style.fill, "#445566");
  EXPECT_EQ(style.stroke, "#112233");

  // A fresh document's white fill / black stroke swaps cleanly.
  ActivePaintStyle fresh;
  internal::SwapActivePaint(fresh);
  EXPECT_EQ(fresh.fill, "black");
  EXPECT_EQ(fresh.stroke, "white");
}

TEST(EditorShellInternalTest, SvgPaintStringForSlotCoversNoneSolidAndReference) {
  internal::ToolbarPaintSlotState none;
  none.isNone = true;
  EXPECT_EQ(internal::SvgPaintStringForSlot(none), "none");

  internal::ToolbarPaintSlotState solid;
  solid.isNone = false;
  solid.color = css::RGBA::RGB(0x11, 0x22, 0x33);
  EXPECT_EQ(internal::SvgPaintStringForSlot(solid), css::RGBA::RGB(0x11, 0x22, 0x33).toHexString());

  internal::ToolbarPaintSlotState reference;
  reference.isNone = false;
  reference.isCustom = true;
  internal::ToolbarPaintReferenceState ref;
  ref.href = "#grad";
  reference.reference = ref;
  EXPECT_EQ(internal::SvgPaintStringForSlot(reference), "url(#grad)");

  internal::ToolbarPaintSlotState context;
  context.isNone = false;
  context.isCustom = true;
  context.customLabel = "context-fill";
  EXPECT_EQ(internal::SvgPaintStringForSlot(context), "context-fill");
}

TEST(EditorShellInternalTest, ReferencedPaintSerializerRetainsUnresolvedFallbackColors) {
  const css::RGBA currentColor = css::RGBA::RGB(0x33, 0x66, 0x99);
  for (const auto& [color, expected] : std::vector<std::pair<css::Color, std::string>>{
           {css::Color(css::RGBA::RGB(255, 0, 0)), "url(#missing) #ff0000"},
           {css::Color(css::RGBA(0x33, 0x66, 0x99, 0x80)), "url(#missing) #33669980"},
           {css::Color(css::Color::CurrentColor()), "url(#missing) currentColor"}}) {
    SCOPED_TRACE(expected);
    const svg::PaintServer paint(svg::PaintServer::ElementReference("#missing", color));
    const auto state =
        internal::ToolbarPaintSlotStateForPaintServer(paint, currentColor, nullptr, std::nullopt);
    EXPECT_EQ(internal::SvgPaintStringForSlot(state), expected);
  }
}

TEST(EditorShellInternalTest, FillStrokeWidgetLayoutAndHitTestClassifyRegions) {
  const ImVec2 widgetMin(100.0f, 200.0f);
  const ImVec2 widgetMax(widgetMin.x + 120.0f, widgetMin.y + 44.0f);
  const internal::FillStrokeWidgetLayout layout =
      internal::ComputeFillStrokeWidgetLayout(widgetMin, widgetMax);

  const auto center = [](const ImVec2& a, const ImVec2& b) {
    return ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
  };
  const auto hit = [&](const ImVec2& p, bool fillCustom, bool strokeCustom, bool fillActive) {
    return internal::HitTestFillStrokeWidget(layout, p, fillCustom, strokeCustom, fillActive);
  };

  EXPECT_EQ(hit(center(layout.swapMin, layout.swapMax), false, false, true),
            internal::FillStrokeWidgetRegion::Swap);
  EXPECT_EQ(hit(center(layout.noneMin, layout.noneMax), false, false, true),
            internal::FillStrokeWidgetRegion::SetNone);

  const ImVec2 overlap(layout.fillMax.x - 3.0f, layout.fillMax.y - 3.0f);
  EXPECT_EQ(hit(overlap, false, false, true), internal::FillStrokeWidgetRegion::FillSwatch);
  EXPECT_EQ(hit(overlap, false, false, false), internal::FillStrokeWidgetRegion::StrokeSwatch);
  const ImVec2 fillOnly(layout.fillMin.x + 4.0f, layout.fillMin.y + 4.0f);
  const ImVec2 strokeOnly(layout.strokeMax.x - 4.0f, layout.strokeMax.y - 4.0f);
  EXPECT_EQ(hit(fillOnly, false, false, false), internal::FillStrokeWidgetRegion::FillSwatch);
  EXPECT_EQ(hit(strokeOnly, false, false, true), internal::FillStrokeWidgetRegion::StrokeSwatch);

  // Chips only classify when their slot carries custom paint (only then drawn).
  const ImVec2 fillChip = center(layout.fillChipMin, layout.fillChipMax);
  EXPECT_EQ(hit(fillChip, false, false, true), internal::FillStrokeWidgetRegion::None);
  EXPECT_EQ(hit(fillChip, true, false, true), internal::FillStrokeWidgetRegion::FillChip);
  const ImVec2 strokeChip = center(layout.strokeChipMin, layout.strokeChipMax);
  EXPECT_EQ(hit(strokeChip, false, true, true), internal::FillStrokeWidgetRegion::StrokeChip);
}

TEST(EditorShellInternalTest, FillStrokeWidgetInteractionStateIgnoresBusyHandoffsDuringDrag) {
  const internal::FillStrokeWidgetInteractionState idleDrag =
      internal::ResolveFillStrokeWidgetInteractionState(
          /*hasDocument=*/true, /*rendererBusy=*/false, /*canvasInteractionActive=*/true,
          /*paintSnapshotMatchesSelection=*/true);
  const internal::FillStrokeWidgetInteractionState busyDrag =
      internal::ResolveFillStrokeWidgetInteractionState(
          /*hasDocument=*/true, /*rendererBusy=*/true, /*canvasInteractionActive=*/true,
          /*paintSnapshotMatchesSelection=*/true);
  EXPECT_EQ(idleDrag.canEdit, busyDrag.canEdit);
  EXPECT_EQ(idleDrag.refreshPaintSnapshot, busyDrag.refreshPaintSnapshot);
  EXPECT_FALSE(idleDrag.canEdit);
  EXPECT_FALSE(idleDrag.refreshPaintSnapshot);

  const internal::FillStrokeWidgetInteractionState firstDragFrame =
      internal::ResolveFillStrokeWidgetInteractionState(
          /*hasDocument=*/true, /*rendererBusy=*/false, /*canvasInteractionActive=*/true,
          /*paintSnapshotMatchesSelection=*/false);
  EXPECT_FALSE(firstDragFrame.canEdit);
  EXPECT_TRUE(firstDragFrame.refreshPaintSnapshot);

  const internal::FillStrokeWidgetInteractionState settled =
      internal::ResolveFillStrokeWidgetInteractionState(
          /*hasDocument=*/true, /*rendererBusy=*/false, /*canvasInteractionActive=*/false,
          /*paintSnapshotMatchesSelection=*/true);
  EXPECT_TRUE(settled.canEdit);
  EXPECT_TRUE(settled.refreshPaintSnapshot);
}

TEST(EditorShellInternalTest, SourceHelpersPreferInitialSourceAndCanonicalizeTrailingNewline) {
  const std::filesystem::path path = TempPathForTest("initial.svg");
  WriteTextFile(path, "<svg id=\"from-file\"/>\n");

  EditorShellOptions sourceOptions;
  sourceOptions.svgPath = path.string();
  sourceOptions.initialSource = "<svg id=\"from-memory\"/>";
  EXPECT_EQ(internal::InitialDocumentSyncSource(sourceOptions), "<svg id=\"from-memory\"/>");

  EditorShellOptions fileOptions;
  fileOptions.svgPath = path.string();
  EXPECT_EQ(internal::InitialDocumentSyncSource(fileOptions), "<svg id=\"from-file\"/>\n");

  EditorShellOptions missingOptions;
  missingOptions.svgPath = path.string() + ".missing";
  EXPECT_TRUE(internal::InitialDocumentSyncSource(missingOptions).empty());

  EXPECT_EQ(internal::CanonicalizeForTextEditor("abc\n"), "abc");
  EXPECT_EQ(internal::CanonicalizeForTextEditor("abc"), "abc");
  EXPECT_TRUE(internal::CanonicalizeForTextEditor("").empty());
}

TEST(EditorShellInternalTest, ReferenceHighlightChipLabelCombinesDirections) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kReferencedSvg));
  auto target = app.document().document().querySelector("#target");
  auto paint = app.document().document().querySelector("#paint");
  auto referrer = app.document().document().querySelector("#referrer");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(paint.has_value());
  ASSERT_TRUE(referrer.has_value());

  ReferenceHighlightSummary summary;
  EXPECT_TRUE(internal::ReferenceHighlightChipLabel(summary).empty());

  summary.referencedElements = {*target, *paint};
  EXPECT_EQ(internal::ReferenceHighlightChipLabel(summary), "-> 2");

  summary.referencingElements = {*target, *paint, *referrer};
  EXPECT_EQ(internal::ReferenceHighlightChipLabel(summary), "-> 2  <- 3");

  summary.referencedElements.clear();
  EXPECT_EQ(internal::ReferenceHighlightChipLabel(summary), "<- 3");
}

TEST(EditorShellInternalTest, ElementCollectionHelpersKeepUniqueElementsAndLabels) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kReferencedSvg));

  auto target = app.document().document().querySelector("#target");
  auto paint = app.document().document().querySelector("#paint");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(paint.has_value());

  std::vector<svg::SVGElement> elements;
  internal::AddUniqueElements(&elements, std::vector<svg::SVGElement>{*target, *paint, *target});

  ASSERT_EQ(elements.size(), 2u);
  EXPECT_TRUE(internal::ContainsElement(elements, *target));
  EXPECT_TRUE(internal::ContainsElement(elements, *paint));
  EXPECT_EQ(internal::ElementContextMenuLabel(*target), "<rect> #target");
  EXPECT_EQ(internal::ElementContextMenuLabel(*paint), "<linearGradient> #paint");
}

TEST(EditorShellInternalTest, ResolveDocumentViewBoxUsesViewBoxIntrinsicSizeAndDefault) {
  EditorApp viewBoxApp;
  ASSERT_TRUE(viewBoxApp.loadFromString(kInitialSvg));
  EXPECT_EQ(internal::ResolveDocumentViewBox(viewBoxApp.document().document()),
            Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  EditorApp intrinsicApp;
  ASSERT_TRUE(intrinsicApp.loadFromString(kIntrinsicSizeSvg));
  EXPECT_EQ(internal::ResolveDocumentViewBox(intrinsicApp.document().document()),
            Box2d::FromXYWH(0.0, 0.0, 160.0, 90.0));

  EditorApp defaultApp;
  ASSERT_TRUE(defaultApp.loadFromString("<svg xmlns=\"http://www.w3.org/2000/svg\"/>"));
  EXPECT_EQ(internal::ResolveDocumentViewBox(defaultApp.document().document()),
            Box2d::FromXYWH(0.0, 0.0, 512.0, 512.0));
}

TEST(EditorShellInternalTest, ResolveDocumentViewBoxIgnoresCommittedRasterCanvasSize) {
  // RenderCoordinator commits zoom/DPR-scaled raster canvas sizes into the
  // document via setCanvasSize. For a viewBox-less document the resolved
  // viewBox must stay pinned to the intrinsic width/height attributes.
  // Reading the committed canvas back instead inflates the viewBox by the
  // device pixel ratio on every commit until the max-canvas clamp, which
  // starves zoom-driven re-rasters and froze zoom/scroll rendering on
  // viewBox-less documents such as z0rly_test6.svg.
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kIntrinsicSizeSvg));
  app.document().document().setCanvasSize(320, 180);  // A 2x-DPR raster commit.
  EXPECT_EQ(internal::ResolveDocumentViewBox(app.document().document()),
            Box2d::FromXYWH(0.0, 0.0, 160.0, 90.0));

  // Even a wildly inflated committed canvas (the runaway feedback fixed
  // point) must not leak into the resolved viewBox.
  app.document().document().setCanvasSize(8192, 4608);
  EXPECT_EQ(internal::ResolveDocumentViewBox(app.document().document()),
            Box2d::FromXYWH(0.0, 0.0, 160.0, 90.0));
}

TEST(EditorShellInternalTest, BusyFrameReusesCachedViewBoxWithoutWaitingForDocumentAccess) {
  svg::SVGDocument document;
  document.setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  std::atomic<bool> writerReady = false;
  std::atomic<bool> releaseWriter = false;
  std::thread writer([document, &writerReady, &releaseWriter]() mutable {
    svg::DocumentWriteAccess access = document.writeAccess();
    writerReady.store(true, std::memory_order_release);
    while (!releaseWriter.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  });
  while (!writerReady.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  std::future<std::optional<Box2d>> frameViewBox = std::async(std::launch::async, [&] {
    return internal::ResolveDocumentViewBoxForFrame(document, /*rendererBusy=*/true);
  });
  const std::future_status status = frameViewBox.wait_for(std::chrono::milliseconds(100));
  releaseWriter.store(true, std::memory_order_release);
  writer.join();

  ASSERT_EQ(status, std::future_status::ready)
      << "A UI frame must never block on the worker's document write guard.";
  EXPECT_FALSE(frameViewBox.get().has_value())
      << "nullopt tells updatePaneLayout to retain its immutable cached viewBox epoch.";
}

TEST(EditorShellInternalTest, PaintReferenceStateIncludesSameDocumentSourceRange) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kPaintToolbarSvg));
  svg::SVGDocument& document = app.document().document();

  const internal::ToolbarPaintReferenceState local = internal::ToolbarPaintReferenceStateFor(
      &document, document.source(), svg::Reference("#paint"));
  EXPECT_EQ(local.href, "#paint");
  EXPECT_FALSE(local.external);
  ASSERT_TRUE(local.sourceRange.has_value());
  EXPECT_NE(std::string_view(document.source())
                .substr(local.sourceRange->start, local.sourceRange->end - local.sourceRange->start)
                .find("<linearGradient id=\"paint\""),
            std::string_view::npos);

  const internal::ToolbarPaintReferenceState withoutSource =
      internal::ToolbarPaintReferenceStateFor(&document, std::nullopt, svg::Reference("#paint"));
  EXPECT_FALSE(withoutSource.sourceRange.has_value());

  const internal::ToolbarPaintReferenceState external = internal::ToolbarPaintReferenceStateFor(
      &document, document.source(), svg::Reference("other.svg#paint"));
  EXPECT_TRUE(external.external);
  EXPECT_FALSE(external.sourceRange.has_value());
}

class EditorShellTestAccess {
public:
  using NativeSaveOutcome = EditorShell::NativeSaveOutcome;

  static void SetAnimationPreview(EditorShell& shell, bool enabled) {
    shell.setAnimationPreview(enabled);
  }
  static void AdvanceAnimationPreview(EditorShell& shell, double nowSeconds,
                                      EditorFrameKind frameKind = EditorFrameKind::Scheduled) {
    shell.advanceAnimationPreview(nowSeconds, frameKind);
  }
  static bool PreviewInputReady(const EditorShell& shell) {
    return shell.animationFramePresented() && !shell.animationResetPending_ &&
           !shell.renderCoordinator_.asyncRenderer().isBusy();
  }
  static void SetPreviewStart(EditorShell& shell, double seconds) {
    shell.animationPreviewStartSeconds_ = seconds;
  }
  static void ClearPreviewPointer(EditorShell& shell) { shell.resetPreviewPointer(); }
  static void SetPreviewResetPending(EditorShell& shell, bool pending) {
    shell.animationResetPending_ = pending;
  }
  static std::size_t QueuedPreviewEdges(const EditorShell& shell) {
    return shell.previewPointerEdges_.size();
  }
  static void QueuePreviewClick(EditorShell& shell, const svg::SVGElement& target, Vector2d point) {
    for (bool pressed : {true, false}) {
      shell.previewPointerInput_ = {
          .point = point,
          .target = target,
          .documentGeneration = shell.app_.document().documentGeneration(),
          .pressed = pressed,
          .released = !pressed,
      };
      shell.queuePreviewPointerEdge();
    }
  }
  static void ApplyPreviewInputAt(EditorShell& shell, double nowSeconds) {
    shell.applyPreviewPointerInput(nowSeconds);
  }
  static bool IsAnimationPreview(const EditorShell& shell) { return shell.animationPreview_; }
  static bool ViewportInitialized(const EditorShell& shell) { return shell.viewportInitialized_; }
  static Box2d ToolPaletteRect(const EditorShell& shell) {
    const ViewportState& viewport = shell.interactionController_.viewport();
    return shell.toolPaletteScreenRect(
        ImVec2(static_cast<float>(viewport.paneOrigin.x),
               static_cast<float>(viewport.paneOrigin.y)),
        ImVec2(static_cast<float>(viewport.paneSize.x), static_cast<float>(viewport.paneSize.y)));
  }
  static bool AnimationResetPending(const EditorShell& shell) {
    return shell.animationResetPending_;
  }
  static void HoldAnimationAtStart(EditorShell& shell) {
    shell.animationPreviewStartSeconds_ = std::numeric_limits<double>::max();
  }
  static std::optional<svg::SVGElement> PreviewHoveredElement(const EditorShell& shell) {
    return shell.previewHoveredElement_;
  }
  static std::vector<std::pair<svg::EventType, RcString>> HoverTransition(
      EditorShell& shell, const std::optional<svg::SVGElement>& target) {
    std::vector<EditorShell::PreviewMouseDispatch> events;
    shell.collectPreviewHoverEvents(target, events);
    std::vector<std::pair<svg::EventType, RcString>> result;
    for (const auto& event : events) {
      result.emplace_back(event.type, event.target.id());
    }
    return result;
  }
  static Box2d AnimationControlRect(const EditorShell& shell) {
    return shell.animationPreviewControlScreenRect();
  }

  static EditorApp& App(EditorShell& shell) { return shell.app_; }
  static const EditorApp& App(const EditorShell& shell) { return shell.app_; }
  static std::size_t PendingHistoryActionCount(const EditorShell& shell) {
    return shell.pendingHistoryActions_.size();
  }

  static void RequestUndo(EditorShell& shell) {
    shell.requestHistoryAction(EditorShell::HistoryAction::Undo);
  }

  static void RequestRedo(EditorShell& shell) {
    shell.requestHistoryAction(EditorShell::HistoryAction::Redo);
  }

  static TextEditor& Source(EditorShell& shell) { return shell.textEditor_; }
  static const TextEditor& Source(const EditorShell& shell) { return shell.textEditor_; }

  static ImFont* UiBoldFont(const EditorShell& shell) { return shell.uiFontBold_; }
  static ImFont* CodeFont(const EditorShell& shell) { return shell.codeFont_; }

  static void RequestFontPreviews(EditorShell& shell, std::vector<std::string> families) {
    shell.visibleFontPreviewFamilies_.insert(families.begin(), families.end());
    shell.requestFontPreviews(families);
  }

  static void PrepareSingleFontPreviewRequest(EditorShell& shell, std::string family) {
    shell.pendingFontPreviews_.clear();
    shell.waitingFontPreviews_.clear();
    shell.fontPreviewInFlight_.reset();
    shell.sampleThumbnailInFlightIndex_.reset();
    shell.fontPreviewBitmaps_.erase(family);
    shell.fontPreviewIdentities_.erase(family);
    RequestFontPreviews(shell, {std::move(family)});
  }

  static std::size_t PendingFontPreviewCount(const EditorShell& shell) {
    return shell.pendingFontPreviews_.size();
  }

  static void AdvanceVisiblePreviews(EditorShell& shell) { shell.advanceVisiblePreviews(); }

  static void AdvanceFontPreviewGeneration(EditorShell& shell) {
    shell.advanceFontPreviewGeneration();
  }

  static std::size_t RequestedAuxiliaryPreviewCount(const EditorShell& shell) {
    return shell.renderCoordinator_.asyncRenderer().sampleThumbnailRenderStats().requested;
  }

  static std::size_t LayerThumbnailRefreshCount(const EditorShell& shell) {
    return shell.layersPanel_.thumbnailRefreshCount();
  }

  static std::size_t LayerThumbnailRenderAttempts(const EditorShell& shell) {
    return shell.layersPanel_.thumbnailRenderAttemptCount();
  }

  static std::size_t LayerThumbnailCacheInspections(const EditorShell& shell) {
    return shell.layersPanel_.thumbnailCacheInspectionCountForTesting();
  }

  static std::size_t RenderedLayerThumbnailCount(const EditorShell& shell) {
    return shell.layerInspectorStatusForReadback().thumbnailRefreshStats.renderedCount;
  }

  static bool ForegroundPresentationAcknowledged(const EditorShell& shell) {
    return shell.foregroundPresentationAcknowledged();
  }

  static std::optional<gui::EditorWindow::PresentedDocumentFrameIdentity>
  ForegroundPresentationObligation(const EditorShell& shell) {
    return shell.foregroundPresentationObligation_;
  }

  static void SetForegroundPresentationObligation(
      EditorShell& shell, gui::EditorWindow::PresentedDocumentFrameIdentity identity) {
    shell.foregroundPresentationObligation_ = identity;
  }

  static void SetSidebarSnapshotRefreshPending(EditorShell& shell, bool pending) {
    shell.sidebarSnapshotRefreshPending_ = pending;
  }

  static bool SidebarSnapshotRefreshPending(const EditorShell& shell) {
    return shell.sidebarSnapshotRefreshPending_;
  }

  static void InstallFramePresentation(EditorShell& shell,
                                       std::shared_ptr<const FramePresentation> frame) {
    shell.installFramePresentation(std::move(frame));
  }

  static FormatBarFontPreview FontPreviewForFamily(EditorShell& shell, std::string_view family) {
    return shell.fontPreviewForFamily(family);
  }

  static std::size_t CachedFontPreviewCount(const EditorShell& shell) {
    return shell.fontPreviewBitmaps_.size();
  }

  static std::vector<svg::FontFaceDependency> FontPreviewDependencies(const EditorShell& shell,
                                                                      std::string_view family) {
    const auto it = shell.fontPreviewIdentities_.find(std::string(family));
    return it == shell.fontPreviewIdentities_.end() ? std::vector<svg::FontFaceDependency>{}
                                                    : it->second.dependencies;
  }

  static SampleThumbnailRenderResult PendingSampleFontResult(EditorShell& shell,
                                                             svg::FontFaceDependency dependency,
                                                             std::uint64_t wakeRevision) {
    shell.showSamplePicker_ = true;
    shell.visibleSamplePreviewIndices_.insert(0);
    shell.sampleThumbnailInFlightIndex_ = 0;
    return {.kind = AuxiliaryPreviewKind::Sample,
            .key = 0,
            .taskGeneration = shell.previewTaskGeneration_,
            .fontWakeRevision = wakeRevision,
            .fontDependencies = {std::move(dependency)},
            .outcome = SampleThumbnailRenderOutcome::FontsPending};
  }

  static void ConsumePreviewResult(EditorShell& shell, SampleThumbnailRenderResult result) {
    shell.handleAuxiliaryPreviewResult(std::move(result));
  }

  static void RetryPendingFontPreviews(EditorShell& shell) { shell.retryPendingFontPreviews(); }
  static void UpdateVisiblePreviewTasks(EditorShell& shell) { shell.updateVisiblePreviewTasks(); }
  static void CancelPreviews(EditorShell& shell) { shell.cancelSampleThumbnailGeneration(); }
  static std::size_t PendingSampleFontCount(const EditorShell& shell) {
    return shell.waitingSamplePreviews_.size();
  }
  static std::size_t FinishedSampleCount(const EditorShell& shell) {
    return shell.finishedSamplePreviewIndices_.size();
  }

  static void RememberOutputFonts(EditorShell& shell,
                                  std::span<const svg::FontFaceDependency> dependencies) {
    shell.rememberOutputFontDemand(dependencies);
  }
  static void DrainOutputFonts(EditorShell& shell) { shell.drainOutputFontDemand(); }
  static std::size_t OutputFontDemandCount(const EditorShell& shell) {
    return shell.outputFontDemand_.size();
  }

  static bool TryOpenPath(EditorShell& shell, std::string_view path, std::string* error) {
    return shell.tryOpenPath(path, error);
  }

  static void SetShowSamplePicker(EditorShell& shell, bool visible) {
    shell.showSamplePicker_ = visible;
  }

  static bool ShowSamplePicker(const EditorShell& shell) { return shell.showSamplePicker_; }

  static std::size_t SampleThumbnailCursor(const EditorShell& shell) {
    return shell.sampleThumbnailGenerationCursor_;
  }

  static std::size_t VisibleSamplePreviewCount(const EditorShell& shell) {
    return shell.visibleSamplePreviewIndices_.size();
  }

  static std::size_t SampleThumbnailSlotCount(const EditorShell& shell) {
    return shell.sampleThumbnailBitmaps_.size();
  }

  static std::size_t SampleThumbnailGeneratedCount(const EditorShell& shell) {
    std::size_t count = 0;
    for (const auto& bitmap : shell.sampleThumbnailBitmaps_) {
      if (bitmap.has_value()) {
        ++count;
      }
    }
    return count;
  }

  static const std::optional<svg::RendererBitmap>& SampleThumbnailBitmap(const EditorShell& shell,
                                                                         std::size_t index) {
    return shell.sampleThumbnailBitmaps_.at(index);
  }

  static bool TrySavePath(EditorShell& shell, std::string_view path, std::string* error) {
    return shell.trySavePath(path, error);
  }

  static void RequestRevert(EditorShell& shell) { shell.requestRevert(); }

  static bool TryApplyGroupOperation(EditorShell& shell, bool ungroup) {
    return shell.tryApplyGroupOperation(ungroup);
  }

  static void QueueSampleLoad(EditorShell& shell, std::string sampleId) {
    shell.queuePendingSampleLoad(std::move(sampleId));
    shell.showSamplePicker_ = true;
  }

  static void ProcessPendingSampleLoad(EditorShell& shell) { shell.processPendingSampleLoad(); }

  static std::string_view PendingSampleLoad(const EditorShell& shell) {
    return shell.pendingSampleLoadId_;
  }

  static bool PendingSampleLoadNeedsConfirmation(const EditorShell& shell) {
    return shell.pendingSampleLoadNeedsConfirmation_;
  }

  static void ConfirmPendingSampleLoadDiscard(EditorShell& shell) {
    shell.confirmPendingSampleLoadDiscard();
  }

  static void RequestSave(EditorShell& shell) { shell.requestSave(); }

  static void RequestSaveAs(EditorShell& shell, std::string error = std::string()) {
    shell.requestSaveAs(std::move(error));
  }

  static void QueueNativeSaveSelection(EditorShell& shell, std::string path) {
    shell.queueNativeSaveSelection(std::move(path));
  }

  static NativeSaveOutcome TryPendingNativeSave(EditorShell& shell, std::string* error) {
    return shell.tryPendingNativeSave(error);
  }

  static std::optional<std::string> PendingNativeSavePath(const EditorShell& shell) {
    return shell.pendingNativeSavePath_;
  }

  static void ExpirePendingNativeSave(EditorShell& shell) {
    shell.pendingNativeSaveDeadline_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
  }

  static void RequestExportViewportSvg(EditorShell& shell, bool includeOverlay,
                                       std::string error = std::string()) {
    shell.requestExportViewportSvg(includeOverlay, std::move(error));
  }

  static bool TryExportViewportSvgToPath(EditorShell& shell, std::string_view path,
                                         std::string* error) {
    return shell.tryExportViewportSvgToPath(path, error);
  }

  static bool PendingViewportExport(const EditorShell& shell) {
    return shell.pendingViewportExport_;
  }

  static bool PendingViewportExportOverlay(const EditorShell& shell) {
    return shell.pendingViewportExportOverlay_;
  }

  static bool OpenFileModalRequested(const EditorShell& shell) {
    return shell.dialogPresenter_.openFileModalRequested();
  }

  static bool SaveFileModalRequested(const EditorShell& shell) {
    return shell.dialogPresenter_.saveFileModalRequested();
  }

  static bool AboutPopupRequested(const EditorShell& shell) {
    return shell.dialogPresenter_.aboutPopupRequested();
  }

  static void MaybeLogResourceDiagnostics(EditorShell& shell, const FrameCostBreakdown& frameCost) {
    shell.maybeLogResourceDiagnostics(frameCost);
  }

  static void MaybeLogFrameMissTelemetry(EditorShell& shell, const FrameCostBreakdown& frameCost) {
    shell.maybeLogFrameMissTelemetry(frameCost);
  }

  static bool HighlightSelectionSourceIfNeeded(EditorShell& shell) {
    return shell.highlightSelectionSourceIfNeeded();
  }

  static void UpdateSourceFocusView(EditorShell& shell, bool scrollToSelection) {
    shell.updateSourceFocusView(scrollToSelection);
  }

  static void SetSourceFocusMode(EditorShell& shell, bool enabled) {
    shell.setSourceFocusMode(enabled);
  }

  static void SetSourcePaneVisible(EditorShell& shell, bool visible) {
    shell.setSourcePaneVisible(visible);
    shell.sourcePaneRevealProgress_ = visible ? 1.0f : 0.0f;
  }

  static void RevealSourceRange(EditorShell& shell, SourceByteRange byteRange) {
    shell.revealSourceRange(byteRange);
  }

  static void StageReadyStyleAnnotationsForCurrentSource(EditorShell& shell) {
    std::promise<DetachedStyleSourceAnnotations> promise;
    promise.set_value(DetachedStyleSourceAnnotations{.valid = true});
    shell.styleSourceAnnotationsFuture_ = promise.get_future();
    shell.styleSourceAnnotationPendingDocumentGeneration_ =
        shell.app_.document().documentGeneration();
    shell.styleSourceAnnotationPendingSourceVersion_ =
        shell.app_.document().document().sourceVersion();
    shell.styleSourceDecorationsValid_ = false;
  }

  static void UpdateSourceStyleDecorations(EditorShell& shell) {
    shell.updateSourceStyleDecorations();
  }

  static void UpdateSourceHoverPreview(EditorShell& shell) { shell.updateSourceHoverPreview(); }

  static void RefreshReferenceHighlightSummaryIfNeeded(EditorShell& shell) {
    shell.refreshReferenceHighlightSummaryIfNeeded();
  }

  static void ApplyReferenceHighlightPreview(EditorShell& shell) {
    shell.applyReferenceHighlightPreview();
  }

  static void SetReferenceHighlightChipHovered(EditorShell& shell, bool hovered) {
    shell.setReferenceHighlightChipHovered(hovered);
  }

  static void SetReferenceHighlightActive(EditorShell& shell, bool active) {
    shell.referenceHighlightActive_ = active;
  }

  static Box2d CanvasZoomControlRect(const EditorShell& shell) {
    return shell.canvasZoomControlScreenRect();
  }

  static void ConfigureViewport(EditorShell& shell, const Box2d& documentViewBox) {
    shell.interactionController_.updatePaneLayout(Vector2d(20.0, 30.0), Vector2d(400.0, 260.0),
                                                  documentViewBox);
    (void)shell.interactionController_.resetToActualSize();
  }

  static AsyncRenderer& BeginDelayedRender(EditorShell& shell, std::chrono::milliseconds delay) {
    AsyncRenderer& renderer = shell.renderCoordinator_.asyncRenderer();
    renderer.setReplayRenderDelayForTesting(delay);
    shell.renderCoordinator_.maybeRequestRender(
        shell.app_, shell.selectTool_, shell.interactionController_.viewport(), &shell.textures_);
    return renderer;
  }

  static void RefreshSelectionBoundsCache(EditorShell& shell) {
    shell.renderCoordinator_.refreshSelectionBoundsCache(shell.app_);
  }

  static bool FlushQueuedMutationAndRefreshOverlay(EditorShell& shell) {
    return shell.flushQueuedMutationAndRefreshOverlay();
  }

  static std::size_t DisplayedSelectionBoundsCount(const EditorShell& shell) {
    return shell.renderCoordinator_.selectionBoundsCache().displayedBoundsDoc.size();
  }

  static bool ImmediateChromePlanProduced(const EditorShell& shell) {
    return shell.immediateChromePlanProduced_;
  }

  static std::size_t SelectionChromePathCount(const EditorShell& shell) {
    const std::optional<SelectionChromeSnapshot>& snapshot =
        shell.renderCoordinator_.immediateOverlaySnapshot();
    return snapshot.has_value() ? snapshot->paths.size() : 0u;
  }

  static std::optional<Box2d> SelectionChromeFirstBoundsDoc(const EditorShell& shell) {
    const std::optional<SelectionChromeSnapshot>& snapshot =
        shell.renderCoordinator_.immediateOverlaySnapshot();
    return snapshot.has_value() && !snapshot->aabbsDoc.empty()
               ? std::optional<Box2d>(snapshot->aabbsDoc.front())
               : std::nullopt;
  }

  static std::uint64_t DisplayedDocVersion(const EditorShell& shell) {
    return shell.renderCoordinator_.displayedDocVersion();
  }

  static bool SamplePresentationPending(const EditorShell& shell) {
    return shell.samplePresentationPending_;
  }

  static std::string_view ActiveSampleId(const EditorShell& shell) { return shell.activeSampleId_; }

  static std::optional<std::uint64_t> ImmediateOverlayDocumentVersion(const EditorShell& shell) {
    return shell.renderCoordinator_.immediateOverlayDocumentVersionForDiagnostics();
  }

  static RenderCoordinator& Coordinator(EditorShell& shell) { return shell.renderCoordinator_; }

  static void HoldRenderResultsForPolls(EditorShell& shell, int polls) {
    shell.renderCoordinator_.asyncRenderer().setReplayResultHoldFramesForTesting(polls);
  }

  static std::vector<svg::SVGElement> ReferenceHighlightElements(const EditorShell& shell) {
    return shell.referenceHighlightElements();
  }

  static std::vector<svg::SVGElement> CombinedSourcePreviewElements(const EditorShell& shell) {
    return shell.combinedSourcePreviewElements();
  }

  static std::optional<StyleFocus> StyleFocusAtSourceOffset(EditorShell& shell,
                                                            std::size_t sourceOffset) {
    return shell.styleFocusAtSourceOffset(sourceOffset);
  }

  static std::optional<StyleFocus> StyleFocusAtSourceCursor(EditorShell& shell) {
    return shell.styleFocusAtSourceCursor();
  }

  static void ApplyPendingDocumentSpaceReplayInput(EditorShell& shell) {
    shell.applyPendingDocumentSpaceReplayInputForTesting();
  }

  static void ApplyReplayAction(EditorShell& shell, const repro::ReproAction& action) {
    shell.applyReplayActionForTesting(action);
  }

  static void ApplyStyleFocus(EditorShell& shell, StyleFocus styleFocus) {
    shell.applyStyleFocus(std::move(styleFocus));
  }

  static void ApplySourcePartition(EditorShell& shell, FocusPartition partition) {
    shell.applySourcePartition(std::move(partition));
  }

  static void OpenRenderPaneContextMenu(EditorShell& shell, const Vector2d& documentPoint) {
    shell.openRenderPaneContextMenu(documentPoint);
  }

  static void RenderRenderPaneContextMenu(EditorShell& shell) {
    shell.renderRenderPaneContextMenu();
  }

  static std::vector<SourceByteRange> SourceHoverRangesForElements(
      const EditorShell& shell, const std::vector<svg::SVGElement>& elements) {
    return shell.sourceHoverRangesForElements(elements);
  }

  static std::optional<Box2d> SelectionSizeChipScreenRect(EditorShell& shell,
                                                          std::string_view label,
                                                          const Vector2d& anchor) {
    return shell.selectionSizeChipScreenRect(
        label, anchor, shell.interactionController_.viewport().imageScreenRect());
  }

  static ImFont* SelectionChipFont(EditorShell& shell) {
    return shell.uiFontBold_ != nullptr ? shell.uiFontBold_ : ImGui::GetFont();
  }

  static std::optional<Box2d> ReferenceHighlightChipScreenRect(EditorShell& shell,
                                                               std::string_view label) {
    return shell.referenceHighlightChipScreenRect(label);
  }

  static std::optional<Vector2d> SelectionChipAnchorScreen(
      EditorShell& shell,
      std::optional<SelectTool::ActiveGesturePreview> activeGesturePreview = std::nullopt) {
    const std::optional<EditorShell::SelectionChipBounds> bounds =
        shell.selectionChipBounds(activeGesturePreview);
    if (!bounds.has_value()) {
      return std::nullopt;
    }

    return bounds->chipAnchorScreen;
  }

  static std::optional<Box2d> SelectionChipScreenBounds(
      EditorShell& shell,
      std::optional<SelectTool::ActiveGesturePreview> activeGesturePreview = std::nullopt) {
    const std::optional<EditorShell::SelectionChipBounds> bounds =
        shell.selectionChipBounds(activeGesturePreview);
    if (!bounds.has_value()) {
      return std::nullopt;
    }

    return bounds->screenBounds;
  }

  static Box2d ToolPaletteScreenRect(EditorShell& shell, const ImVec2& paneOrigin,
                                     const ImVec2& contentRegion) {
    return shell.toolPaletteScreenRect(paneOrigin, contentRegion);
  }

  static bool CanvasHasSelectableElements(EditorShell& shell) {
    return shell.canvasHasSelectableElements();
  }

  static void SelectAllCanvasElements(EditorShell& shell) { shell.selectAllCanvasElements(); }

  static bool SelectionIsAllText(const EditorShell& shell) { return shell.selectionIsAllText(); }

  static void ConvertSelectedTextToOutlines(EditorShell& shell) {
    shell.convertSelectedTextToOutlines();
  }

  static const std::string& LastConvertTextError(const EditorShell& shell) {
    return shell.lastConvertTextError_;
  }

  static void RenderSourcePane(EditorShell& shell, float paneOriginY, float paneHeight,
                               float paneWidth, ImFont* codeFont) {
    shell.renderSourcePane(/*paneOriginX=*/0.0f, paneOriginY, paneHeight, paneWidth, codeFont);
  }

  static void RenderRenderPane(EditorShell& shell, const Vector2d& renderPaneOrigin,
                               const Vector2d& renderPaneSize, ImGuiWindowFlags paneFlags) {
    // The render pane now docks itself; outside a DockSpace it Begins as a
    // floating "Render" window, so position it explicitly for the test frame.
    ImGui::SetNextWindowPos(
        ImVec2(static_cast<float>(renderPaneOrigin.x), static_cast<float>(renderPaneOrigin.y)),
        ImGuiCond_Always);
    ImGui::SetNextWindowSize(
        ImVec2(static_cast<float>(renderPaneSize.x), static_cast<float>(renderPaneSize.y)),
        ImGuiCond_Always);
    shell.renderRenderPane(paneFlags | ImGuiWindowFlags_NoSavedSettings);
  }

  static void RenderFillStrokeToolbarWidget(EditorShell& shell) {
    shell.renderFillStrokeToolbarWidget();
  }

  static std::uint64_t ToolbarLiveSelectionIdentityReads(const EditorShell& shell) {
    return shell.toolbarLiveSelectionIdentityReadsForTesting_;
  }

  static void RenderToolPalette(EditorShell& shell, const ImVec2& paneOrigin,
                                const ImVec2& contentRegion) {
    shell.renderToolPalette(paneOrigin, contentRegion);
  }

  static void RenderSourcePaneSplitter(EditorShell& shell, float windowWidth, float paneOriginY,
                                       float paneHeight, float sourcePaneWidth) {
    shell.renderSourcePaneSplitter(windowWidth, paneOriginY, paneHeight, sourcePaneWidth);
  }

  static void RenderLayerPanelContents(EditorShell& shell) { shell.renderLayerPanelContents(); }

  static void RenderEditingScopeBreadcrumb(EditorShell& shell) {
    shell.renderEditingScopeBreadcrumb();
  }

  static void RenderReferenceHighlightChip(EditorShell& shell) {
    shell.renderReferenceHighlightChip();
  }

  static void RenderSelectionSizeChip(
      EditorShell& shell, const SelectionTransformHandleIntent& hoverTransformIntent,
      const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview) {
    shell.renderSelectionSizeChip(hoverTransformIntent, activeGesturePreview);
  }

  static void ApplyMenuActions(EditorShell& shell, const MenuBarActions& actions) {
    shell.applyMenuActions(actions);
  }

  static void ApplyMenuHistoryActions(EditorShell& shell, const MenuBarActions& actions,
                                      bool sourcePaneFocused) {
    shell.applyMenuHistoryActions(actions, sourcePaneFocused);
  }

  static void UseInMemoryShapeClipboard(EditorShell& shell) {
    shell.shapeClipboard_ = std::make_unique<InMemoryClipboard>();
  }

  static void ClearShapeClipboard(EditorShell& shell) { shell.shapeClipboard_.reset(); }

  static void SetShapeClipboardText(EditorShell& shell, std::string_view text) {
    ASSERT_NE(shell.shapeClipboard_, nullptr);
    shell.shapeClipboard_->setText(text);
  }

  static std::string ShapeClipboardText(const EditorShell& shell) {
    return shell.shapeClipboard_ != nullptr ? shell.shapeClipboard_->getText() : std::string();
  }

  static bool ShapeClipboardHasText(const EditorShell& shell) {
    return shell.shapeClipboard_ != nullptr && shell.shapeClipboard_->hasText();
  }

  static void CopySelectedShapesToClipboard(EditorShell& shell) {
    shell.copySelectedShapesToClipboard();
  }

  static void CutSelectedShapesToClipboard(EditorShell& shell) {
    shell.cutSelectedShapesToClipboard();
  }

  static void PasteShapesFromClipboard(EditorShell& shell, bool inFront) {
    shell.pasteShapesFromClipboard(inFront);
  }

  static void HandleGlobalShortcuts(EditorShell& shell) {
    // runFrame refreshes these immutable UI snapshots immediately before shortcut dispatch. Tests
    // that exercise the private dispatcher directly must establish the same idle-frame precondition
    // instead of observing the constructor defaults.
    shell.cachedCanvasHasSelectableElements_ = shell.canvasHasSelectableElements();
    shell.cachedSelectionIsAllText_ = shell.selectionIsAllText();
    shell.handleGlobalShortcuts();
  }

  static bool ActiveToolIsSelect(const EditorShell& shell) {
    return shell.activeTool_ == EditorShell::ActiveTool::Select;
  }

  static bool ActiveToolIsPen(const EditorShell& shell) {
    return shell.activeTool_ == EditorShell::ActiveTool::Pen;
  }

  static bool ActiveToolIsText(const EditorShell& shell) {
    return shell.activeTool_ == EditorShell::ActiveTool::Text;
  }

  static bool ActiveToolIsEyedropper(const EditorShell& shell) {
    return shell.activeTool_ == EditorShell::ActiveTool::Eyedropper;
  }

  static bool ArmEyedropper(EditorShell& shell, bool stroke) {
    return shell.armEyedropper(stroke ? EditorShell::PaintTarget::Stroke
                                      : EditorShell::PaintTarget::Fill);
  }

  static void CancelEyedropper(EditorShell& shell, bool restorePreviousTool) {
    shell.cancelEyedropper(restorePreviousTool);
  }

  static void ApplySampledColor(EditorShell& shell, bool stroke, const css::RGBA& color) {
    shell.applyPaintColor(
        stroke ? EditorShell::PaintTarget::Stroke : EditorShell::PaintTarget::Fill, color,
        /*recordUndo=*/true);
  }

  static bool EyedropperTargetsStroke(const EditorShell& shell) {
    return shell.eyedropperTarget_ == EditorShell::PaintTarget::Stroke;
  }

  static bool ActivePaintTargetIsStroke(const EditorShell& shell) {
    return shell.activePaintTarget_ == EditorShell::PaintTarget::Stroke;
  }

  static bool EyedropperCaptureEnabled(const EditorShell& shell) {
    return shell.renderCoordinator_.documentPixelCaptureEnabled();
  }

  static void RestartEyedropperCapture(EditorShell& shell) {
    shell.renderCoordinator_.setDocumentPixelCaptureEnabled(false);
    shell.renderCoordinator_.setDocumentPixelCaptureEnabled(true);
  }

  static bool EyedropperCaptureRequestPending(const EditorShell& shell) {
    return shell.renderCoordinator_.requestedPixelCapture_.has_value();
  }

  static void SetExpiredEyedropperCanvasCommitWake(EditorShell& shell) {
    shell.renderCoordinator_.pixelCaptureCanvasCommitDue_ =
        shell.renderCoordinator_.schedulingNow() - std::chrono::milliseconds(1);
  }

  static void ExpireViewportSettleWake(EditorShell& shell) {
    shell.viewportInitialized_ = true;
    shell.showSamplePicker_ = false;
    shell.samplePresentationPending_ = false;
    shell.renderCoordinator_.viewportSettleDue_ =
        shell.renderCoordinator_.schedulingNow() - std::chrono::milliseconds(1);
  }

  static void QueueIdleRenderRefresh(EditorShell& shell) { shell.queueIdleRenderRefreshIfNeeded(); }

  static std::optional<float> ViewportRenderWake(EditorShell& shell) {
    return shell.nextViewportRenderWakeSeconds();
  }

  static bool EyedropperCanvasCommitWakePending(const EditorShell& shell) {
    return shell.renderCoordinator_.pixelCaptureCanvasCommitDue_.has_value();
  }

  static const DocumentPixelCapture* PixelCapture(const EditorShell& shell) {
    return shell.renderCoordinator_.documentPixelCaptureFor(shell.app_,
                                                            shell.viewportForReadback());
  }

  static bool TextToolIsEditing(const EditorShell& shell) { return shell.textTool_.isEditing(); }

  static bool FormatBarShouldShow(const EditorShell& shell) { return shell.formatBarShouldShow(); }

  static FormatBarState ComputeFormatBarState(EditorShell& shell) {
    return shell.computeFormatBarState();
  }

  static void ApplyFormatBarActions(EditorShell& shell, const FormatBarState& state,
                                    const FormatBarActions& actions) {
    shell.applyFormatBarActions(state, actions);
  }

  static bool CommitTextEditing(EditorShell& shell) { return shell.textTool_.commit(shell.app_); }

  static void ToggleTextBold(EditorShell& shell) { shell.textTool_.toggleBold(shell.app_); }
  static void ToggleTextItalic(EditorShell& shell) { shell.textTool_.toggleItalic(shell.app_); }
  static void ToggleTextUnderline(EditorShell& shell) {
    shell.textTool_.toggleUnderline(shell.app_);
  }

  static std::size_t TextToolCaretIndex(const EditorShell& shell) {
    return shell.textTool_.caretIndex();
  }

  static SelectionChromeDetail SelectionChromeDetailForActiveTool(const EditorShell& shell) {
    return shell.selectionChromeDetailForActiveTool();
  }

  static bool PenToolIsDrafting(const EditorShell& shell) { return shell.penTool_.isDrafting(); }

  static std::shared_ptr<const Path> PenPreviewPath(const EditorShell& shell) {
    return shell.penTool_.previewPath();
  }

  static bool PenToolIsDraggingAnchor(const EditorShell& shell) {
    return shell.penTool_.isDraggingAnchor();
  }

  static const std::string& PenToolActivePathData(const EditorShell& shell) {
    return shell.penTool_.activePathData();
  }

  static bool PenDragFlushedThisFrame(const EditorShell& shell) {
    return shell.penDragFlushedThisFrame_;
  }

  static bool SelectToolIsMarqueeing(const EditorShell& shell) {
    return shell.selectTool_.isMarqueeing();
  }

  static bool BeginSelectedShapeDrag(EditorShell& shell, const Vector2d& documentPoint,
                                     const Box2d& selectionBounds) {
    const std::array<Box2d, 1> bounds = {selectionBounds};
    return shell.selectTool_.tryStartRedragOnSelected(shell.app_, documentPoint, MouseModifiers{},
                                                      bounds);
  }

  static bool MoveSelectedShapeDrag(EditorShell& shell, const Vector2d& documentPoint) {
    shell.selectTool_.onMouseMove(shell.app_, documentPoint, /*buttonHeld=*/true);
    return shell.flushInteractiveDragMutationAndRequestRender();
  }

  static void UpdateSelectionDrag(EditorShell& shell) {
    shell.updateRenderPaneSelectionDrag(false,
                                        shell.interactionController_.viewport().pixelsPerDocUnit());
  }

  static void EndSelectedShapeDrag(EditorShell& shell, const Vector2d& documentPoint) {
    shell.selectTool_.onMouseUp(shell.app_, documentPoint);
  }

  static void BufferPendingClick(EditorShell& shell, const Vector2d& documentPoint,
                                 MouseModifiers modifiers = MouseModifiers{}) {
    shell.interactionController_.bufferPendingClick(documentPoint, modifiers);
  }

  static bool HasPendingClick(const EditorShell& shell) {
    return shell.interactionController_.pendingClick().has_value();
  }

  static bool RendererBusy(const EditorShell& shell) {
    return shell.renderCoordinator_.asyncRenderer().isBusy();
  }

  static int RenderRequestsPosted(const EditorShell& shell) {
    return shell.renderCoordinator_.lastFrameCostBreakdown().renderRequestsPosted;
  }

  static void SetPendingSelectClickStartSeconds(EditorShell& shell, double seconds) {
    shell.pendingSelectClickStartSeconds_ = seconds;
  }

  static bool RequestRenderAtEndOfFrame(const EditorShell& shell) {
    return shell.requestRenderAtEndOfFrame_;
  }

  static void ClearRequestRenderAtEndOfFrame(EditorShell& shell) {
    shell.requestRenderAtEndOfFrame_ = false;
  }

  static void ApplyDeferredRenderRequest(EditorShell& shell) { shell.applyDeferredRenderRequest(); }

  static void SetRequestRenderAtEndOfFrame(EditorShell& shell) {
    shell.requestRenderAtEndOfFrame_ = true;
  }

  static void RenderSidebars(EditorShell& shell) { shell.renderSidebars(); }

  static std::size_t VisibleLayerRowCount(const EditorShell& shell) {
    return shell.layersPanel_.visibleRowCount();
  }

  static CompositorDebugPanel& DiagnosticPanel(EditorShell& shell) {
    return shell.compositorDebugPanel_;
  }

  static void SetShowCompositorDebugPanel(EditorShell& shell, bool value) {
    shell.showCompositorDebugPanel_ = value;
  }

  static bool ShowCompositorDebugPanel(const EditorShell& shell) {
    return shell.showCompositorDebugPanel_;
  }

  static PerfOverlayMode GetPerfOverlayMode(const EditorShell& shell) {
    return shell.perfOverlayMode_;
  }

  static void SetDockLayoutLocked(EditorShell& shell, bool value) {
    shell.dockLayoutLocked_ = value;
  }

  static bool DockLayoutLocked(const EditorShell& shell) { return shell.dockLayoutLocked_; }

  static void RequestDockLayoutReset(EditorShell& shell) { shell.dockLayoutResetRequested_ = true; }

  static bool DockLayoutResetRequested(const EditorShell& shell) {
    return shell.dockLayoutResetRequested_;
  }

  static const EditorAdaptiveUiLayout& AdaptiveUiLayout(const EditorShell& shell) {
    return shell.adaptiveUiLayout_;
  }

  static void SetAdaptiveUiLayout(EditorShell& shell, const EditorAdaptiveUiLayout& layout) {
    shell.adaptiveUiLayout_ = layout;
  }

  static void SetCompactPanelVisible(EditorShell& shell, bool visible) {
    shell.compactPanelVisible_ = visible;
  }

  static bool CompactPanelVisible(const EditorShell& shell) { return shell.compactPanelVisible_; }

  static bool CompactInspectorSheet(const EditorShell& shell) {
    return shell.compactInspectorSheet_;
  }

  static bool SourcePaneVisible(const EditorShell& shell) { return shell.sourcePaneVisible_; }
  static std::optional<Box2d> LoadFailureRevealButtonRect(const EditorShell& shell) {
    return shell.loadFailureRevealButtonRect_;
  }
  static float SourcePaneWidth(const EditorShell& shell) { return shell.sourcePaneWidth_; }
  static void SetSourcePaneWidth(EditorShell& shell, float value) {
    shell.sourcePaneWidth_ = value;
  }
  static bool SourceFocusMode(const EditorShell& shell) { return shell.sourceFocusMode_; }
  static bool SourceFocusOriginatedInStyle(const EditorShell& shell) {
    return shell.sourceFocusOriginatedInStyle_;
  }
  static bool SourceSelectionOriginatedInText(const EditorShell& shell) {
    return shell.sourceSelectionOriginatedInText_;
  }
  static void SetSourceSelectionOriginatedInText(EditorShell& shell, bool value) {
    shell.sourceSelectionOriginatedInText_ = value;
  }
  static bool PreserveSourceEditFocusCursor(const EditorShell& shell) {
    return shell.preserveSourceEditFocusCursor_;
  }
  static void SetPreserveSourceEditFocusCursor(EditorShell& shell, bool value) {
    shell.preserveSourceEditFocusCursor_ = value;
  }
  static const ReferenceHighlightSummary& ReferenceSummary(const EditorShell& shell) {
    return shell.referenceHighlightSummary_;
  }
  static bool ReferenceHighlightActive(const EditorShell& shell) {
    return shell.referenceHighlightActive_;
  }
  static bool ReferenceHighlightChipHovered(const EditorShell& shell) {
    return shell.referenceHighlightChipHovered_;
  }
  static bool StyleSourceDecorationsValid(const EditorShell& shell) {
    return shell.styleSourceDecorationsValid_;
  }
  static std::size_t StyleSourceContributionCount(const EditorShell& shell) {
    return shell.styleSourceContributions_.size();
  }
  static std::uint64_t StyleSourceDecorationDocumentGeneration(const EditorShell& shell) {
    return shell.styleSourceDecorationDocumentGeneration_;
  }
  static std::optional<Vector2d> RenderContextMenuDocumentPoint(const EditorShell& shell) {
    return shell.renderContextMenuDocumentPoint_;
  }
  static bool RenderContextMenuOpenRequested(const EditorShell& shell) {
    return shell.renderContextMenuOpenRequested_;
  }
  static bool RenderContextMenuHasHitElement(const EditorShell& shell) {
    return shell.renderContextMenuHitElement_.has_value();
  }
  static std::size_t LastReferenceHighlightSelectionSize(const EditorShell& shell) {
    return shell.lastReferenceHighlightSelection_.size();
  }
  static void NoteFrameDelta(EditorShell& shell, float deltaMs) {
    shell.interactionController_.noteFrameDelta(deltaMs);
  }
  static std::uint64_t ResourceDiagnosticsFrame(const EditorShell& shell) {
    return shell.resourceDiagnosticsFrame_;
  }
  static bool FrameMissTelemetryWriteErrorLogged(const EditorShell& shell) {
    return shell.frameMissTelemetryWriteErrorLogged_;
  }

#ifdef DONNER_EDITOR_WGPU
  static std::uint64_t UploadPresentationBitmapDeviceId(EditorShell& shell) {
    svg::RendererBitmap bitmap;
    bitmap.dimensions = Vector2i(1, 1);
    bitmap.rowBytes = 4u;
    bitmap.pixels = {0x11u, 0x22u, 0x33u, 0xFFu};
    RenderResult::CompositedTile tile;
    tile.id = "ui-device-probe";
    tile.kind = RenderResult::CompositedTile::Kind::Immediate;
    tile.generation = 1;
    tile.bitmapDimsPx = bitmap.dimensions;
    tile.rasterCanvasSize = bitmap.dimensions;
    tile.bitmapDimsDoc = Vector2d(1.0, 1.0);
    tile.bitmap = std::move(bitmap);
    RenderResult::CompositedPreview preview;
    preview.tiles.push_back(std::move(tile));
    shell.textures_.uploadComposited(preview);
    if (shell.textures_.tiles().size() != 1u ||
        shell.textures_.tiles().front().textureSnapshot == nullptr) {
      return 0;
    }
    const auto* snapshot = static_cast<const svg::RendererGeodeTextureSnapshot*>(
        shell.textures_.tiles().front().textureSnapshot.get());
    return snapshot->deviceId();
  }

  static GlTextureCache& ThumbnailTextures(EditorShell& shell) { return shell.thumbnailTextures_; }

  static std::shared_ptr<const svg::RendererTextureSnapshot> RenderProbeThumbnail(
      EditorShell& shell) {
    const auto element = shell.app_.document().document().querySelector("#background");
    return element ? shell.layerThumbnailRenderer_.renderElement(*element, Vector2i(8, 8))
                         .textureSnapshot()
                   : nullptr;
  }

  static std::uint64_t RenderLayerThumbnailDeviceId(EditorShell& shell) {
    const std::optional<svg::SVGElement> element =
        shell.app_.document().document().querySelector("#background");
    if (!element.has_value()) {
      return 0;
    }
    const svg::RendererImage image =
        shell.layerThumbnailRenderer_.renderElement(*element, Vector2i(8, 8));
    if (image.textureSnapshot() == nullptr) {
      return 0;
    }
    const auto* snapshot =
        static_cast<const svg::RendererGeodeTextureSnapshot*>(image.textureSnapshot().get());
    return snapshot->deviceId();
  }
#endif
};

void RunShellFrame(gui::EditorWindow& window, EditorShell& shell) {
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
}

void RunFramesUntilDisplayedSelectionBounds(gui::EditorWindow& window, EditorShell& shell) {
  for (const tests::HangGuard guard; guard.active();) {
    RunShellFrame(window, shell);
    if (EditorShellTestAccess::DisplayedSelectionBoundsCount(shell) > 0u) {
      return;
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

/// Hold raster results while a live style edit advances the document.
/// @param window Hidden window driving frames.
/// @param shell Shell whose target is already selected.
/// @return Version of the retained raster/geometry pair.
std::uint64_t HoldRasterWhileLiveDocumentAdvances(gui::EditorWindow& window, EditorShell& shell) {
  RunFramesUntilDisplayedSelectionBounds(window, shell);
  EditorShellTestAccess::HoldRenderResultsForPolls(shell, 64);

  const std::uint64_t presentedVersion = EditorShellTestAccess::DisplayedDocVersion(shell);
  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "pen",
                                           });
  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetStyleProperty,
                                               .propertyName = "fill",
                                               .propertyValue = "#010203",
                                           });
  (void)EditorShellTestAccess::App(shell).flushFrame();
  RunShellFrame(window, shell);
  return presentedVersion;
}

bool WaitForStyleSourceDecorations(EditorShell& shell) {
  const tests::HangGuard guard;
  do {
    EditorShellTestAccess::UpdateSourceStyleDecorations(shell);
    if (EditorShellTestAccess::StyleSourceDecorationsValid(shell)) {
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  } while (guard.active());

  return false;
}

ImGuiWindow* FindSourceEditorWindow() {
  for (ImGuiWindow* window : ImGui::GetCurrentContext()->Windows) {
    if (std::string_view(window->Name).find("Source/##source") != std::string_view::npos) {
      return window;
    }
  }
  return nullptr;
}

float SourceEditorContentHeight() {
  const ImGuiWindow* window = FindSourceEditorWindow();
  return window != nullptr ? window->DC.CursorMaxPos.y - window->DC.CursorStartPos.y : 0.0f;
}

void DriveGlobalShortcut(EditorShell& shell, const std::vector<ImGuiKey>& keys, bool ctrl = false,
                         bool shift = false, bool super = false, bool textInputActive = false,
                         const std::function<void()>& renderUi = {}) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.ConfigMacOSXBehaviors = false;
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  if (ctrl) {
    io.AddKeyEvent(ImGuiMod_Ctrl, true);
  }
  if (shift) {
    io.AddKeyEvent(ImGuiMod_Shift, true);
  }
  if (super) {
    io.AddKeyEvent(ImGuiMod_Super, true);
  }
  for (ImGuiKey key : keys) {
    io.AddKeyEvent(key, true);
  }

  ImGui::NewFrame();
  if (renderUi) {
    renderUi();
  }
  if (textInputActive) {
    io.WantTextInput = true;
  }
  EditorShellTestAccess::HandleGlobalShortcuts(shell);
  ImGui::Render();

  for (ImGuiKey key : keys) {
    io.AddKeyEvent(key, false);
  }
  if (super) {
    io.AddKeyEvent(ImGuiMod_Super, false);
  }
  if (shift) {
    io.AddKeyEvent(ImGuiMod_Shift, false);
  }
  if (ctrl) {
    io.AddKeyEvent(ImGuiMod_Ctrl, false);
  }
  ImGui::NewFrame();
  if (renderUi) {
    renderUi();
  }
  ImGui::Render();
}

void RenderToolbarFrame(gui::EditorWindow& window, EditorShell& shell, const ImVec2& cursor,
                        const ImVec2& mouse, bool mouseDown) {
  (void)window;
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);

  ImGui::NewFrame();
  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(220.0f, 100.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellToolbarMouseHost", nullptr, kHostFlags);
  ImGui::SetCursorScreenPos(cursor);
  EditorShellTestAccess::RenderFillStrokeToolbarWidget(shell);
  ImGui::End();
  ImGui::Render();
}

bool DrawDataContainsColor(ImU32 color) {
  const ImDrawData* drawData = ImGui::GetDrawData();
  if (drawData == nullptr) {
    return false;
  }
  for (int listIndex = 0; listIndex < drawData->CmdListsCount; ++listIndex) {
    const ImDrawList* drawList = drawData->CmdLists[listIndex];
    for (const ImDrawVert& vertex : drawList->VtxBuffer) {
      if (vertex.col == color) {
        return true;
      }
    }
  }
  return false;
}

void ClickToolbar(gui::EditorWindow& window, EditorShell& shell, const ImVec2& cursor,
                  const ImVec2& mouse) {
  RenderToolbarFrame(window, shell, cursor, mouse, /*mouseDown=*/false);
  RenderToolbarFrame(window, shell, cursor, mouse, /*mouseDown=*/true);
  RenderToolbarFrame(window, shell, cursor, mouse, /*mouseDown=*/false);
}

std::optional<ImVec2> CurrentPopupFirstButtonCenter() {
  ImGuiContext* context = ImGui::GetCurrentContext();
  if (context == nullptr || context->OpenPopupStack.empty() ||
      context->OpenPopupStack.back().Window == nullptr) {
    return std::nullopt;
  }
  const ImGuiWindow* popup = context->OpenPopupStack.back().Window;
  return ImVec2(popup->DC.CursorStartPos.x + 45.0f,
                popup->DC.CursorStartPos.y + ImGui::GetFrameHeight() * 0.5f);
}

void RenderToolPaletteFrame(gui::EditorWindow& window, EditorShell& shell, const ImVec2& paneOrigin,
                            const ImVec2& contentRegion, const ImVec2& mouse, bool mouseDown) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);

  window.beginFrame();
  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellToolPaletteMouseHost", nullptr, kHostFlags);
  EditorShellTestAccess::RenderToolPalette(shell, paneOrigin, contentRegion);
  ImGui::End();
  window.endFrame();
}

void RenderPaneMouseFrame(gui::EditorWindow& window, EditorShell& shell,
                          const Vector2d& documentPoint, bool mouseDown,
                          const Vector2d& renderPaneOrigin = Vector2d(20.0, 30.0),
                          const Vector2d& renderPaneSize = Vector2d(400.0, 260.0),
                          bool shift = false, bool option = false, bool command = false,
                          bool doubleClick = false) {
  const Vector2d screenPoint = shell.viewportForReadback().documentToScreen(documentPoint);

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.AddKeyEvent(ImGuiMod_Shift, shift);
  io.AddKeyEvent(ImGuiMod_Alt, option);
  io.AddKeyEvent(ImGuiMod_Ctrl, command);
  io.AddMousePosEvent(static_cast<float>(screenPoint.x), static_cast<float>(screenPoint.y));
  io.AddMouseButtonEvent(0, mouseDown);

  window.beginFrame();
  if (doubleClick) {
    io.MouseClickedCount[0] = 2;
  }
  EditorShellTestAccess::RenderRenderPane(
      shell, renderPaneOrigin, renderPaneSize,
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
  window.endFrame();

  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  io.AddKeyEvent(ImGuiMod_Alt, false);
  io.AddKeyEvent(ImGuiMod_Shift, false);
}

void RenderContextMenuFrame(gui::EditorWindow& window, EditorShell& shell) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }

  window.beginFrame();
  ImGui::SetNextWindowPos(ImVec2(20.0f, 20.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(220.0f, 120.0f), ImGuiCond_Always);
  ImGui::Begin(
      "EditorShellContextMenuHost", nullptr,
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
  EditorShellTestAccess::RenderRenderPaneContextMenu(shell);
  ImGui::End();
  window.endFrame();
}

Box2d RenderSourcePaneSplitterFrame(gui::EditorWindow& window, EditorShell& shell,
                                    float sourcePaneWidth, const ImVec2& mouse, bool mouseDown,
                                    float windowWidth = 640.0f) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);

  window.beginFrame();
  EditorShellTestAccess::RenderSourcePaneSplitter(shell, windowWidth, /*paneOriginY=*/0.0f,
                                                  /*paneHeight=*/220.0f, sourcePaneWidth);
  const ImVec2 itemMin = ImGui::GetItemRectMin();
  const ImVec2 itemMax = ImGui::GetItemRectMax();
  window.endFrame();
  return Box2d(Vector2d(itemMin.x, itemMin.y), Vector2d(itemMax.x, itemMax.y));
}

void RenderReferenceHighlightChipFrame(gui::EditorWindow& window, EditorShell& shell,
                                       const ImVec2& mouse, bool mouseDown) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);

  window.beginFrame();
  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellReferenceChipHost", nullptr, kHostFlags);
  EditorShellTestAccess::RenderReferenceHighlightChip(shell);
  ImGui::End();
  window.endFrame();
}

struct EditingScopeBreadcrumbItem {
  ImRect rect;
  ImGuiID id;
  ImGuiID expectedLabelId;
  bool hovered;
};

std::optional<EditingScopeBreadcrumbItem> RenderEditingScopeBreadcrumbFrame(
    gui::EditorWindow& window, EditorShell& shell, const char* expectedLabel,
    const ImVec2& mouse = ImVec2(-100.0f, -100.0f), bool mouseDown = false) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);

  window.beginFrame();
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Always);
  ImGui::Begin(
      "EditorShellBreadcrumbHost", nullptr,
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
  // A zero-ID item makes the no-scope path distinguishable from a submitted button.
  ImGui::Dummy(ImVec2(1.0f, 1.0f));
  EditorShellTestAccess::RenderEditingScopeBreadcrumb(shell);
  std::optional<EditingScopeBreadcrumbItem> item;
  if (ImGui::GetCurrentContext()->LastItemData.ID != 0) {
    item = EditingScopeBreadcrumbItem{ImRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax()),
                                      ImGui::GetCurrentContext()->LastItemData.ID,
                                      ImGui::GetID(expectedLabel), ImGui::IsItemHovered()};
  }
  ImGui::End();
  window.endFrame();
  return item;
}

void RenderSelectionSizeChipFrame(
    gui::EditorWindow& window, EditorShell& shell,
    const SelectionTransformHandleIntent& hoverTransformIntent,
    const std::optional<SelectTool::ActiveGesturePreview>& activeGesturePreview) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }

  window.beginFrame();
  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(640.0f, 480.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellSelectionChipHost", nullptr, kHostFlags);
  EditorShellTestAccess::RenderSelectionSizeChip(shell, hoverTransformIntent, activeGesturePreview);
  ImGui::End();
  window.endFrame();
}

void RunHiddenWindowShellConstructsAndRunsFrames(double contentScale) {
  gui::EditorWindow window = MakeHiddenWindow(contentScale);
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  ExpectTestWindowScale(window, Vector2i(640, 480), contentScale);

  {
    EditorShell emptyShell(window, EditorShellOptions{});
    EXPECT_FALSE(emptyShell.valid());
  }

  EditorShellOptions options;
  options.initialSource = std::string(kInitialSvg);
  options.initialPath = "memory.svg";
  options.editorNoticeText = "test notice";
  EditorShell shell(window, std::move(options));

  ASSERT_TRUE(shell.valid());
  EXPECT_FALSE(shell.selectedElementLabelForReadback().has_value());

  window.beginFrame();
  shell.runFrame();
  window.endFrame();

  const EditorAdaptiveUiLayout& adaptiveLayout = EditorShellTestAccess::AdaptiveUiLayout(shell);
  EXPECT_TRUE(adaptiveLayout.compactTouch());
  EXPECT_FLOAT_EQ(adaptiveLayout.topBarHeight, 52.0f);
  EXPECT_FLOAT_EQ(adaptiveLayout.toolButtonSize, 44.0f);
  EXPECT_FALSE(adaptiveLayout.showPaintControls);
  EXPECT_FALSE(adaptiveLayout.showTextFormatBar);
  EXPECT_FALSE(adaptiveLayout.showCanvasScrollbars);

  const LayerInspectorStatusReadback status = shell.layerInspectorStatusForReadback();
  EXPECT_GE(status.viewportDesiredCanvas.x, 0);
  EXPECT_GE(status.viewportDesiredCanvas.y, 0);
  EXPECT_FALSE(shell.selectedElementLabelForReadback().has_value());

  shell.setContentOnlyCaptureForNextFrameForReplay(true);
  shell.overrideViewportForReplay(shell.viewportForReadback());

  window.beginFrame();
  shell.runFrame();
  window.endFrame();

  EXPECT_TRUE(shell.valid());
}

TEST(EditorShellTest, AnimationPreviewWaitsForPresentationAndRestartsWithoutEdits) {
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  EditorApp& app = EditorShellTestAccess::App(shell);
  const std::string source(app.document().document().source());
  const bool dirty = app.isDirty();
  const bool canUndo = app.canUndo();

  EditorShellTestAccess::SetAnimationPreview(shell, true);
  EXPECT_TRUE(EditorShellTestAccess::IsAnimationPreview(shell));
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 10.0);
  const std::uint64_t initialVersion = app.document().currentFrameVersion();
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 10.008);
  EXPECT_EQ(app.document().currentFrameVersion(), initialVersion);
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 10.017);
  EXPECT_DOUBLE_EQ(app.document().document().currentTime(), 0.0);
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 10.5);
  EXPECT_DOUBLE_EQ(app.document().document().currentTime(), 0.0);

  EditorShellTestAccess::SetAnimationPreview(shell, false);
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 11.0);
  EXPECT_DOUBLE_EQ(app.document().document().currentTime(), 0.0);
  const std::uint64_t stoppedVersion = app.document().currentFrameVersion();
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 20.0);
  EXPECT_EQ(app.document().currentFrameVersion(), stoppedVersion);
  EditorShellTestAccess::SetAnimationPreview(shell, true);
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 20.0);
  EXPECT_DOUBLE_EQ(app.document().document().currentTime(), 0.0);
  EXPECT_EQ(app.document().document().source(), source);
  EXPECT_EQ(app.isDirty(), dirty);
  EXPECT_EQ(app.canUndo(), canUndo);
}

TEST(EditorShellTest, AnimationPreviewEventFrameDoesNotStartAnotherOrdinaryTick) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
      <rect width="8" height="8"><animate attributeName="x" from="0" to="16" dur="2s"/></rect>
    </svg>)svg";
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(source));
  auto& document = EditorShellTestAccess::App(shell).document();
  EditorShellTestAccess::SetAnimationPreview(shell, true);
  const tests::HangGuard guard;
  while (!EditorShellTestAccess::PreviewInputReady(shell) && guard.active()) {
    if (!EditorShellTestAccess::AnimationResetPending(shell)) {
      EditorShellTestAccess::HoldAnimationAtStart(shell);
    }
    RunShellFrame(window, shell);
  }
  ASSERT_TRUE(EditorShellTestAccess::PreviewInputReady(shell));
  ASSERT_DOUBLE_EQ(document.document().currentTime(), 0.0);
  EditorShellTestAccess::SetPreviewStart(shell, 10.0);
  const auto version = document.currentFrameVersion();
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 10.25, EditorFrameKind::Event);
  EXPECT_DOUBLE_EQ(document.document().currentTime(), 0.0);
  EXPECT_EQ(document.currentFrameVersion(), version);
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 10.25, EditorFrameKind::Scheduled);
  EXPECT_DOUBLE_EQ(document.document().currentTime(), 0.25);
  EXPECT_GT(document.currentFrameVersion(), version);

  EditorShellTestAccess::SetAnimationPreview(shell, false);
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 11.0, EditorFrameKind::Event);
  EXPECT_DOUBLE_EQ(document.document().currentTime(), 0.0);
  EXPECT_FALSE(EditorShellTestAccess::AnimationResetPending(shell));
  EXPECT_EQ(document.document().source(), source);
}

TEST(EditorShellTest, AnimationPreviewHeaderRespondsToMouse) {
  gui::EditorWindow window(TestWindowOptions(
      {.title = "Animation header", .initialWidth = 1600, .initialHeight = 900}, 1.0));
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  RunShellFrame(window, shell);
  RunShellFrame(window, shell);
  const Box2d rect = EditorShellTestAccess::AnimationControlRect(shell);
  ASSERT_GT(rect.width(), 0.0);
  const ImGuiWindow* layers = ImGui::FindWindowByName(kLayersWindowName);
  ASSERT_NE(layers, nullptr);
  EXPECT_LE(rect.bottomRight.y, layers->Pos.y);
  const ImVec2 previewPoint(static_cast<float>(rect.topLeft.x + rect.width() * 0.75),
                            static_cast<float>(rect.topLeft.y + rect.height() * 0.5));
  for (const bool down : {false, true, false}) {
    ImGui::GetIO().AddMousePosEvent(previewPoint.x, previewPoint.y);
    ImGui::GetIO().AddMouseButtonEvent(0, down);
    RunShellFrame(window, shell);
  }
  EXPECT_TRUE(EditorShellTestAccess::IsAnimationPreview(shell));
}

TEST(EditorShellTest, AnimationPreviewPresentsSuccessiveFrames) {
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  const EditorSample* sample = FindEditorSample("animation");
  ASSERT_NE(sample, nullptr);
  EditorShell shell(window, OptionsWithSource(sample->source));
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (EditorShellTestAccess::Coordinator(shell).framePresentation() == nullptr &&
         std::chrono::steady_clock::now() < deadline) {
    RunShellFrame(window, shell);
  }
  ASSERT_NE(EditorShellTestAccess::Coordinator(shell).framePresentation(), nullptr);
  const auto initialVersion =
      EditorShellTestAccess::Coordinator(shell).framePresentation()->identity().version;
  EditorShellTestAccess::SetAnimationPreview(shell, true);
  std::set<std::uint64_t> presentedVersions;
  while (presentedVersions.size() < 3 && std::chrono::steady_clock::now() < deadline) {
    RunShellFrame(window, shell);
    const auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
    if (frame != nullptr && frame->identity().version > initialVersion) {
      presentedVersions.insert(frame->identity().version);
    }
  }
  EXPECT_THAT(presentedVersions, testing::SizeIs(3));
}

TEST(EditorShellTest, PreviewHoverUsesCapturedTargetsWithoutSelectingOrEditing) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
      <rect id="pad" x="10" y="10" width="80" height="80" fill="red">
        <set attributeName="fill" to="blue" begin="mouseover" end="mouseout"/>
      </rect>
    </svg>)svg";
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(source));
  auto& app = EditorShellTestAccess::App(shell);
  const auto target = app.document().document().querySelector("#pad");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::SetAnimationPreview(shell, true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  std::optional<Vector2d> screenPoint;
  while (std::chrono::steady_clock::now() < deadline) {
    RunShellFrame(window, shell);
    if (!EditorShellTestAccess::AnimationResetPending(shell)) {
      EditorShellTestAccess::HoldAnimationAtStart(shell);
    }
    const auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
    if (frame) {
      const Vector2d point = frame->viewport().documentToScreen(Vector2d(50, 50));
      if (frame->pointerTargetAt(point).has_value()) {
        screenPoint = point;
        break;
      }
    }
  }
  ASSERT_TRUE(screenPoint.has_value());
  const auto versionBefore = app.document().currentFrameVersion();
  while (EditorShellTestAccess::PreviewHoveredElement(shell) != target &&
         std::chrono::steady_clock::now() < deadline) {
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(screenPoint->x),
                                    static_cast<float>(screenPoint->y));
    RunShellFrame(window, shell);
  }
  EXPECT_EQ(EditorShellTestAccess::PreviewHoveredElement(shell), target);
  EXPECT_GT(app.document().currentFrameVersion(), versionBefore);
  EXPECT_FALSE(app.hasSelection());
  EXPECT_FALSE(app.isDirty());
  EXPECT_FALSE(app.canUndo());
}

TEST(EditorShellTest, QueuedPreviewClickStartsAtCurrentTimeUsingItsCapturedTarget) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
      <circle id="orb" cx="60" cy="45" r="10" pointer-events="none">
        <animate attributeName="r" values="10;30;10" begin="pad.click" dur="0.6s"/>
      </circle>
      <rect id="pad" x="10" y="90" width="40" height="20">
        <animate attributeName="x" from="10" to="70" dur="1s" fill="freeze"/>
      </rect>
    </svg>)svg";
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(source));
  auto& app = EditorShellTestAccess::App(shell);
  auto& document = app.document().document();
  EditorShellTestAccess::SetAnimationPreview(shell, true);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  while (!EditorShellTestAccess::PreviewInputReady(shell) &&
         std::chrono::steady_clock::now() < deadline) {
    if (!EditorShellTestAccess::AnimationResetPending(shell)) {
      EditorShellTestAccess::HoldAnimationAtStart(shell);
    }
    RunShellFrame(window, shell);
  }
  ASSERT_TRUE(EditorShellTestAccess::PreviewInputReady(shell));
  ASSERT_DOUBLE_EQ(document.currentTime(), 0);
  const auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
  const Vector2d point = frame->viewport().documentToScreen(Vector2d(30, 100));
  const auto target = frame->pointerTargetAt(point);
  ASSERT_TRUE(target.has_value());
  ASSERT_EQ(target, document.querySelector("#pad"));
  const std::string sourceBefore(document.source());

  EditorShellTestAccess::ClearPreviewPointer(shell);
  EditorShellTestAccess::SetPreviewStart(shell, 10);
  EditorShellTestAccess::ApplyPreviewInputAt(shell, 20);
  EXPECT_DOUBLE_EQ(document.currentTime(), 0) << "Empty input leaves normal ticking in charge";
  EditorShellTestAccess::QueuePreviewClick(shell, *target, point);
  EditorShellTestAccess::ApplyPreviewInputAt(shell, std::numeric_limits<double>::quiet_NaN());
  EXPECT_EQ(EditorShellTestAccess::QueuedPreviewEdges(shell), 2u);
  EXPECT_DOUBLE_EQ(document.currentTime(), 0);
  EditorShellTestAccess::SetPreviewResetPending(shell, true);
  EditorShellTestAccess::ApplyPreviewInputAt(shell, 20);
  EXPECT_EQ(EditorShellTestAccess::QueuedPreviewEdges(shell), 2u);
  EXPECT_DOUBLE_EQ(document.currentTime(), 0);
  EditorShellTestAccess::SetPreviewResetPending(shell, false);
  EditorShellTestAccess::ApplyPreviewInputAt(shell, 20);
  EXPECT_DOUBLE_EQ(document.currentTime(), 10);
  EXPECT_EQ(EditorShellTestAccess::QueuedPreviewEdges(shell), 0u);

  svg::Renderer renderer;
  const auto access = document.tryWriteAccess();
  ASSERT_TRUE(access.has_value());
  for (const auto [time, diameter] : {std::pair(10.3, 60.0), std::pair(10.7, 20.0)}) {
    SCOPED_TRACE(time);
    document.setTime(time);
    renderer.draw(document);
    const auto bounds =
        document.querySelector("#orb")->cast<svg::SVGGeometryElement>().worldBounds();
    ASSERT_TRUE(bounds.has_value());
    EXPECT_NEAR(bounds->width(), diameter, 1e-6);
    const auto padBounds = target->cast<svg::SVGGeometryElement>().worldBounds();
    ASSERT_TRUE(padBounds.has_value());
    EXPECT_NEAR(padBounds->topLeft.x, 70, 1e-6) << "The captured target moved before dispatch";
  }
  EXPECT_EQ(document.source(), sourceBefore);
  EXPECT_FALSE(app.hasSelection());
  EXPECT_FALSE(app.isDirty());
  EXPECT_FALSE(app.canUndo());
}

TEST(EditorShellTest, PreviewHoverDoesNotLeaveTheSharedAncestor) {
  constexpr std::string_view source = R"svg(
    <svg id="root" xmlns="http://www.w3.org/2000/svg"><g id="group">
      <rect id="a" width="10" height="10"/><rect id="b" x="20" width="10" height="10"/>
    </g></svg>)svg";
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(source));
  auto& document = EditorShellTestAccess::App(shell).document().document();
  const auto first = document.querySelector("#a");
  const auto second = document.querySelector("#b");
  ASSERT_THAT(first, testing::Ne(std::nullopt));
  ASSERT_THAT(second, testing::Ne(std::nullopt));
  const auto pair = [](svg::EventType type, const char* id) {
    return std::pair(type, RcString(id));
  };
  EXPECT_THAT(EditorShellTestAccess::HoverTransition(shell, first),
              testing::ElementsAre(pair(svg::EventType::MouseOver, "a"),
                                   pair(svg::EventType::MouseEnter, "root"),
                                   pair(svg::EventType::MouseEnter, "group"),
                                   pair(svg::EventType::MouseEnter, "a")));
  EXPECT_THAT(EditorShellTestAccess::HoverTransition(shell, second),
              testing::ElementsAre(
                  pair(svg::EventType::MouseOut, "a"), pair(svg::EventType::MouseLeave, "a"),
                  pair(svg::EventType::MouseOver, "b"), pair(svg::EventType::MouseEnter, "b")));
}

/// Run Preview frames until the presented frame resolves \p documentPoint to a pointer target.
/// @return The screen position of \p documentPoint in that frame, or nullopt on timeout.
std::optional<Vector2d> AwaitPreviewPointerTarget(gui::EditorWindow& window, EditorShell& shell,
                                                  const Vector2d& documentPoint) {
  for (const tests::HangGuard guard; guard.active();) {
    RunShellFrame(window, shell);
    const auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
    if (frame) {
      const Vector2d screenPoint = frame->viewport().documentToScreen(documentPoint);
      if (frame->pointerTargetAt(screenPoint).has_value()) {
        return screenPoint;
      }
    }
  }
  return std::nullopt;
}

TEST(EditorShellTest, PreviewPointerOverUseTargetsTheStableUseElementAcrossTicks) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
      <defs><rect id="tile" width="80" height="70" fill="red">
        <animate attributeName="fill" values="red;blue" dur="1s" repeatCount="indefinite"/>
      </rect></defs>
      <use id="u" x="10" y="10" href="#tile"/>
      <rect id="entered" y="90" width="10" height="10" visibility="hidden">
        <set attributeName="visibility" to="visible" begin="u.mouseover"/>
      </rect>
      <rect id="left" x="20" y="90" width="10" height="10" visibility="hidden">
        <set attributeName="visibility" to="visible" begin="u.mouseout"/>
      </rect>
    </svg>)svg";
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(source));
  auto& app = EditorShellTestAccess::App(shell);
  auto& document = app.document().document();
  const auto use = document.querySelector("#u");
  ASSERT_THAT(use, testing::Ne(std::nullopt));
  EditorShellTestAccess::SetAnimationPreview(shell, true);
  const std::optional<Vector2d> point = AwaitPreviewPointerTarget(window, shell, Vector2d(50, 50));
  ASSERT_TRUE(point.has_value());
  for (const tests::HangGuard guard;
       EditorShellTestAccess::PreviewHoveredElement(shell) != use && guard.active();) {
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(point->x), static_cast<float>(point->y));
    RunShellFrame(window, shell);
  }
  ASSERT_EQ(EditorShellTestAccess::PreviewHoveredElement(shell), use);

  // Each tick rebuilds the shadow content of the <use>; the target must stay the same element so
  // the pointer never leaves and re-enters it.
  const auto hoveredFrame = EditorShellTestAccess::Coordinator(shell).framePresentation();
  ASSERT_NE(hoveredFrame, nullptr);
  const auto hoveredVersion = hoveredFrame->identity().version;
  std::set<std::uint64_t> versions;
  std::optional<svg::SVGGraphicsElement> enteredFlag;
  std::optional<svg::SVGGraphicsElement> leftFlag;
  for (const tests::HangGuard guard; (versions.size() < 3 || !enteredFlag) && guard.active();) {
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(point->x), static_cast<float>(point->y));
    RunShellFrame(window, shell);
    const auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
    if (frame == nullptr || frame->identity().version <= hoveredVersion ||
        !versions.insert(frame->identity().version).second) {
      continue;
    }
    const auto target = frame->pointerTargetAt(*point);
    EXPECT_THAT(target, testing::Optional(*use));
    EXPECT_THAT(EditorShellTestAccess::HoverTransition(shell, target), testing::IsEmpty());
    enteredFlag = frame->pointerTargetAt(frame->viewport().documentToScreen(Vector2d(5, 95)));
    if (const auto left =
            frame->pointerTargetAt(frame->viewport().documentToScreen(Vector2d(25, 95)))) {
      leftFlag = left;
    }
  }
  EXPECT_THAT(versions, testing::SizeIs(testing::Ge(3u)));
  EXPECT_GT(document.currentTime(), 0.0);
  EXPECT_THAT(enteredFlag, testing::Optional(*document.querySelector("#entered")))
      << "mouseover on the <use> instance starts its animation";
  EXPECT_EQ(leftFlag, std::nullopt) << "re-instantiated <use> content dispatched a mouseout";
  EXPECT_FALSE(app.hasSelection());
  EXPECT_FALSE(app.isDirty());
}

// Leaving Preview with Escape must return a clicked, still-animating <use> to the authored
// document: Edit mode shows and hit-tests the instance where the source puts it.
TEST(EditorShellTest, LeavingPreviewRestoresAnAnimatedUseToItsAuthoredPosition) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
      <defs><rect id="tile" width="20" height="20" fill="blue"/></defs>
      <use id="u" href="#tile" x="10" y="40">
        <set attributeName="x" to="70" begin="click"/>
      </use>
    </svg>)svg";
  const Vector2d authoredPoint(20, 50);
  const Vector2d animatedPoint(80, 50);
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(source));
  auto& app = EditorShellTestAccess::App(shell);
  const auto use = app.document().document().querySelector("#u");
  ASSERT_THAT(use, testing::Ne(std::nullopt));
  const std::string sourceBefore(app.document().document().source());

  EditorShellTestAccess::SetAnimationPreview(shell, true);
  const std::optional<Vector2d> point = AwaitPreviewPointerTarget(window, shell, authoredPoint);
  ASSERT_TRUE(point.has_value());
  for (const bool down : {false, true, false}) {
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(point->x), static_cast<float>(point->y));
    ImGui::GetIO().AddMouseButtonEvent(0, down);
    RunShellFrame(window, shell);
  }
  std::optional<svg::SVGElement> animatedTarget;
  for (const tests::HangGuard guard; animatedTarget != use && guard.active();) {
    RunShellFrame(window, shell);
    if (const auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation()) {
      animatedTarget = frame->pointerTargetAt(frame->viewport().documentToScreen(animatedPoint));
    }
  }
  ASSERT_THAT(animatedTarget, testing::Optional(testing::AllOf(
                                  testing::Property("id", &svg::SVGElement::id, testing::Eq("u")),
                                  testing::Eq(*use))))
      << "clicking the <use> in Preview starts its animation";

  ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
  RunShellFrame(window, shell);
  ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
  RunShellFrame(window, shell);
  ASSERT_FALSE(EditorShellTestAccess::IsAnimationPreview(shell));
  for (const tests::HangGuard guard;
       !EditorShellTestAccess::PreviewInputReady(shell) && guard.active();) {
    RunShellFrame(window, shell);
  }
  ASSERT_TRUE(EditorShellTestAccess::PreviewInputReady(shell));

  const auto authoredHit = app.hitTest(authoredPoint);
  ASSERT_THAT(authoredHit, testing::Ne(std::nullopt));
  EXPECT_THAT(authoredHit->id(), testing::Eq("u"));
  EXPECT_THAT(app.hitTest(animatedPoint), testing::Eq(std::nullopt))
      << "the <use> stayed at its Preview position after leaving Preview";
  EXPECT_EQ(app.document().document().source(), sourceBefore);
  EXPECT_FALSE(app.isDirty());
}

TEST(EditorShellTest, PreviewOnGeodeSplashResolvesUseContentToDocumentElements) {
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  const EditorSample* splash = FindEditorSample("geode-splash");
  ASSERT_NE(splash, nullptr);
  EditorShell shell(window, OptionsWithSource(splash->source, "geode_splash.svg"));
  EditorShellTestAccess::SetAnimationPreview(shell, true);
  ASSERT_TRUE(AwaitPreviewPointerTarget(window, shell, Vector2d(10, 10)).has_value());
  const auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
  ASSERT_NE(frame, nullptr);
  std::set<RcString> hitIds;
  for (double y = 340; y <= 690; y += 10) {
    for (double x = 300; x <= 640; x += 10) {
      const auto target =
          frame->pointerTargetAt(frame->viewport().documentToScreen(Vector2d(x, y)));
      ASSERT_TRUE(target.has_value()) << "the background covers the canvas at " << x << "," << y;
      ASSERT_THAT(target->tryType(), testing::Ne(std::nullopt));
      hitIds.insert(target->id());
    }
  }
  // The rest of the splash's <use> elements are painted over by later shapes on this grid.
  EXPECT_THAT(hitIds, testing::Contains(RcString("quartz-face")))
      << "Hits on <use> content resolve to the <use> element";
}

TEST(EditorShellTest, SelectToolDragsAUseInstanceAndUndoRestoresIt) {
  constexpr std::string_view source = R"svg(
    <svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
      <defs><rect id="tile" width="40" height="40" fill="blue"/></defs>
      <use id="u" x="10" y="10" href="#tile"/>
    </svg>)svg";
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(source));
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto use = app.document().document().querySelector("#u");
  ASSERT_THAT(use, testing::Ne(std::nullopt));
  const auto runFrameWithMouse = [&](const Vector2d& documentPoint, bool mouseDown) {
    const Vector2d screen = shell.viewportForReadback().documentToScreen(documentPoint);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(static_cast<float>(screen.x), static_cast<float>(screen.y));
    io.AddMouseButtonEvent(0, mouseDown);
    RunShellFrame(window, shell);
  };
  const auto settle = [&] {
    for (const tests::HangGuard guard; guard.active();) {
      runFrameWithMouse(Vector2d(-20, -20), /*mouseDown=*/false);
      if (!app.document().hasPendingMutations() && !EditorShellTestAccess::RendererBusy(shell) &&
          EditorShellTestAccess::DisplayedDocVersion(shell) ==
              app.document().currentFrameVersion()) {
        return true;
      }
    }
    return false;
  };
  ASSERT_TRUE(settle());

  // Keep the render worker inside its document access across the input below, as raster work that
  // has not reached a cancellation check does. A selected <use> has no cached selection bounds, so
  // the shell buffers the press until it can hit-test the document.
  AsyncRenderer& renderer = shell.asyncRendererForReplay();
  renderer.setReplayRenderDelayIgnoresCancellationForTesting(true);
  renderer.setReplayRenderDelayForTesting(std::chrono::milliseconds(1000));
  app.setSelection(*use);
  for (const tests::HangGuard guard;
       !EditorShellTestAccess::RendererBusy(shell) && guard.active();) {
    EditorShellTestAccess::SetRequestRenderAtEndOfFrame(shell);
    runFrameWithMouse(Vector2d(20, 20), /*mouseDown=*/false);
  }
  ASSERT_TRUE(EditorShellTestAccess::RendererBusy(shell)) << "The selection change must render";
  // The worker takes the document on its own thread; wait until it holds it before pressing.
  for (const tests::HangGuard guard;
       app.document().document().tryWriteAccess().has_value() && guard.active();) {
    std::this_thread::sleep_for(tests::kPollInterval);
  }
  ASSERT_FALSE(app.document().document().tryWriteAccess().has_value())
      << "The render worker never took the document";
  runFrameWithMouse(Vector2d(20, 20), /*mouseDown=*/true);
  ASSERT_TRUE(EditorShellTestAccess::HasPendingClick(shell))
      << "The press must wait while the worker holds the document";
  renderer.setReplayRenderDelayForTesting(std::chrono::milliseconds(0));
  renderer.setReplayRenderDelayIgnoresCancellationForTesting(false);
  // A user's press outlasts the render: hold the button until the shell dispatches the press.
  for (const tests::HangGuard guard;
       EditorShellTestAccess::HasPendingClick(shell) && guard.active();) {
    runFrameWithMouse(Vector2d(20, 20), /*mouseDown=*/true);
  }
  ASSERT_FALSE(EditorShellTestAccess::HasPendingClick(shell))
      << "The press was never dispatched after the worker released the document";
  runFrameWithMouse(Vector2d(25, 23), /*mouseDown=*/true);
  runFrameWithMouse(Vector2d(35, 28), /*mouseDown=*/true);
  runFrameWithMouse(Vector2d(35, 28), /*mouseDown=*/false);
  ASSERT_TRUE(settle());

  ASSERT_THAT(app.selectedElement(), testing::Optional(*use));
  EXPECT_EQ(use->cast<svg::SVGGraphicsElement>().transform(), Transform2d::Translate(15.0, 8.0));
  EXPECT_THAT(use->getAttribute("transform"), testing::Ne(std::nullopt));

  app.undo();
  ASSERT_TRUE(settle());
  EXPECT_EQ(use->cast<svg::SVGGraphicsElement>().transform(), Transform2d());
  EXPECT_THAT(app.document().document().source(), testing::Eq(source));
}

TEST(EditorShellTest, ReplacingDocumentStopsAnimationPreview) {
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  EditorShellTestAccess::SetAnimationPreview(shell, true);
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 10.0);
  ASSERT_TRUE(EditorShellTestAccess::App(shell).loadFromString(kInitialSvg));
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 11.0);
  EXPECT_FALSE(EditorShellTestAccess::IsAnimationPreview(shell));
  EXPECT_DOUBLE_EQ(EditorShellTestAccess::App(shell).document().document().currentTime(), 0.0);
}

TEST(EditorShellTest, HiddenWindowShellConstructsAndRunsFrames) {
  RunHiddenWindowShellConstructsAndRunsFrames(kTestContentScale);
}

#ifdef DONNER_EDITOR_WGPU
TEST(EditorShellTest, FramesRetireReplacedLayerThumbnailSnapshots) {
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  auto& cache = EditorShellTestAccess::ThumbnailTextures(shell);
  constexpr std::uint64_t kProbeKey = std::numeric_limits<std::uint64_t>::max();
  std::weak_ptr<const svg::RendererTextureSnapshot> retired;
  {
    const auto first = EditorShellTestAccess::RenderProbeThumbnail(shell);
    const auto second = EditorShellTestAccess::RenderProbeThumbnail(shell);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(first, second);
    ASSERT_NE(cache.retainThumbnailTextureSnapshot(kProbeKey, first).texture, 0u);
    ASSERT_NE(cache.retainThumbnailTextureSnapshot(kProbeKey, second).texture, 0u);
    retired = first;
  }
  ASSERT_FALSE(retired.expired());
  ASSERT_EQ(cache.presentationResourceStats().pendingRetiredTextures, 1);
  for (int frame = 0; frame < 3; ++frame) {
    RunShellFrame(window, shell);
    EXPECT_FALSE(retired.expired()) << "Keep replaced thumbnails through the safety window";
  }
  RunShellFrame(window, shell);
  EXPECT_TRUE(retired.expired()) << "Normal editor frames must retire old layer thumbnails";
}

TEST(EditorShellTest, UiRuntimeProducersUseFramebufferDevice) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "WGPU hidden editor window is unavailable on this host";
  }
  const std::shared_ptr<geode::GeodeDevice> primary = window.geodeDevice();
  const std::shared_ptr<geode::GeodeDevice> framebuffer = window.geodeFramebufferDevice();
  ASSERT_NE(primary, nullptr);
  ASSERT_NE(framebuffer, nullptr);
  const std::uint64_t primaryDeviceId = primary->runtimeDevice().deviceId();
  const std::uint64_t framebufferDeviceId = framebuffer->runtimeDevice().deviceId();
  ASSERT_NE(primaryDeviceId, framebufferDeviceId);

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  const std::uint64_t uploadDeviceId =
      EditorShellTestAccess::UploadPresentationBitmapDeviceId(shell);
  EXPECT_THAT(uploadDeviceId, testing::Eq(framebufferDeviceId));
  EXPECT_THAT(uploadDeviceId, testing::Ne(primaryDeviceId));
  EXPECT_THAT(EditorShellTestAccess::RenderLayerThumbnailDeviceId(shell),
              testing::Eq(framebufferDeviceId));
}

TEST(EditorShellTest, CpuAdoptionDoesNotStartPreviewsBeforeForegroundPresentation) {
  gui::EditorWindow window(TestWindowOptions({
      .title = "Blocked first-frame preview admission",
      .initialWidth = 1600,
      .initialHeight = 900,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "WGPU hidden editor window is unavailable on this host";
  }
  std::uint64_t completedSerial = 0;
  window.setPresentationCompletionProbeForTesting([&] { return completedSerial; });

  EditorShellOptions options = OptionsWithSource(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="400" viewBox="0 0 640 400"/>)");
  options.showWelcome = true;
  EditorShell shell(window, options);
  ASSERT_TRUE(shell.valid());
  // Hold the canvas-commit debounce: on a slow host it can elapse while the sample is adopted, and
  // the render-tree rebuild that follows a HiDPI commit leaves a staged result behind that the
  // directly driven sidebar frames below never poll, so the renderer looks busy to them. The
  // commit is not part of this preview-admission contract.
  RenderCoordinatorTestAccess::useFakeSchedulingClock(EditorShellTestAccess::Coordinator(shell));
  for (int frame = 0; frame < 3; ++frame) {
    RunShellFrame(window, shell);
  }
  const auto oldPresentation = window.lastFullyPresentedDocumentFrame();
  ASSERT_TRUE(oldPresentation.has_value());
  ASSERT_FALSE(EditorShellTestAccess::AdaptiveUiLayout(shell).compactTouch());

  EditorShellTestAccess::QueueSampleLoad(shell, "basic-shapes");
  EditorShellTestAccess::ConfirmPendingSampleLoadDiscard(shell);
  EditorShellTestAccess::ProcessPendingSampleLoad(shell);
  EditorShellTestAccess::PrepareSingleFontPreviewRequest(shell, "Bebas Neue");
  ASSERT_EQ(EditorShellTestAccess::PendingFontPreviewCount(shell), 1u);
  const std::size_t layerRendersBefore = EditorShellTestAccess::RenderedLayerThumbnailCount(shell);
  const std::size_t previewRequestsBefore =
      EditorShellTestAccess::RequestedAuxiliaryPreviewCount(shell);

  bool adopted = false;
  for (int attempt = 0; attempt < 200; ++attempt) {
    shell.prepareFrame();
    RunShellFrame(window, shell);
    if (EditorShellTestAccess::DisplayedDocVersion(shell) ==
        EditorShellTestAccess::App(shell).document().currentFrameVersion()) {
      adopted = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(adopted) << "The CPU result did not adopt while presentation admission was blocked";
  ASSERT_FALSE(EditorShellTestAccess::ShowSamplePicker(shell));

  const auto acceptedFrame = EditorShellTestAccess::Coordinator(shell).framePresentation();
  ASSERT_NE(acceptedFrame, nullptr);
  EditorShellTestAccess::InstallFramePresentation(shell, nullptr);
  EXPECT_FALSE(EditorShellTestAccess::ForegroundPresentationAcknowledged(shell));
  EditorShellTestAccess::InstallFramePresentation(shell, acceptedFrame);
  ASSERT_TRUE(shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(10)));

  const std::uint64_t blockedWakeSerial = window.wakeEventLoopRequestSerialForTesting();
  for (int frame = 0; frame < 3; ++frame) {
    window.beginFrame();
    EditorShellTestAccess::RenderSidebars(shell);
    EditorShellTestAccess::AdvanceFontPreviewGeneration(shell);
    window.endFrame();
    EXPECT_EQ(window.wakeEventLoopRequestSerialForTesting(), blockedWakeSerial);
  }
  EXPECT_EQ(EditorShellTestAccess::RenderedLayerThumbnailCount(shell), layerRendersBefore);
  EXPECT_EQ(EditorShellTestAccess::RequestedAuxiliaryPreviewCount(shell), previewRequestsBefore);
  EXPECT_EQ(EditorShellTestAccess::PendingFontPreviewCount(shell), 1u);

  // Retiring the old fences only makes another frame admissible; a failed UI pass still cannot
  // acknowledge the adopted document capture.
  completedSerial = std::numeric_limits<std::uint64_t>::max();
  window.forceUiPassFailureForTesting(true);
  const std::uint64_t owedWakeSerial = window.presentedDocumentWakeSerialForTesting();
  window.beginFrame();
  window.endFrame();
  EXPECT_EQ(window.presentedDocumentWakeSerialForTesting(), owedWakeSerial);
  EXPECT_FALSE(EditorShellTestAccess::ForegroundPresentationAcknowledged(shell));
  EXPECT_EQ(EditorShellTestAccess::RenderedLayerThumbnailCount(shell), layerRendersBefore);
  EXPECT_EQ(EditorShellTestAccess::RequestedAuxiliaryPreviewCount(shell), previewRequestsBefore);
  EXPECT_EQ(EditorShellTestAccess::PendingFontPreviewCount(shell), 1u);

  // A successful frame carrying the old scene identity is equally insufficient.
  window.forceUiPassFailureForTesting(false);
  window.setPresentationFrameIdentity(/*frameId=*/999, oldPresentation->captureId,
                                      oldPresentation->documentGeneration,
                                      oldPresentation->frameVersion,
                                      /*inputRepresented=*/true, /*viewportZoom=*/1.0);
  window.beginFrame();
  window.endFrame();
  EXPECT_FALSE(EditorShellTestAccess::ForegroundPresentationAcknowledged(shell));
  EXPECT_EQ(window.presentedDocumentWakeSerialForTesting(), owedWakeSerial);

  // Only a fully drawn and presented frame carrying the matching adopted capture releases new
  // preview work. Cached previews and the pending queues remain available throughout the hold.
  EditorShellTestAccess::InstallFramePresentation(shell, acceptedFrame);
  window.beginFrame();
  window.endFrame();
  EXPECT_EQ(window.presentedDocumentWakeSerialForTesting(), owedWakeSerial + 1u);
  ASSERT_TRUE(EditorShellTestAccess::ForegroundPresentationAcknowledged(shell));
  window.beginFrame();
  window.endFrame();
  EXPECT_EQ(window.presentedDocumentWakeSerialForTesting(), owedWakeSerial + 1u);
  window.beginFrame();
  EditorShellTestAccess::RenderSidebars(shell);
  EditorShellTestAccess::AdvanceFontPreviewGeneration(shell);
  window.endFrame();
  EXPECT_GT(EditorShellTestAccess::RenderedLayerThumbnailCount(shell), layerRendersBefore);
  EXPECT_GT(EditorShellTestAccess::RequestedAuxiliaryPreviewCount(shell), previewRequestsBefore);
}

TEST(EditorShellTest, SettledEmptyDocumentDoesNotBlockFontPreviews) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "WGPU hidden editor window is unavailable on this host";
  }
  EditorShell shell(
      window,
      OptionsWithSource(R"(<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0"/>)"));
  ASSERT_TRUE(shell.valid());
  RenderCoordinator& coordinator = EditorShellTestAccess::Coordinator(shell);
  ASSERT_EQ(coordinator.framePresentation(), nullptr);
  ASSERT_TRUE(EditorShellTestAccess::ForegroundPresentationAcknowledged(shell));

  const std::size_t requestedBefore = EditorShellTestAccess::RequestedAuxiliaryPreviewCount(shell);
  EditorShellTestAccess::RequestFontPreviews(shell, {"Bebas Neue"});
  EditorShellTestAccess::AdvanceFontPreviewGeneration(shell);
  EXPECT_GT(EditorShellTestAccess::RequestedAuxiliaryPreviewCount(shell), requestedBefore);
}

TEST(EditorShellTest, SidebarPresentationWakeTracksOnlyRenderableRasterDemand) {
  gui::EditorWindow window(TestWindowOptions({
      .title = "Sidebar presentation wake policy",
      .initialWidth = 844,
      .initialHeight = 390,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "WGPU hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::SetForegroundPresentationObligation(
      shell, {.documentGeneration = 91, .frameVersion = 7, .captureId = 3});
  EditorShellTestAccess::SetSidebarSnapshotRefreshPending(shell, true);

  const EditorAdaptiveUiLayout compact = ComputeEditorAdaptiveUiLayout({
      .windowWidth = 844.0f,
      .windowHeight = 390.0f,
      .preferTouch = true,
  });
  ASSERT_TRUE(compact.compactTouch());
  EditorShellTestAccess::SetAdaptiveUiLayout(shell, compact);
  EditorShellTestAccess::SetCompactPanelVisible(shell, true);
  window.beginFrame();
  EditorShellTestAccess::RenderSidebars(shell);
  EXPECT_FALSE(window.hasPendingPresentedDocumentWakeForTesting())
      << "compact swatches never consume raster thumbnails and must not arm their wake";
  window.endFrame();

  EditorShellTestAccess::SetAdaptiveUiLayout(shell, ComputeEditorAdaptiveUiLayout({
                                                        .windowWidth = 1600.0f,
                                                        .windowHeight = 900.0f,
                                                        .preferTouch = false,
                                                    }));
  EditorShellTestAccess::SetCompactPanelVisible(shell, false);
  EditorShellTestAccess::SetSidebarSnapshotRefreshPending(shell, false);
  window.beginFrame();
  EditorShellTestAccess::RenderSidebars(shell);
  EXPECT_TRUE(window.hasPendingPresentedDocumentWakeForTesting())
      << "a non-sample first load with stale rows must arm one wake without font demand";
  window.endFrame();
}

TEST(EditorShellTest, NewShellInvalidatesPriorProducerPresentationAck) {
  gui::EditorWindow window(TestWindowOptions({
      .title = "Reused-window presentation producer",
      .initialWidth = 1600,
      .initialHeight = 900,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "WGPU hidden editor window is unavailable on this host";
  }
  std::uint64_t completedSerial = std::numeric_limits<std::uint64_t>::max();
  window.setPresentationCompletionProbeForTesting([&] { return completedSerial; });

  gui::EditorWindow::PresentedDocumentFrameIdentity firstIdentity;
  // Each shell holds its canvas-commit debounce. On a slow host it can elapse within the loops
  // below, and the render-tree rebuild after a HiDPI commit adopts a second capture, so the
  // identities compared here would depend on host speed. The commit is not part of this contract.
  {
    EditorShell first(window, OptionsWithSource(kInitialSvg));
    ASSERT_TRUE(first.valid());
    RenderCoordinatorTestAccess::useFakeSchedulingClock(EditorShellTestAccess::Coordinator(first));
    bool acknowledged = false;
    for (int attempt = 0; attempt < 200; ++attempt) {
      first.prepareFrame();
      RunShellFrame(window, first);
      const auto frame = EditorShellTestAccess::Coordinator(first).framePresentation();
      const auto presented = window.lastFullyPresentedDocumentFrame();
      if (frame != nullptr && frame->identity().captureId > 0 && presented.has_value() &&
          *presented == gui::EditorWindow::PresentedDocumentFrameIdentity{
                            .documentGeneration = frame->identity().documentGeneration,
                            .frameVersion = frame->identity().version,
                            .captureId = frame->identity().captureId}) {
        firstIdentity = *presented;
        acknowledged = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_TRUE(acknowledged);
    ASSERT_GT(firstIdentity.captureId, 0u);

    completedSerial = 0;
    for (int frame = 0; frame < 3; ++frame) {
      RunShellFrame(window, first);
    }
    ASSERT_EQ(window.lastFullyPresentedDocumentFrame(), firstIdentity);
  }

  EditorShell second(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(second.valid());
  RenderCoordinatorTestAccess::useFakeSchedulingClock(EditorShellTestAccess::Coordinator(second));
  EXPECT_FALSE(window.lastFullyPresentedDocumentFrame().has_value());
  const std::size_t layerRendersBefore = EditorShellTestAccess::RenderedLayerThumbnailCount(second);

  bool adopted = false;
  for (int attempt = 0; attempt < 200; ++attempt) {
    second.prepareFrame();
    RunShellFrame(window, second);
    if (EditorShellTestAccess::DisplayedDocVersion(second) ==
            EditorShellTestAccess::App(second).document().currentFrameVersion() &&
        EditorShellTestAccess::Coordinator(second).framePresentation() != nullptr) {
      adopted = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(adopted);
  const auto secondIdentity = EditorShellTestAccess::ForegroundPresentationObligation(second);
  ASSERT_TRUE(secondIdentity.has_value());
  ASSERT_EQ(*secondIdentity, firstIdentity);
  EXPECT_EQ(EditorShellTestAccess::RenderedLayerThumbnailCount(second), layerRendersBefore);
  ASSERT_TRUE(second.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(10)));

  EditorShellTestAccess::PrepareSingleFontPreviewRequest(second, "Bebas Neue");
  ASSERT_EQ(EditorShellTestAccess::PendingFontPreviewCount(second), 1u);
  const std::size_t previewRequestsBefore =
      EditorShellTestAccess::RequestedAuxiliaryPreviewCount(second);

  window.beginFrame();
  EditorShellTestAccess::AdvanceFontPreviewGeneration(second);
  EditorShellTestAccess::RenderSidebars(second);
  window.endFrame();
  EXPECT_FALSE(EditorShellTestAccess::ForegroundPresentationAcknowledged(second));
  EXPECT_EQ(EditorShellTestAccess::RenderedLayerThumbnailCount(second), layerRendersBefore);
  EXPECT_EQ(EditorShellTestAccess::RequestedAuxiliaryPreviewCount(second), previewRequestsBefore);
  EXPECT_EQ(EditorShellTestAccess::PendingFontPreviewCount(second), 1u);
}
#endif

// A render that produces nothing to present is retried after each delay without any input: the
// idle loop wakes for the retry, and that frame alone must post it.
TEST(EditorShellTest, NothingToPresentRetriesPostFromIdleFrames) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  ExpectTestWindowScale(window, Vector2i(640, 480), kTestContentScale);
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  RenderCoordinator& coordinator = EditorShellTestAccess::Coordinator(shell);
  RenderCoordinatorTestAccess::useFakeSchedulingClock(coordinator);
  coordinator.asyncRenderer().setWithholdCompositorTilesForTesting(true);

  // Idle frames only: no input, no document edit. The worker finishes each render before the next
  // frame, which polls its result.
  const auto runIdleFrames = [&](int frames) {
    for (int frame = 0; frame < frames; ++frame) {
      RunShellFrame(window, shell);
      EXPECT_TRUE(coordinator.asyncRenderer().waitUntilNoRenderInFlightForTesting(
          std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    }
    RunShellFrame(window, shell);
    return coordinator.nothingToPresentResultTotalForDiagnostics();
  };

  std::ignore = runIdleFrames(30);
  RenderCoordinatorTestAccess::makeCanvasCommitDue(coordinator);
  RenderCoordinatorTestAccess::makeRasterViewportSettled(coordinator);
  const std::uint64_t settled = runIdleFrames(10);
  const std::uint64_t retryVersion =
      EditorShellTestAccess::App(shell).document().currentFrameVersion();
  const Vector2i retryCanvas = EditorShellTestAccess::App(shell).document().document().canvasSize();
  ASSERT_GE(settled, 1u) << "the editor's first render must have produced nothing to present";
  EXPECT_EQ(runIdleFrames(10), settled) << "no retry before its delay has passed";

  std::uint64_t expected = settled;
  for (const std::chrono::milliseconds delay : NothingToPresentRetry::kRetryDelays) {
    RenderCoordinatorTestAccess::advanceFakeSchedulingClock(delay);
    ++expected;
    EXPECT_EQ(runIdleFrames(10), expected)
        << "the retry due after " << delay.count() << " ms must post from an idle frame; "
        << RenderCoordinatorTestAccess::overviewScheduleState(
               coordinator, EditorShellTestAccess::App(shell), shell.viewportForReadback());
    EXPECT_THAT(EditorShellTestAccess::App(shell).document().currentFrameVersion(),
                testing::Eq(retryVersion));
    EXPECT_THAT(EditorShellTestAccess::App(shell).document().document().canvasSize(),
                testing::Eq(retryCanvas));
  }
  RenderCoordinatorTestAccess::advanceFakeSchedulingClock(std::chrono::minutes(1));
  EXPECT_EQ(runIdleFrames(10), expected) << "after the last retry, idle frames post nothing";
  EXPECT_EQ(coordinator.nextNothingToPresentRetryWakeSeconds(), std::nullopt);
}

// A retry that falls due while the editor cannot ask for a render, here because the sample picker
// covers the canvas, waits without keeping the idle loop awake, and posts once the picker closes.
TEST(EditorShellTest, DueNothingToPresentRetryWaitsForTheSamplePickerWithoutWaking) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  RenderCoordinator& coordinator = EditorShellTestAccess::Coordinator(shell);
  RenderCoordinatorTestAccess::useFakeSchedulingClock(coordinator);
  coordinator.asyncRenderer().setWithholdCompositorTilesForTesting(true);
  const auto runIdleFrames = [&](int frames) {
    for (int frame = 0; frame < frames; ++frame) {
      RunShellFrame(window, shell);
      EXPECT_TRUE(coordinator.asyncRenderer().waitUntilNoRenderInFlightForTesting(
          std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    }
    RunShellFrame(window, shell);
    return coordinator.nothingToPresentResultTotalForDiagnostics();
  };
  const std::uint64_t settled = runIdleFrames(30);
  ASSERT_GE(settled, 1u);

  EditorShellTestAccess::SetShowSamplePicker(shell, true);
  RenderCoordinatorTestAccess::advanceFakeSchedulingClock(
      NothingToPresentRetry::kRetryDelays.front());
  EXPECT_EQ(runIdleFrames(10), settled) << "the picker covers the canvas, so nothing renders";
  EXPECT_EQ(coordinator.nextNothingToPresentRetryWakeSeconds(), std::nullopt)
      << "a due retry that cannot post must not keep the idle loop awake";

  EditorShellTestAccess::SetShowSamplePicker(shell, false);
  EXPECT_EQ(runIdleFrames(10), settled + 1) << "the due retry posts once the picker closes";
}

void RunFullFrameSmokeCoversPanelSourceAndContextMenuStates(double contentScale) {
  gui::EditorWindow window = MakeHiddenWindow(contentScale);
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  ExpectTestWindowScale(window, Vector2i(640, 480), contentScale);

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  RunFramesUntilDisplayedSelectionBounds(window, shell);

  // Showing the Compositor Debug panel rebuilds the DockSpace layout to add its
  // node; a full frame exercises the docked-panel path.
  EditorShellTestAccess::SetShowCompositorDebugPanel(shell, true);
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_TRUE(shell.valid());

  // Unlocking the layout and requesting a reset both flow through runFrame's
  // DockSpace host; the reset request is consumed within the frame.
  EditorShellTestAccess::SetDockLayoutLocked(shell, false);
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_FALSE(EditorShellTestAccess::DockLayoutLocked(shell));

  EditorShellTestAccess::RequestDockLayoutReset(shell);
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_FALSE(EditorShellTestAccess::DockLayoutResetRequested(shell));

  EditorShellTestAccess::SetSourcePaneVisible(shell, false);
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));

  EditorShellTestAccess::SetSourcePaneVisible(shell, true);
  EditorShellTestAccess::SetSourceFocusMode(shell, true);
  EditorShellTestAccess::UpdateSourceFocusView(shell, /*scrollToSelection=*/true);
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_TRUE(EditorShellTestAccess::SourceFocusMode(shell));

  EditorShellTestAccess::OpenRenderPaneContextMenu(shell, Vector2d(12.0, 14.0));
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuOpenRequested(shell));
}

TEST(EditorShellTest, FullFrameSmokeCoversPanelSourceAndContextMenuStates) {
  RunFullFrameSmokeCoversPanelSourceAndContextMenuStates(kTestContentScale);
}

TEST(EditorShellTest, IntrinsicSizeDocumentInitializesViewportWithoutViewBox) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kIntrinsicSizeSvg, "intrinsic.svg"));
  ASSERT_TRUE(shell.valid());

  window.beginFrame();
  EditorShellTestAccess::RenderRenderPane(
      shell, Vector2d(0.0, 0.0), Vector2d(320.0, 200.0),
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
  window.endFrame();

  const ViewportState viewport = shell.viewportForReadback();
  EXPECT_GT(viewport.documentViewBox.width(), 100.0);
  EXPECT_GT(viewport.documentViewBox.height(), 50.0);
}

TEST(EditorShellTest, SelectionReadbackAndIdleWakeReflectSourceState) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  EXPECT_FALSE(shell.nextIdleWakeSeconds().has_value());
  EXPECT_FALSE(shell.selectedElementLabelForReadback().has_value());

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  EXPECT_EQ(shell.selectedElementLabelForReadback(), "<rect> #target");
  EXPECT_TRUE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));
  ASSERT_FALSE(EditorShellTestAccess::Source(shell).getSelectedText().empty());

  const std::vector<SourceByteRange> ranges =
      EditorShellTestAccess::SourceHoverRangesForElements(shell, {*target});
  ASSERT_FALSE(ranges.empty());
  EditorShellTestAccess::RevealSourceRange(shell, ranges.front());

  const std::optional<float> idleWake = shell.nextIdleWakeSeconds();
  ASSERT_TRUE(idleWake.has_value());
  EXPECT_GE(*idleWake, 0.0f);
}

TEST(EditorShellTest, LayerInspectorReadbackIncludesSelectedStyleAndPathDiagnostics) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  constexpr std::string_view kDiagnosticSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <path id="target" d="M 10 12 L 50 12 L 50 36 Z" style="fill: #010203; stroke: #040506"/>
</svg>
)svg";
  EditorShell shell(window, OptionsWithSource(kDiagnosticSvg, "diagnostics.svg"));
  ASSERT_TRUE(shell.valid());
  RenderCoordinator& coordinator = EditorShellTestAccess::Coordinator(shell);
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  (void)target->getComputedStyle();
  RunFramesUntilDisplayedSelectionBounds(window, shell);
  const tests::HangGuard initialRenderGuard;
  ASSERT_TRUE(coordinator.asyncRenderer().waitUntilNoRenderInFlightForTesting(
      initialRenderGuard.deadline()));
  RunShellFrame(window, shell);
  EXPECT_EQ(coordinator.documentCanvasCommitTotal(), 0u);
  EXPECT_EQ(EditorShellTestAccess::App(shell).document().document().canvasSize(),
            Vector2i(120, 80));
  const tests::HangGuard rebuildGuard;
  ASSERT_THAT(
      coordinator.asyncRenderer().waitUntilNoRenderInFlightForTesting(rebuildGuard.deadline()),
      testing::IsTrue());
  RunShellFrame(window, shell);

  const LayerInspectorStatusReadback status = shell.layerInspectorStatusForReadback();

  ASSERT_TRUE(status.selectedStyleAttribute.has_value());
  EXPECT_NE(status.selectedStyleAttribute->find("fill: #010203"), std::string::npos);
  ASSERT_TRUE(status.selectedLocalStyleFill.has_value());
  EXPECT_FALSE(status.selectedLocalStyleFill->empty());
  ASSERT_TRUE(status.selectedComputedFill.has_value());
  EXPECT_FALSE(status.selectedComputedFill->empty());
  ASSERT_TRUE(status.selectedRenderingInstanceFill.has_value());
  EXPECT_FALSE(status.selectedRenderingInstanceFill->empty());
  ASSERT_TRUE(status.selectedPathDataAttribute.has_value());
  EXPECT_EQ(*status.selectedPathDataAttribute, "M 10 12 L 50 12 L 50 36 Z");

  // A canvas commit clears the optional rendering instance until the next document render.
  EditorShellTestAccess::App(shell).document().document().setCanvasSize(480, 320);
  const LayerInspectorStatusReadback afterCommit = shell.layerInspectorStatusForReadback();
  EXPECT_THAT(afterCommit.selectedRenderingInstanceFill, testing::Eq(std::nullopt));
  EXPECT_THAT(afterCommit.selectedStyleAttribute,
              testing::Optional(testing::Eq("fill: #010203; stroke: #040506")));
  EXPECT_THAT(afterCommit.selectedLocalStyleFill,
              testing::Optional(testing::Eq("PaintServer(solid rgba(1, 2, 3, 255))")));
  EXPECT_THAT(afterCommit.selectedComputedFill,
              testing::Optional(testing::Eq("PaintServer(solid rgba(1, 2, 3, 255))")));
  EXPECT_THAT(afterCommit.selectedPathDataAttribute,
              testing::Optional(testing::Eq("M 10 12 L 50 12 L 50 36 Z")));
}

TEST(EditorShellTest, ReplayActionsSwitchToolsAndIgnoreUnknownToolNames) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "pen",
                                           });
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "text",
                                           });
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "select",
                                           });
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "unsupported",
                                           });
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::CommitPenPath,
                                           });
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
}

TEST(EditorShellTest, ChromeRetainsRasterRevisionWhileLiveDocumentAdvances) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  const std::uint64_t presentedVersion = HoldRasterWhileLiveDocumentAdvances(window, shell);
  ASSERT_GT(presentedVersion, 0u) << "The gate only engages once a render has been presented.";
  ASSERT_TRUE(EditorShellTestAccess::ImmediateChromePlanProduced(shell));
  ASSERT_EQ(EditorShellTestAccess::ImmediateOverlayDocumentVersion(shell),
            std::optional<std::uint64_t>(presentedVersion));
  ASSERT_GT(app.document().currentFrameVersion(), presentedVersion);

  // Leaving the Pen tool takes away the live-geometry allowance, and there is no drag projection
  // to reconcile chrome with older pixels, so the next frame is one the gate suppresses.
  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "select",
                                           });
  RunShellFrame(window, shell);

  ASSERT_EQ(EditorShellTestAccess::DisplayedDocVersion(shell), presentedVersion)
      << "Withheld results should have kept the presented version pinned across the frame.";
  EXPECT_EQ(EditorShellTestAccess::ImmediateOverlayDocumentVersion(shell), presentedVersion);
  EXPECT_TRUE(EditorShellTestAccess::ImmediateChromePlanProduced(shell))
      << "A suppressed overlay refresh must not take the chrome pass with it: without a plan the "
         "frame presents document pixels with no chrome drawn over them, and the selection "
         "outline, bounds and handles blink out until the worker catches up.";
  EXPECT_EQ(EditorShellTestAccess::SelectionChromePathCount(shell), 1u)
      << "The retained chrome should still outline the selected element.";
}

TEST(EditorShellTest, DeselectionDropsChromeWhileRetainingRaster) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  const std::uint64_t presentedVersion = HoldRasterWhileLiveDocumentAdvances(window, shell);
  ASSERT_GT(presentedVersion, 0u);
  ASSERT_EQ(EditorShellTestAccess::SelectionChromePathCount(shell), 1u);

  // Deselecting under the same suppressed-refresh conditions. Retained chrome may trail the live
  // geometry, but it may never outline an element that is no longer selected.
  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "select",
                                           });
  app.clearSelection();
  ASSERT_GT(app.document().currentFrameVersion(), presentedVersion)
      << "The Pen frame must leave the chrome captured ahead of the presented pixels.";
  RunShellFrame(window, shell);

  ASSERT_EQ(EditorShellTestAccess::DisplayedDocVersion(shell), presentedVersion);
  EXPECT_EQ(EditorShellTestAccess::SelectionChromePathCount(shell), 0u)
      << "Chrome captured for the old selection must not survive the deselect just because the "
         "overlay refresh is otherwise suppressed.";
}

TEST(EditorShellTest, ReplayActionSelectCommitsDraftPenPath) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "pen",
                                           });
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(25.0, 16.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(25.0, 16.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "select",
                                           });

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);
}

TEST(EditorShellTest, ReplayActionCommitPenPathCommitsDraftPathInPlace) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetActiveTool,
                                               .tool = "pen",
                                           });
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(8.0, 9.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(8.0, 9.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(28.0, 19.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(28.0, 19.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::CommitPenPath,
                                           });

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);
}

TEST(EditorShellTest, ReplayStyleActionsUpdateSelectionAndActivePaintFallbacks) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetStyleProperty,
                                               .propertyName = "fill",
                                               .propertyValue = "#010203",
                                           });
  (void)EditorShellTestAccess::App(shell).flushFrame();
  target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(target->getAttribute("style").has_value());
  EXPECT_NE(target->getAttribute("style")->str().find("fill: #010203"), std::string::npos);

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetStyleProperty,
                                               .propertyName = "stroke",
                                               .propertyValue = "#040506",
                                           });
  (void)EditorShellTestAccess::App(shell).flushFrame();
  target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(target->getAttribute("style").has_value());
  EXPECT_NE(target->getAttribute("style")->str().find("stroke: #040506"), std::string::npos);

  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetStyleProperty,
                                               .propertyName = "stroke-width",
                                               .propertyValue = "7.5",
                                           });
  (void)EditorShellTestAccess::App(shell).flushFrame();
  target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(target->getAttribute("style").has_value());
  EXPECT_NE(target->getAttribute("style")->str().find("stroke-width: 7.5"), std::string::npos);

  EditorShellTestAccess::App(shell).clearSelection();
  EditorShellTestAccess::ApplyReplayAction(shell,
                                           repro::ReproAction{
                                               .kind = repro::ReproAction::Kind::SetStyleProperty,
                                               .propertyName = "stroke-width",
                                               .propertyValue = "not-a-number",
                                           });
  EXPECT_TRUE(EditorShellTestAccess::App(shell).document().queue().empty());
}

TEST(EditorShellTest, ConstructsFromSvgPathAndKeepsInvalidSourceEditable) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  const std::filesystem::path validPath = TempPathForTest("valid.svg");
  WriteTextFile(validPath, kInitialSvg);

  EditorShellOptions pathOptions;
  pathOptions.svgPath = validPath.string();
  EditorShell pathShell(window, std::move(pathOptions));

  ASSERT_TRUE(pathShell.valid());
  EXPECT_TRUE(EditorShellTestAccess::App(pathShell).hasDocument());
  ASSERT_TRUE(EditorShellTestAccess::App(pathShell).currentFilePath().has_value());
  EXPECT_EQ(*EditorShellTestAccess::App(pathShell).currentFilePath(), validPath.string());

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));

  EXPECT_TRUE(invalidShell.valid());
  EXPECT_FALSE(EditorShellTestAccess::App(invalidShell).hasDocument());
  EXPECT_EQ(EditorShellTestAccess::Source(invalidShell).getText(), "<svg><rect></svg>");
}

/// SVG source with more tree nodes than the parser accepts, one element per line when
/// \p prettyPrinted, so whitespace text nodes count toward the limit too.
std::string SvgOverTheElementLimit(bool prettyPrinted = false) {
  std::string source = R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">)";
  for (int i = 0; i < 8'300; ++i) {
    source += prettyPrinted ? "\n  " : "";
    source += R"(<rect width="1" height="1"/>)";
  }
  return source + "</svg>";
}

// A document that cannot be loaded must explain itself on the canvas, where the user is looking,
// rather than only in the Problems list of the source pane, which is closed by default.
TEST(EditorShellTest, DocumentThatFailsToLoadShowsANoticeOnTheCanvas) {
  struct Case {
    const char* name;
    std::string source;
  };
  for (const Case& item : {Case{"over-element-limit", SvgOverTheElementLimit()},
                           Case{"pretty-printed-over-limit", SvgOverTheElementLimit(true)},
                           Case{"malformed", "<svg><rect></svg>"}}) {
    SCOPED_TRACE(item.name);
    gui::EditorWindow window(TestWindowOptions(
        {.title = "Load failure", .initialWidth = 1280, .initialHeight = 700}, 1.0));
    ASSERT_TRUE(window.valid());
    EditorShell shell(window, OptionsWithSource(item.source, "broken.svg"));
    ASSERT_FALSE(EditorShellTestAccess::App(shell).hasDocument());
    ASSERT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));

    const ImGuiWindow* notice = nullptr;
    for (const tests::HangGuard guard; notice == nullptr && guard.active();) {
      RunShellFrame(window, shell);
      notice = ImGui::FindWindowByName("###canvas_load_failure");
      if (notice != nullptr && !notice->WasActive) {
        notice = nullptr;
      }
    }
    ASSERT_NE(notice, nullptr) << "the canvas is blank with no explanation";
    const ViewportState& viewport = shell.viewportForReadback();
    const Box2d pane(viewport.paneOrigin, viewport.paneOrigin + viewport.paneSize);
    const Box2d noticeRect =
        Box2d::FromXYWH(notice->Pos.x, notice->Pos.y, notice->Size.x, notice->Size.y);
    EXPECT_TRUE(pane.contains(noticeRect.topLeft) && pane.contains(noticeRect.bottomRight))
        << "notice " << noticeRect << " is not inside the canvas " << pane;
  }
}

// Show in Source opens the closed source pane with the parse error selected, so the user lands on
// the problem the notice describes.
TEST(EditorShellTest, LoadFailureNoticeShowsTheProblemInTheSourcePane) {
  gui::EditorWindow window(TestWindowOptions(
      {.title = "Load failure", .initialWidth = 1280, .initialHeight = 700}, 1.0));
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource("<svg>\n  <rect>\n</svg>", "broken.svg"));
  // An auto-sized notice measures itself on its first frame, then settles; aim once it is stable.
  std::optional<Box2d> button;
  std::optional<Box2d> previousButton;
  for (const tests::HangGuard guard;
       (!button.has_value() || button != previousButton) && guard.active();) {
    previousButton = button;
    RunShellFrame(window, shell);
    button = EditorShellTestAccess::LoadFailureRevealButtonRect(shell);
  }
  ASSERT_THAT(button, testing::Ne(std::nullopt));
  ASSERT_EQ(button, previousButton) << "the notice never settled";
  const Vector2d center = (button->topLeft + button->bottomRight) * 0.5;
  for (const bool down : {false, true, false}) {
    ImGui::GetIO().AddMousePosEvent(static_cast<float>(center.x), static_cast<float>(center.y));
    ImGui::GetIO().AddMouseButtonEvent(0, down);
    RunShellFrame(window, shell);
  }
  EXPECT_TRUE(EditorShellTestAccess::SourcePaneVisible(shell));
  const TextEditor& source = EditorShellTestAccess::Source(shell);
  const auto& diagnostics = source.sourceDiagnostics();
  const auto error =
      std::ranges::find(diagnostics, DiagnosticSeverity::Error, &SourceDiagnostic::severity);
  ASSERT_NE(error, diagnostics.end());
  EXPECT_THAT(source.getSelectedText(),
              testing::Eq(std::string_view("<svg>\n  <rect>\n</svg>")
                              .substr(error->range.start, error->range.end - error->range.start)));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).hasDocument());
}

TEST(EditorShellTest, LoadFailureNoticeNamesTheElementLimitOrTheParseError) {
  EXPECT_EQ(internal::DocumentLoadFailureNoticeFor("Maximum element count exceeded", 4097),
            (internal::DocumentLoadFailureNotice{
                .title = "This document is too large to open",
                .detail = "The editor loads up to 8,192 elements, text runs and comments, and this "
                          "file passes that limit at line 4,097."}));
  EXPECT_EQ(
      internal::DocumentLoadFailureNoticeFor("Maximum element count exceeded", std::nullopt).detail,
      "The editor loads up to 8,192 elements, text runs and comments.");
  EXPECT_EQ(internal::DocumentLoadFailureNoticeFor("Mismatched closing tag", 3),
            (internal::DocumentLoadFailureNotice{.title = "This document could not be opened",
                                                 .detail = "Mismatched closing tag on line 3."}));
  EXPECT_EQ(internal::DocumentLoadFailureNoticeFor("", std::nullopt).detail,
            "The SVG could not be parsed.");
}

TEST(EditorShellTest, OpenSaveAndRevertUpdateDocumentAndSourceState) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::SetShowSamplePicker(shell, true);

  std::string error;
  EXPECT_FALSE(
      EditorShellTestAccess::TryOpenPath(shell, TempPathForTest("missing.svg").string(), &error));
  EXPECT_EQ(error, "Could not open file.");
  EXPECT_TRUE(EditorShellTestAccess::ShowSamplePicker(shell));

  const std::filesystem::path invalidPath = TempPathForTest("invalid.svg");
  WriteTextFile(invalidPath, "<svg><rect></svg>");
  error.clear();
  EXPECT_FALSE(EditorShellTestAccess::TryOpenPath(shell, invalidPath.string(), &error));
  EXPECT_EQ(error, "Failed to parse SVG.");

  const std::filesystem::path openPath = TempPathForTest("opened.svg");
  constexpr std::string_view kOpenedSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg"><rect id="opened"/></svg>)svg";
  WriteTextFile(openPath, kOpenedSvg);
  error.clear();
  EXPECT_TRUE(EditorShellTestAccess::TryOpenPath(shell, openPath.string(), &error)) << error;
  EXPECT_TRUE(EditorShellTestAccess::App(shell).hasDocument());
  EXPECT_NE(EditorShellTestAccess::Source(shell).getText().find("opened"), std::string::npos);
  EXPECT_FALSE(EditorShellTestAccess::ShowSamplePicker(shell));

  error.clear();
  EXPECT_FALSE(EditorShellTestAccess::TrySavePath(shell, "", &error));
  EXPECT_EQ(error, "Choose a file path.");

  EditorShell invalidSaveShell(window, OptionsWithSource("<svg><rect></svg>", "broken-save.svg"));
  ASSERT_TRUE(invalidSaveShell.valid());
  error.clear();
  EXPECT_FALSE(EditorShellTestAccess::TrySavePath(invalidSaveShell, "ignored.svg", &error));
  EXPECT_EQ(error, "No SVG document is loaded.");

  const std::filesystem::path savePath = TempPathForTest("saved.svg");
  error.clear();
  EXPECT_TRUE(EditorShellTestAccess::TrySavePath(shell, savePath.string(), &error)) << error;
  EXPECT_NE(ReadTextFile(savePath).find("opened"), std::string::npos);
  EXPECT_FALSE(EditorShellTestAccess::App(shell).isDirty());

  error.clear();
  EXPECT_FALSE(EditorShellTestAccess::TrySavePath(
      shell, TempPathForTest("missing_directory/saved.svg").string(), &error));
  EXPECT_FALSE(error.empty());

  EditorShellTestAccess::Source(shell).setText("<svg>dirty</svg>");
  EditorShellTestAccess::App(shell).markDirty();
  EditorShellTestAccess::RequestRevert(shell);

  EXPECT_FALSE(EditorShellTestAccess::App(shell).isDirty());
  EXPECT_NE(EditorShellTestAccess::Source(shell).getText().find("opened"), std::string::npos);
}

TEST(EditorShellTest, GroupDispatchFlushesAtomicallyAndDeferredSampleWaitsForQueuedEdits) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  constexpr std::string_view kGroupableSvg = R"svg(<svg xmlns="http://www.w3.org/2000/svg">
    <rect id="a"/><circle id="b"/>
  </svg>)svg";
  EditorShell shell(window, OptionsWithSource(kGroupableSvg, "groupable.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const svg::SVGElement a = *app.document().document().querySelector("#a");
  const svg::SVGElement b = *app.document().document().querySelector("#b");
  app.setSelection(std::vector<svg::SVGElement>{a, b});

  ASSERT_TRUE(EditorShellTestAccess::TryApplyGroupOperation(shell, /*ungroup=*/false));
  EXPECT_FALSE(app.document().hasPendingMutations());
  ASSERT_EQ(app.selectedElements().size(), 1u);
  EXPECT_EQ(app.selectedElements().front().tryType(), svg::ElementType::G);

  ASSERT_TRUE(EditorShellTestAccess::TryApplyGroupOperation(shell, /*ungroup=*/true));
  EXPECT_FALSE(app.document().hasPendingMutations());
  ASSERT_EQ(app.selectedElements().size(), 2u);

  ASSERT_TRUE(app.groupSelection());
  ASSERT_TRUE(app.document().hasPendingMutations());
  EditorShellTestAccess::QueueSampleLoad(shell, "basic-shapes");
  EditorShellTestAccess::ProcessPendingSampleLoad(shell);
  EXPECT_EQ(EditorShellTestAccess::PendingSampleLoad(shell), "basic-shapes");
  EXPECT_FALSE(app.document().document().querySelector("polygon").has_value());

  ASSERT_TRUE(app.flushFrame());
  EditorShellTestAccess::ProcessPendingSampleLoad(shell);
  EXPECT_TRUE(EditorShellTestAccess::PendingSampleLoadNeedsConfirmation(shell));
  EXPECT_EQ(EditorShellTestAccess::PendingSampleLoad(shell), "basic-shapes");
  EditorShellTestAccess::ConfirmPendingSampleLoadDiscard(shell);
  EditorShellTestAccess::ProcessPendingSampleLoad(shell);
  EXPECT_TRUE(EditorShellTestAccess::PendingSampleLoad(shell).empty());
  EXPECT_TRUE(app.document().document().querySelector("polygon").has_value());
  EXPECT_FALSE(app.currentFilePath().has_value());
}

TEST(EditorShellTest, SampleLoadRequiresConfirmationForPendingSourceEdits) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::Source(shell).setText("<svg>unsaved</svg>");
  EditorShellTestAccess::QueueSampleLoad(shell, "basic-shapes");

  EditorShellTestAccess::ProcessPendingSampleLoad(shell);

  EXPECT_TRUE(EditorShellTestAccess::PendingSampleLoadNeedsConfirmation(shell));
  EXPECT_EQ(EditorShellTestAccess::PendingSampleLoad(shell), "basic-shapes");
  EXPECT_FALSE(
      EditorShellTestAccess::App(shell).document().document().querySelector("polygon").has_value());

  EditorShellTestAccess::ConfirmPendingSampleLoadDiscard(shell);
  EditorShellTestAccess::ProcessPendingSampleLoad(shell);

  EXPECT_TRUE(EditorShellTestAccess::PendingSampleLoad(shell).empty());
  EXPECT_TRUE(
      EditorShellTestAccess::App(shell).document().document().querySelector("polygon").has_value());
}

TEST(EditorShellTest, NewerFileAndRevertRequestsCancelDeferredSampleReplacement) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::QueueSampleLoad(shell, "basic-shapes");

  const std::filesystem::path openPath = TempPathForTest("sample-cancel-open.svg");
  constexpr std::string_view kOpenedSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg"><rect id="newer-file"/></svg>)svg";
  WriteTextFile(openPath, kOpenedSvg);
  std::string error;
  ASSERT_TRUE(EditorShellTestAccess::TryOpenPath(shell, openPath.string(), &error)) << error;
  EXPECT_TRUE(EditorShellTestAccess::PendingSampleLoad(shell).empty());
  EXPECT_TRUE(EditorShellTestAccess::App(shell)
                  .document()
                  .document()
                  .querySelector("#newer-file")
                  .has_value());

  EditorShellTestAccess::QueueSampleLoad(shell, "gradients-clip");
  EditorShellTestAccess::Source(shell).setText("<svg>dirty</svg>");
  EditorShellTestAccess::App(shell).markDirty();
  EditorShellTestAccess::RequestRevert(shell);
  EXPECT_TRUE(EditorShellTestAccess::PendingSampleLoad(shell).empty());
  EXPECT_TRUE(EditorShellTestAccess::App(shell)
                  .document()
                  .document()
                  .querySelector("#newer-file")
                  .has_value());
}

TEST(EditorShellTest, RevertRequestIgnoresGuardStates) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  EditorShellTestAccess::RequestRevert(invalidShell);
  EXPECT_FALSE(EditorShellTestAccess::App(invalidShell).hasDocument());

  EditorShell cleanShell(window, OptionsWithSource(kInitialSvg, "clean.svg"));
  ASSERT_TRUE(cleanShell.valid());
  const std::string cleanSource = EditorShellTestAccess::Source(cleanShell).getText();
  EditorShellTestAccess::RequestRevert(cleanShell);
  EXPECT_EQ(EditorShellTestAccess::Source(cleanShell).getText(), cleanSource);
  EXPECT_FALSE(EditorShellTestAccess::App(cleanShell).isDirty());

  EditorShell dirtyWithoutCleanSource(window, OptionsWithSource(kInitialSvg, "dirty.svg"));
  ASSERT_TRUE(dirtyWithoutCleanSource.valid());
  EditorShellTestAccess::Source(dirtyWithoutCleanSource).setText("<svg>dirty</svg>");
  EditorShellTestAccess::App(dirtyWithoutCleanSource).setCleanSourceText("");
  EditorShellTestAccess::App(dirtyWithoutCleanSource).markDirty();

  EditorShellTestAccess::RequestRevert(dirtyWithoutCleanSource);

  EXPECT_EQ(EditorShellTestAccess::Source(dirtyWithoutCleanSource).getText(), "<svg>dirty</svg>");
  EXPECT_TRUE(EditorShellTestAccess::App(dirtyWithoutCleanSource).isDirty());
}

TEST(EditorShellTest, DiagnosticSaveHonorsShellAuthorityAndHostDestination) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  const std::filesystem::path sentinel = TempPathForTest("diagnostic-sentinel.jsonl");
  const std::filesystem::path destination = TempPathForTest("diagnostic-output.jsonl");
  {
    std::ofstream output(sentinel);
    output << "dummy user data";
    ASSERT_TRUE(output.good());
  }
  std::error_code error;
  std::filesystem::remove(destination, error);
  ASSERT_FALSE(error) << error.message();
  {
    EditorShellOptions options = OptionsWithSource(kInitialSvg);
    options.allowFileSystemActions = false;
    options.diagnosticTelemetryOutputPath = destination.string();
    EditorShell shell(window, std::move(options));
    ASSERT_TRUE(shell.valid());
    CompositorDebugPanel& panel = EditorShellTestAccess::DiagnosticPanel(shell);
    CompositorDebugPanelTestAccess::setTelemetryPath(panel, sentinel.string());
    EXPECT_FALSE(CompositorDebugPanelTestAccess::saveTelemetry(panel));
    EXPECT_FALSE(std::filesystem::exists(destination));
  }
  {
    EditorShellOptions options = OptionsWithSource(kInitialSvg);
    options.diagnosticTelemetryOutputPath = destination.string();
    EditorShell shell(window, std::move(options));
    ASSERT_TRUE(shell.valid());
    CompositorDebugPanel& panel = EditorShellTestAccess::DiagnosticPanel(shell);
    CompositorDebugPanelTestAccess::setTelemetryPath(panel, sentinel.string());
    ASSERT_TRUE(CompositorDebugPanelTestAccess::saveTelemetry(panel));
    EXPECT_TRUE(std::filesystem::exists(destination));
  }
  std::ifstream victim(sentinel);
  const std::string after((std::istreambuf_iterator<char>(victim)),
                          std::istreambuf_iterator<char>());
  EXPECT_EQ(after, "dummy user data");
  victim.close();
  std::filesystem::remove(sentinel, error);
  EXPECT_FALSE(error) << error.message();
  std::filesystem::remove(destination, error);
  EXPECT_FALSE(error) << error.message();
}

TEST(EditorShellTest, SaveRequestsUseCurrentPathAndFallbackToSaveAs) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  const std::filesystem::path savePath = TempPathForTest("request_save.svg");
  EditorShell shell(window, OptionsWithSource(kInitialSvg, savePath.string()));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::RequestSave(shell);
  EXPECT_TRUE(std::filesystem::exists(savePath));
  EXPECT_NE(ReadTextFile(savePath).find("target"), std::string::npos);

  EditorShell untitledShell(window, OptionsWithSource(kInitialSvg, ""));
  ASSERT_TRUE(untitledShell.valid());
  EditorShellTestAccess::App(untitledShell).setCurrentFilePath("");
  EditorShellTestAccess::RequestSaveAs(untitledShell, "choose a path");
  EditorShellTestAccess::RequestSave(untitledShell);
  EXPECT_TRUE(untitledShell.valid());
}

TEST(EditorShellTest, NativeViewportExportRetainsChosenPathUntilRendererIsReady) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "source.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  const std::filesystem::path exportPath = TempPathForTest("native_deferred_viewport.svg");
  std::filesystem::remove(exportPath);

  EditorShellTestAccess::RequestExportViewportSvg(shell, /*includeOverlay=*/false);
  ASSERT_TRUE(EditorShellTestAccess::SaveFileModalRequested(shell));
  EditorShellTestAccess::QueueNativeSaveSelection(shell, exportPath.string());
  ASSERT_FALSE(EditorShellTestAccess::SaveFileModalRequested(shell));
  AsyncRenderer& renderer =
      EditorShellTestAccess::BeginDelayedRender(shell, std::chrono::milliseconds(500));
  ASSERT_TRUE(renderer.isBusy());

  std::string error;
  EXPECT_EQ(EditorShellTestAccess::TryPendingNativeSave(shell, &error),
            EditorShellTestAccess::NativeSaveOutcome::Deferred);
  EXPECT_THAT(EditorShellTestAccess::PendingNativeSavePath(shell),
              testing::Optional(exportPath.string()));
  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_FALSE(EditorShellTestAccess::SaveFileModalRequested(shell));
  EXPECT_FALSE(std::filesystem::exists(exportPath));

  renderer.cancelInFlight();
  ASSERT_TRUE(renderer.waitUntilNoRenderInFlightForTesting(std::chrono::steady_clock::now() +
                                                           std::chrono::seconds(2)));
  std::ignore = renderer.pollResult();
  renderer.setReplayRenderDelayForTesting(std::chrono::milliseconds(0));

  error.clear();
  EXPECT_EQ(EditorShellTestAccess::TryPendingNativeSave(shell, &error),
            EditorShellTestAccess::NativeSaveOutcome::Saved)
      << error;
  EXPECT_FALSE(EditorShellTestAccess::PendingNativeSavePath(shell).has_value());
  EXPECT_FALSE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_FALSE(EditorShellTestAccess::SaveFileModalRequested(shell));
  ASSERT_TRUE(std::filesystem::exists(exportPath));
  EXPECT_NE(ReadTextFile(exportPath).find("target"), std::string::npos);
}

TEST(EditorShellTest, NativeViewportExportReportsPermanentErrorWithoutImGuiSaveDialog) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "source.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  EditorShellTestAccess::RequestExportViewportSvg(shell, /*includeOverlay=*/false);
  const std::filesystem::path invalidPath =
      TempPathForTest("native_save_missing_directory/viewport.svg");
  EditorShellTestAccess::QueueNativeSaveSelection(shell, invalidPath.string());

  std::string error;
  EXPECT_EQ(EditorShellTestAccess::TryPendingNativeSave(shell, &error),
            EditorShellTestAccess::NativeSaveOutcome::Failed);
  EXPECT_FALSE(error.empty());
  EXPECT_FALSE(EditorShellTestAccess::PendingNativeSavePath(shell).has_value());
  EXPECT_FALSE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_FALSE(EditorShellTestAccess::SaveFileModalRequested(shell));
}

TEST(EditorShellTest, NativeViewportExportTimesOutWithoutReopeningImGuiSaveDialog) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "source.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::RequestExportViewportSvg(shell, /*includeOverlay=*/false);
  EditorShellTestAccess::QueueNativeSaveSelection(shell, TempPathForTest("timed_out.svg").string());
  EditorShellTestAccess::ExpirePendingNativeSave(shell);

  std::string error;
  EXPECT_EQ(EditorShellTestAccess::TryPendingNativeSave(shell, &error),
            EditorShellTestAccess::NativeSaveOutcome::Failed);
  EXPECT_THAT(error, testing::HasSubstr("timed out"));
  EXPECT_FALSE(EditorShellTestAccess::PendingNativeSavePath(shell).has_value());
  EXPECT_FALSE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_FALSE(EditorShellTestAccess::SaveFileModalRequested(shell));
}

TEST(EditorShellTest, ViewportSvgExportRequestsAndWritesCroppedSvg) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  const std::filesystem::path sourcePath = TempPathForTest("source.svg");
  EditorShell shell(window, OptionsWithSource(kInitialSvg, sourcePath.string()));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  EditorShellTestAccess::RequestExportViewportSvg(shell, /*includeOverlay=*/false);
  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_FALSE(EditorShellTestAccess::PendingViewportExportOverlay(shell));

  EditorShellTestAccess::RequestExportViewportSvg(shell, /*includeOverlay=*/true, "try again");
  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExportOverlay(shell));

  const std::filesystem::path exportPath = TempPathForTest("viewport.svg");
  std::string error;
  EXPECT_TRUE(EditorShellTestAccess::TryExportViewportSvgToPath(shell, exportPath.string(), &error))
      << error;
  EXPECT_FALSE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_FALSE(EditorShellTestAccess::PendingViewportExportOverlay(shell));
  const std::string exported = ReadTextFile(exportPath);
  EXPECT_NE(exported.find("<svg"), std::string::npos);
  EXPECT_NE(exported.find("target"), std::string::npos);

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  error.clear();
  EXPECT_FALSE(
      EditorShellTestAccess::TryExportViewportSvgToPath(invalidShell, exportPath.string(), &error));
  EXPECT_EQ(error, "No document is open to export.");

  error.clear();
  EXPECT_FALSE(EditorShellTestAccess::TryExportViewportSvgToPath(
      shell, TempPathForTest("missing_directory/viewport.svg").string(), &error));
  EXPECT_FALSE(error.empty());
}

TEST(EditorShellTest, ViewportSvgExportRequestUsesUntitledDocument) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, ""));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::App(shell).setCurrentFilePath("");

  EditorShellTestAccess::RequestExportViewportSvg(shell, /*includeOverlay=*/false);

  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_FALSE(EditorShellTestAccess::PendingViewportExportOverlay(shell));
}

TEST(EditorShellTest, DiagnosticsLoggingHonorsEnvironmentTargets) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  FrameCostBreakdown frameCost;
  frameCost.overlay.captureMs = 2.0;
  frameCost.compositedUpload.uploadMs = 3.0;
  frameCost.compositedRender.cachedMs = 4.0;

  unsetenv("DONNER_EDITOR_RESOURCE_LOG");
  unsetenv("DONNER_EDITOR_FRAME_MISS_LOG");
  EditorShellTestAccess::MaybeLogResourceDiagnostics(shell, frameCost);
  EXPECT_EQ(EditorShellTestAccess::ResourceDiagnosticsFrame(shell), 0u);

  setenv("DONNER_EDITOR_RESOURCE_LOG", "1", /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogResourceDiagnostics(shell, frameCost);
  EXPECT_EQ(EditorShellTestAccess::ResourceDiagnosticsFrame(shell), 1u);
  EditorShellTestAccess::MaybeLogResourceDiagnostics(shell, frameCost);
  EXPECT_EQ(EditorShellTestAccess::ResourceDiagnosticsFrame(shell), 2u);
  for (int frame = 3; frame <= 60; ++frame) {
    EditorShellTestAccess::MaybeLogResourceDiagnostics(shell, frameCost);
  }
  EXPECT_EQ(EditorShellTestAccess::ResourceDiagnosticsFrame(shell), 60u);
  unsetenv("DONNER_EDITOR_RESOURCE_LOG");

  const std::filesystem::path telemetryPath = TempPathForTest("frame_miss.jsonl");
  std::filesystem::remove(telemetryPath);
  setenv("DONNER_EDITOR_FRAME_MISS_LOG", telemetryPath.string().c_str(), /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_FALSE(std::filesystem::exists(telemetryPath));

  EditorShellTestAccess::NoteFrameDelta(shell, 25.0f);
  setenv("DONNER_EDITOR_FRAME_MISS_LOG", "false", /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_FALSE(std::filesystem::exists(telemetryPath));

  setenv("DONNER_EDITOR_FRAME_MISS_LOG", telemetryPath.string().c_str(), /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_NE(ReadTextFile(telemetryPath).find("frame_budget_miss"), std::string::npos);

  for (std::string_view stderrTarget : {"", "1", "true", "stderr"}) {
    setenv("DONNER_EDITOR_FRAME_MISS_LOG", std::string(stderrTarget).c_str(), /*overwrite=*/1);
    EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
    EXPECT_FALSE(EditorShellTestAccess::FrameMissTelemetryWriteErrorLogged(shell));
  }

  setenv("DONNER_EDITOR_FRAME_MISS_LOG", "0", /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_FALSE(EditorShellTestAccess::FrameMissTelemetryWriteErrorLogged(shell));

  unsetenv("DONNER_EDITOR_FRAME_MISS_LOG");
  setenv("DONNER_EDITOR_RESOURCE_LOG", "1", /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_FALSE(EditorShellTestAccess::FrameMissTelemetryWriteErrorLogged(shell));
  unsetenv("DONNER_EDITOR_RESOURCE_LOG");

  const std::filesystem::path badTelemetryPath =
      TempPathForTest("missing_directory/frame_miss.jsonl");
  setenv("DONNER_EDITOR_FRAME_MISS_LOG", badTelemetryPath.string().c_str(), /*overwrite=*/1);
  EditorShellTestAccess::MaybeLogFrameMissTelemetry(shell, frameCost);
  EXPECT_TRUE(EditorShellTestAccess::FrameMissTelemetryWriteErrorLogged(shell));
  unsetenv("DONNER_EDITOR_FRAME_MISS_LOG");
}

TEST(EditorShellTest, SourceFocusAndStyleDecorationsTrackSelectionAndDirtySource) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  EXPECT_TRUE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));
  EditorShellTestAccess::SetSourceFocusMode(shell, true);
  EXPECT_TRUE(EditorShellTestAccess::SourceFocusMode(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).hasFocusPartition());

  EXPECT_TRUE(WaitForStyleSourceDecorations(shell));
  EXPECT_GT(EditorShellTestAccess::StyleSourceContributionCount(shell), 0u);
  EXPECT_GT(EditorShellTestAccess::Source(shell).sourceStyleDecorations().size(), 0u);

  const std::size_t contributionCount = EditorShellTestAccess::StyleSourceContributionCount(shell);
  EditorShellTestAccess::UpdateSourceStyleDecorations(shell);
  EXPECT_EQ(EditorShellTestAccess::StyleSourceContributionCount(shell), contributionCount);

  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.setSelection(Coordinates(0, 0), Coordinates(0, 0));
  source.insertText(" ");
  EditorShellTestAccess::UpdateSourceStyleDecorations(shell);
  EXPECT_FALSE(EditorShellTestAccess::StyleSourceDecorationsValid(shell));
  EXPECT_TRUE(source.sourceStyleDecorations().empty());
}

TEST(EditorShellTest, StaleTransformAnnotationResultKeepsRefreshPending) {
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_THAT(window.valid(), testing::IsTrue());
  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_THAT(shell.valid(), testing::IsTrue());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  const std::uint64_t revision = app.document().nonTransformRevision();
  EditorShellTestAccess::StageReadyStyleAnnotationsForCurrentSource(shell);
  const std::uint64_t sourceVersion = app.document().document().sourceVersion();
  app.applyMutation(
      EditorCommand::SetTransformCommand(*target, Transform2d::Translate(Vector2d(1, 0))));
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  // The typed mutation advances the rendered model; authored source follows at writeback.
  EXPECT_THAT(app.document().nonTransformRevision(), testing::Eq(revision));
  const auto writeback =
      app.document().document().setElementAttribute(*target, "transform", "translate(1)");
  ASSERT_THAT(writeback.applied, testing::IsTrue());
  EXPECT_THAT(app.document().document().sourceVersion(), testing::Gt(sourceVersion));
  EXPECT_THAT(app.document().nonTransformRevision(), testing::Eq(revision));
  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.setText(internal::CanonicalizeForTextEditor(app.document().document().source()));
  source.resetTextChanged();
  source.beginSourceStyleDecorationRefresh(app.document().documentGeneration(), revision);
  EditorShellTestAccess::UpdateSourceStyleDecorations(shell);
  EXPECT_THAT(source.setSourceStyleDecorations({}), testing::IsTrue())
      << "Discarding only an obsolete transform-source result must keep the refresh pending";

  app.applyMutation(EditorCommand::SetAttributeCommand(*target, "id", "changed-target"));
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  EXPECT_THAT(app.document().nonTransformRevision(), testing::Gt(revision));
}

TEST(EditorShellTest, SourceStyleDecorationsDiscardReplacedDocumentResult) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  ASSERT_TRUE(WaitForStyleSourceDecorations(shell));
  ASSERT_GT(EditorShellTestAccess::StyleSourceContributionCount(shell), 0u);

  ASSERT_TRUE(EditorShellTestAccess::App(shell).document().loadFromString(kStyledSvg));
  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.setText(internal::CanonicalizeForTextEditor(kStyledSvg));
  source.resetTextChanged();
  EditorShellTestAccess::UpdateSourceStyleDecorations(shell);
  EXPECT_FALSE(EditorShellTestAccess::StyleSourceDecorationsValid(shell));
  EXPECT_TRUE(source.sourceStyleDecorations().empty());

  constexpr std::string_view kReplacementSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg">
  <g id="replacement"/>
</svg>
)svg";
  ASSERT_TRUE(EditorShellTestAccess::App(shell).document().loadFromString(kReplacementSvg));
  source.setText(internal::CanonicalizeForTextEditor(kReplacementSvg));
  source.resetTextChanged();

  ASSERT_TRUE(WaitForStyleSourceDecorations(shell));
  EXPECT_EQ(EditorShellTestAccess::StyleSourceDecorationDocumentGeneration(shell),
            EditorShellTestAccess::App(shell).document().documentGeneration());
  EXPECT_EQ(EditorShellTestAccess::StyleSourceContributionCount(shell), 0u);
  EXPECT_TRUE(source.sourceStyleDecorations().empty());
}

TEST(EditorShellTest, ValidStyleSourceEditRequestsCanvasRenderWithoutCanvasClick) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_THAT(shell.valid(), testing::Eq(true));

  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.resetTextChanged();
  const std::size_t declarationOffset = source.getText().find("fill: red");
  ASSERT_THAT(declarationOffset, testing::Ne(std::string::npos));
  const std::size_t colorOffset = declarationOffset + 6u;
  source.setSelection(source.getCoordinatesAtByteOffset(colorOffset),
                      source.getCoordinatesAtByteOffset(colorOffset + 3u));
  source.insertText("blue");
  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  const std::uint64_t versionBefore =
      EditorShellTestAccess::App(shell).document().currentFrameVersion();

  window.beginFrame();
  ImGuiIO& io = ImGui::GetIO();
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  EditorShellTestAccess::RenderSourcePane(shell, /*paneOriginY=*/0.0f, /*paneHeight=*/180.0f,
                                          /*paneWidth=*/260.0f, io.Fonts->Fonts[0]);
  window.endFrame();

  EXPECT_THAT(EditorShellTestAccess::App(shell).document().currentFrameVersion(),
              testing::Gt(versionBefore));
  const auto target =
      EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  EXPECT_THAT(target->getComputedStyle().fill.get(),
              testing::Optional(svg::PaintServer(
                  svg::PaintServer::Solid(css::Color(css::RGBA(0, 0, 0xFF, 0xFF))))));
  EXPECT_THAT(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell), testing::Eq(true));

  RunShellFrame(window, shell);
  ASSERT_TRUE(shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(3)));
  RunShellFrame(window, shell);
  EXPECT_THAT(EditorShellTestAccess::DisplayedDocVersion(shell),
              testing::Eq(EditorShellTestAccess::App(shell).document().currentFrameVersion()));
}

TEST(EditorShellTest, TypingStylesheetColorKeepsSourceFocusAndReferenceRopes) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_THAT(shell.valid(), testing::Eq(true));

  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.resetTextChanged();
  const std::size_t declarationOffset = source.getText().find("fill: red");
  ASSERT_THAT(declarationOffset, testing::Ne(std::string::npos));
  const std::size_t colorOffset = declarationOffset + 6u;
  source.setCursorPosition(source.getCoordinatesAtByteOffset(colorOffset));
  std::optional<StyleFocus> styleFocus =
      EditorShellTestAccess::StyleFocusAtSourceOffset(shell, colorOffset);
  ASSERT_THAT(styleFocus, testing::Optional(testing::_));
  ASSERT_THAT(styleFocus->partition.referenceLinks.empty(), testing::Eq(false));
  EditorShellTestAccess::ApplyStyleFocus(shell, std::move(*styleFocus));
  EditorShellTestAccess::SetSourceFocusMode(shell, true);
  ASSERT_THAT(source.hasFocusPartition(), testing::Eq(true));

  source.setSelection(source.getCoordinatesAtByteOffset(colorOffset),
                      source.getCoordinatesAtByteOffset(colorOffset + 3u));
  for (const char ch : std::string_view("blue")) {
    source.insertText(std::string(1, ch));
    window.beginFrame();
    ImGuiIO& io = ImGui::GetIO();
    if (!io.Fonts->IsBuilt()) {
      io.Fonts->Build();
    }
    EditorShellTestAccess::RenderSourcePane(shell, /*paneOriginY=*/0.0f,
                                            /*paneHeight=*/180.0f, /*paneWidth=*/260.0f,
                                            io.Fonts->Fonts[0]);
    window.endFrame();
    RunShellFrame(window, shell);
  }

  ASSERT_TRUE(shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(3)));
  RunShellFrame(window, shell);
  EXPECT_THAT(EditorShellTestAccess::DisplayedDocVersion(shell),
              testing::Eq(EditorShellTestAccess::App(shell).document().currentFrameVersion()));
  EXPECT_THAT(source.getText(), testing::HasSubstr("fill: blue"));
  EXPECT_THAT(std::string(EditorShellTestAccess::App(shell).document().document().source()),
              testing::HasSubstr("fill: blue"));
  const std::optional<svg::SVGElement> targetAfter =
      EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_THAT(targetAfter, testing::Optional(testing::_));
  EXPECT_THAT(targetAfter->getComputedStyle().fill.get(),
              testing::Optional(svg::PaintServer(
                  svg::PaintServer::Solid(css::Color(css::RGBA(0, 0, 255, 255))))));
  EXPECT_THAT(source.hasFocusPartition(), testing::Eq(true));
  EXPECT_THAT(EditorShellTestAccess::SourceFocusOriginatedInStyle(shell), testing::Eq(true));
  EXPECT_THAT(source.isTextChanged(), testing::Eq(false));
  EXPECT_THAT(EditorShellTestAccess::App(shell).document().hasPendingMutations(),
              testing::Eq(false));
  const std::size_t cursorOffset = source.getByteOffsetAtCoordinates(source.getCursorPosition());
  EXPECT_THAT(cursorOffset,
              testing::AllOf(testing::Ge(colorOffset), testing::Le(colorOffset + 4u)));
  EXPECT_THAT(EditorShellTestAccess::StyleFocusAtSourceOffset(shell, cursorOffset),
              testing::Optional(testing::_));
  const std::optional<StyleFocus> focusAfter =
      EditorShellTestAccess::StyleFocusAtSourceCursor(shell);
  ASSERT_THAT(focusAfter, testing::Optional(testing::_));
  EXPECT_THAT(focusAfter->partition.referenceLinks.empty(), testing::Eq(false));
}

TEST(EditorShellTest, SelectedSplashStylesheetEditPresentsWithoutCanvasClick) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  const EditorSample* splash = FindEditorSample("donner-splash");
  ASSERT_NE(splash, nullptr);
  std::string sourceText(splash->source);
  const std::size_t ruleOffset = sourceText.find(".cls-5 {");
  ASSERT_NE(ruleOffset, std::string::npos);
  const std::size_t originalColorOffset = sourceText.find("url(#linear-gradient)", ruleOffset);
  ASSERT_NE(originalColorOffset, std::string::npos);
  sourceText.replace(originalColorOffset, std::string_view("url(#linear-gradient)").size(), "red");

  EditorShell shell(window, OptionsWithSource(sourceText, "donner_splash.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const std::optional<svg::SVGElement> letter =
      app.document().document().querySelector("#Donner_D");
  ASSERT_THAT(letter, testing::Optional(testing::_));
  app.setSelection(*letter);

  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.resetTextChanged();
  const std::size_t declarationOffset = source.getText().find("fill: red", ruleOffset);
  ASSERT_NE(declarationOffset, std::string::npos);
  const std::size_t colorOffset = declarationOffset + 6u;
  source.setCursorPosition(source.getCoordinatesAtByteOffset(colorOffset));
  std::optional<StyleFocus> styleFocus =
      EditorShellTestAccess::StyleFocusAtSourceOffset(shell, colorOffset);
  ASSERT_THAT(styleFocus, testing::Optional(testing::_));
  EditorShellTestAccess::ApplyStyleFocus(shell, std::move(*styleFocus));
  EditorShellTestAccess::SetSourceFocusMode(shell, true);
  source.setSelection(source.getCoordinatesAtByteOffset(colorOffset),
                      source.getCoordinatesAtByteOffset(colorOffset + 3u));
  source.insertText("green");
  const std::uint64_t before = app.document().currentFrameVersion();

  window.beginFrame();
  ImGuiIO& io = ImGui::GetIO();
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  EditorShellTestAccess::RenderSourcePane(shell, /*paneOriginY=*/0.0f, /*paneHeight=*/180.0f,
                                          /*paneWidth=*/260.0f, io.Fonts->Fonts[0]);
  window.endFrame();
  EXPECT_GT(app.document().currentFrameVersion(), before);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  for (const tests::HangGuard guard; guard.active();) {
    RunShellFrame(window, shell);
    if (EditorShellTestAccess::DisplayedDocVersion(shell) == app.document().currentFrameVersion()) {
      break;
    }
    ASSERT_TRUE(
        shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(guard.deadline()));
  }
  EXPECT_EQ(EditorShellTestAccess::DisplayedDocVersion(shell),
            app.document().currentFrameVersion());
  const std::optional<svg::SVGElement> updatedLetter =
      app.document().document().querySelector("#Donner_D");
  ASSERT_THAT(updatedLetter, testing::Optional(testing::_));
  EXPECT_THAT(updatedLetter->getComputedStyle().fill.get(),
              testing::Optional(svg::PaintServer(
                  svg::PaintServer::Solid(css::Color(css::RGBA(0, 128, 0, 255))))));
}

TEST(EditorShellTest, StyleFocusCursorAndPartitionGuards) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());

  const std::string sourceText = EditorShellTestAccess::Source(shell).getText();
  const std::size_t fillOffset = sourceText.find("fill");
  ASSERT_NE(fillOffset, std::string::npos);
  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.setCursorPosition(source.getCoordinatesAtByteOffset(fillOffset));

  std::optional<StyleFocus> cursorFocus = EditorShellTestAccess::StyleFocusAtSourceCursor(shell);
  ASSERT_TRUE(cursorFocus.has_value());
  EXPECT_FALSE(cursorFocus->impactedElements.empty());

  EXPECT_FALSE(EditorShellTestAccess::StyleFocusAtSourceOffset(shell, 1).has_value());

  EditorShellTestAccess::SetSourceFocusMode(shell, false);
  EditorShellTestAccess::ApplySourcePartition(
      shell, FocusPartition{
                 .fullColor = {{.startLine = 1, .endLine = 2}},
                 .referenceColor = {{.startLine = 2, .endLine = 3}},
                 .dimmed = {{.startLine = 0, .endLine = 1}},
                 .hidden = {{.startLine = 3, .endLine = 4}},
             });
  EXPECT_TRUE(source.hasFocusPartition());

  source.insertText(" ");
  EXPECT_FALSE(EditorShellTestAccess::StyleFocusAtSourceCursor(shell).has_value());
}

TEST(EditorShellTest, RenderContextMenuOpenStoresPointAndHitElement) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::OpenRenderPaneContextMenu(shell, Vector2d(12.0, 14.0));

  EXPECT_TRUE(EditorShellTestAccess::RenderContextMenuOpenRequested(shell));
  ASSERT_TRUE(EditorShellTestAccess::RenderContextMenuDocumentPoint(shell).has_value());
  EXPECT_EQ(*EditorShellTestAccess::RenderContextMenuDocumentPoint(shell), Vector2d(12.0, 14.0));
  EXPECT_TRUE(EditorShellTestAccess::RenderContextMenuHasHitElement(shell));

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  EditorShellTestAccess::OpenRenderPaneContextMenu(invalidShell, Vector2d(1.0, 1.0));
  EXPECT_TRUE(EditorShellTestAccess::RenderContextMenuOpenRequested(invalidShell));
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuHasHitElement(invalidShell));
}

TEST(EditorShellTest, RenderContextMenuRendersHitEmptyAndNoDocumentStates) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::OpenRenderPaneContextMenu(shell, Vector2d(12.0, 14.0));
  RenderContextMenuFrame(window, shell);
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuOpenRequested(shell));
  EXPECT_TRUE(EditorShellTestAccess::RenderContextMenuHasHitElement(shell));

  EditorShellTestAccess::OpenRenderPaneContextMenu(shell, Vector2d(400.0, 300.0));
  RenderContextMenuFrame(window, shell);
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuOpenRequested(shell));
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuHasHitElement(shell));

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  EditorShellTestAccess::OpenRenderPaneContextMenu(invalidShell, Vector2d(1.0, 1.0));
  RenderContextMenuFrame(window, invalidShell);
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuOpenRequested(invalidShell));
  EXPECT_FALSE(EditorShellTestAccess::RenderContextMenuHasHitElement(invalidShell));
}

TEST(EditorShellTest, DocumentSpaceReplayInputSelectsHitElementAndHandlesGuards) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  EXPECT_TRUE(EditorShellTestAccess::App(shell).selectedElements().empty());

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  invalidShell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 14.0),
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(invalidShell);
  EXPECT_TRUE(EditorShellTestAccess::App(invalidShell).selectedElements().empty());

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 14.0),
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 14.0),
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(20.0, 30.0),
      .hitElementId = std::string(),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");
}

TEST(EditorShellTest, DocumentSpaceReplayInputRoutesSelectPressAndRelease) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 14.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(24.0, 22.0),
      .leftMouseDown = true,
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 14.0),
      .leftMouseReleased = true,
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));
}

void RunFullDesktopFrameLoopPresentsShapeDragBeforeMouseUp(double contentScale) {
  gui::EditorWindow window(TestWindowOptions(
      {
          .title = "Donner full desktop drag regression",
          .initialWidth = 1200,
          .initialHeight = 800,
      },
      contentScale));
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  ExpectTestWindowScale(window, Vector2i(1200, 800), contentScale);

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  std::optional<svg::SVGElement> target =
      EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_THAT(target, ::testing::Optional(::testing::_));
  // Establish the selected-layer cache before measuring the drag handoff. The contract below is
  // same-frame rebasing of already-promoted pixels, not completion within a fixed number of
  // back-to-back frames by the asynchronous worker that creates those pixels.
  EditorShellTestAccess::App(shell).setSelection(*target);
  const auto runFrameWithMouse = [&](const ImVec2& mouse, bool mouseDown) {
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(mouse.x, mouse.y);
    io.AddMouseButtonEvent(0, mouseDown);
    window.beginFrame();
    shell.runFrame();
    window.endFrame();
  };
  const tests::HangGuard prewarmGuard;
  for (int frame = 0; prewarmGuard.active(); ++frame) {
    runFrameWithMouse(ImVec2(-20.0f, -20.0f), /*mouseDown=*/false);
    if (!EditorShellTestAccess::RendererBusy(shell) && frame >= 2) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_THAT(shell.layerInspectorStatusForReadback().displayedDragPreview,
              ::testing::Optional(::testing::_))
      << "The drag assertion requires a promoted selected-layer cache";

  const auto screenPoint = [&](const Vector2d& documentPoint) {
    const Vector2d screen = shell.viewportForReadback().documentToScreen(documentPoint);
    return ImVec2(static_cast<float>(screen.x), static_cast<float>(screen.y));
  };
  runFrameWithMouse(screenPoint(Vector2d(20.0, 20.0)), /*mouseDown=*/false);
  runFrameWithMouse(screenPoint(Vector2d(20.0, 20.0)), /*mouseDown=*/true);
  runFrameWithMouse(screenPoint(Vector2d(25.0, 23.0)), /*mouseDown=*/true);
  runFrameWithMouse(screenPoint(Vector2d(35.0, 28.0)), /*mouseDown=*/true);

  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value())
      << "pendingClick=" << EditorShellTestAccess::HasPendingClick(shell)
      << " rendererBusy=" << EditorShellTestAccess::RendererBusy(shell);
  ASSERT_EQ(EditorShellTestAccess::App(shell).selectedElement()->id(), "target");
  EXPECT_THAT(EditorShellTestAccess::SelectionChromePathCount(shell), ::testing::Gt(0u))
      << "The held drag must present the selected path alongside its bounds.";
  const LayerInspectorStatusReadback status = shell.layerInspectorStatusForReadback();
  ASSERT_TRUE(status.activeDragPreview.has_value())
      << "The full desktop frame loop must retain a live drag preview";
  EXPECT_EQ(status.activeDragPreview->translation, Vector2d(15.0, 8.0));
  ASSERT_TRUE(status.displayedDragPreview.has_value())
      << "A promoted drag tile must retain the worker epoch its pixels represent";
  constexpr Vector2d kTargetProbePoint(10.0, 12.0);
  const Vector2d expectedLiveProbe =
      status.activeDragPreview->documentFromCachedDocument.transformPosition(kTargetProbePoint);
  const bool hasLiveDragTile =
      std::ranges::any_of(status.tiles, [&](const LayerInspectorStatusReadback::Tile& tile) {
        if (!tile.isDragTarget) {
          return false;
        }
        const Vector2d presentedProbe =
            tile.presentedDocumentFromCachedDocument.transformPosition(kTargetProbePoint);
        return std::abs(presentedProbe.x - expectedLiveProbe.x) < 1e-6 &&
               std::abs(presentedProbe.y - expectedLiveProbe.y) < 1e-6;
      });
  std::ostringstream tileDiagnostics;
  for (const LayerInspectorStatusReadback::Tile& tile : status.tiles) {
    tileDiagnostics << "\n  id=" << tile.id << " dragTarget=" << tile.isDragTarget
                    << " cachedTranslation=(" << tile.documentFromCachedDocument.data[4] << ", "
                    << tile.documentFromCachedDocument.data[5] << ") presentedTranslation=("
                    << tile.presentedDocumentFromCachedDocument.data[4] << ", "
                    << tile.presentedDocumentFromCachedDocument.data[5] << ")";
  }
  EXPECT_TRUE(hasLiveDragTile)
      << "At least one cached shape tile must rebase its represented worker epoch onto the live "
         "drag transform before mouse-up; tiles:"
      << tileDiagnostics.str();

  runFrameWithMouse(screenPoint(Vector2d(35.0, 28.0)), /*mouseDown=*/false);
}

TEST(EditorShellTest, FullDesktopFrameLoopPresentsShapeDragBeforeMouseUp) {
  RunFullDesktopFrameLoopPresentsShapeDragBeforeMouseUp(kTestContentScale);
}

// The rest of this suite renders at 2x by design (see `kTestContentScale`). This 1x cross-section
// is a deliberate regression check for standard-density displays: startup and first present, a
// held select-and-drag that presents a live tile before mouse-up, and the docked source pane and
// overlay states. Keep it small; scale-sensitive coverage belongs in the 2x suite.
TEST(EditorShell1xSmokeTest, HiddenWindowShellConstructsAndRunsFrames) {
  RunHiddenWindowShellConstructsAndRunsFrames(/*contentScale=*/1.0);
}

TEST(EditorShell1xSmokeTest, FullDesktopFrameLoopPresentsShapeDragBeforeMouseUp) {
  RunFullDesktopFrameLoopPresentsShapeDragBeforeMouseUp(/*contentScale=*/1.0);
}

TEST(EditorShell1xSmokeTest, FullFrameSmokeCoversPanelSourceAndContextMenuStates) {
  RunFullFrameSmokeCoversPanelSourceAndContextMenuStates(/*contentScale=*/1.0);
}

namespace {

bool CachedRasterCoversVisibleArtboard(EditorShell& shell, const ViewportState& viewport,
                                       bool requireRequestedDensity) {
  const auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
  if (frame == nullptr || frame->tiles().empty()) {
    return false;
  }
  const Box2d pane = viewport.screenToDocument(
      Box2d(viewport.paneOrigin, viewport.paneOrigin + viewport.paneSize));
  const Box2d visible(
      Vector2d(std::max(pane.topLeft.x, viewport.documentViewBox.topLeft.x),
               std::max(pane.topLeft.y, viewport.documentViewBox.topLeft.y)),
      Vector2d(std::min(pane.bottomRight.x, viewport.documentViewBox.bottomRight.x),
               std::min(pane.bottomRight.y, viewport.documentViewBox.bottomRight.y)));
  const Box2d rect = frame->rasterDocumentRect();
  const Vector2i pixels = frame->rasterOutputSizePx();
  if (rect.width() <= 0.0 || rect.height() <= 0.0 || pixels.x <= 0 || pixels.y <= 0 ||
      visible.width() <= 0.0 || visible.height() <= 0.0 || !rect.contains(visible.topLeft) ||
      !rect.contains(visible.bottomRight)) {
    return false;
  }
  if (!requireRequestedDensity) {
    return true;
  }
  const EditorRasterViewport raster = viewport.rasterViewport();
  const Vector2d requiredScale(raster.outputSizePx.x / raster.documentRect.width(),
                               raster.outputSizePx.y / raster.documentRect.height());
  return pixels.x / rect.width() == requiredScale.x && pixels.y / rect.height() == requiredScale.y;
}

}  // namespace

TEST(EditorShellTest, PartlyOffscreenSplashShineEllipseFirstHeldMoveMatchesSettledPixels) {
  gui::EditorWindow window(TestWindowOptions({
      .title = "Partly offscreen splash drag pixels",
      .initialWidth = 960,
      .initialHeight = 720,
      .forceOffscreenRenderTarget = true,
      .enableFramebufferReadback = true,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "Framebuffer readback is unavailable on this host";
  }
  ExpectTestWindowScale(window, Vector2i(960, 720), kTestContentScale);
  const EditorSample* splash = FindEditorSample("donner-splash");
  ASSERT_NE(splash, nullptr);
  EditorShell shell(window, OptionsWithSource(splash->source, "donner_splash.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  // A canvas click hits the child ellipse. Explicit group promotion from the Layers panel is
  // covered separately by the compositor test for #Background_shine.
  const auto shine = app.document().document().querySelector("#Background_shine ellipse");
  ASSERT_THAT(shine, testing::Optional(testing::_));
  const Entity shineEntity = shine->unsafeEntityHandle().entity();
  const std::optional<Box2d> originalWorldBounds =
      shine->cast<svg::SVGGeometryElement>().worldBounds();
  ASSERT_TRUE(originalWorldBounds.has_value());

  const auto captureFrame = [&](bool contentOnly = false) {
    if (contentOnly) {
      shell.setContentOnlyCaptureForNextFrameForReplay(true);
    }
    window.beginFrame();
    shell.runFrame();
    return window.endFrameAndReadPixels();
  };
  const auto settleFrame = [&](std::string_view stage, std::uint64_t minimumVersion,
                               bool requireSelectedCache = false,
                               bool requireRequestedDensity = true) {
    const tests::HangGuard guard;
    for (int frame = 0; guard.active(); ++frame) {
      window.beginFrame();
      shell.runFrame();
      window.endFrame();
      if (!shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(guard.deadline())) {
        ADD_FAILURE() << "The splash render did not complete within the hang guard at " << stage
                      << " frame=" << frame
                      << " rendererBusy=" << shell.asyncRendererForReplay().isBusy()
                      << " renderInFlight="
                      << shell.asyncRendererForReplay().hasRenderInFlightForTesting()
                      << " heldResultPolls="
                      << shell.asyncRendererForReplay().replayResultHoldPollCountForTesting();
        return false;
      }
      if (requireSelectedCache && frame == 0) {
        // Selected prewarming requires the 120ms viewport/canvas setup window to have settled.
        RenderCoordinator& coordinator = EditorShellTestAccess::Coordinator(shell);
        RenderCoordinatorTestAccess::makeCanvasCommitDue(coordinator);
        RenderCoordinatorTestAccess::makeRasterViewportSettled(coordinator);
        continue;
      }
      if (!app.document().hasPendingMutations() && !shell.asyncRendererForReplay().isBusy() &&
          app.document().currentFrameVersion() >= minimumVersion &&
          EditorShellTestAccess::DisplayedDocVersion(shell) ==
              app.document().currentFrameVersion() &&
          CachedRasterCoversVisibleArtboard(shell, shell.viewportForReadback(),
                                            requireRequestedDensity) &&
          (!requireSelectedCache || EditorShellTestAccess::Coordinator(shell)
                                        .compositedPresentation()
                                        .hasCachedTexturesForEntity(shineEntity))) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const LayerInspectorStatusReadback status = shell.layerInspectorStatusForReadback();
    const auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
    ADD_FAILURE() << "The splash did not produce a stable cached frame at " << stage
                  << ": minimumVersion=" << minimumVersion
                  << " current=" << app.document().currentFrameVersion()
                  << " displayed=" << EditorShellTestAccess::DisplayedDocVersion(shell)
                  << " tiles=" << status.tiles.size()
                  << " rendererBusy=" << EditorShellTestAccess::RendererBusy(shell)
                  << " pendingMutations=" << app.document().hasPendingMutations()
                  << " renderInFlight="
                  << shell.asyncRendererForReplay().hasRenderInFlightForTesting()
                  << " heldResultPolls="
                  << shell.asyncRendererForReplay().replayResultHoldPollCountForTesting()
                  << " requireRequestedDensity=" << requireRequestedDensity
                  << " presentedRect=" << (frame != nullptr ? frame->rasterDocumentRect() : Box2d())
                  << " presentedPixels="
                  << (frame != nullptr ? frame->rasterOutputSizePx() : Vector2i::Zero())
                  << " presentedOverview=" << (frame != nullptr && frame->usesOverviewRaster())
                  << " requestedRect=" << shell.viewportForReadback().rasterViewport().documentRect
                  << " requestedPixels="
                  << shell.viewportForReadback().rasterViewport().outputSizePx;
    return false;
  };
  ASSERT_TRUE(settleFrame("initial", app.document().currentFrameVersion()));
  // The splash is larger than this canvas, so it opens fitted; Actual Size leaves it partly
  // offscreen, the view this regression needs.
  MenuBarActions actualSize;
  actualSize.actualSize = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actualSize);
  ASSERT_DOUBLE_EQ(shell.viewportForReadback().zoom, 1.0);
  ASSERT_TRUE(settleFrame("actual size", app.document().currentFrameVersion()));
  app.setSelection(*shine);
  ASSERT_THAT(settleFrame("selected", app.document().currentFrameVersion(),
                          /*requireSelectedCache=*/true),
              testing::Eq(true));

  const ViewportState viewport = shell.viewportForReadback();
  const EditorRasterViewport visibleRaster = viewport.rasterViewport();
  ASSERT_TRUE(visibleRaster.viewportBounded);
  ASSERT_NE(visibleRaster.outputSizePx, viewport.selectedPrewarmRasterViewport().outputSizePx)
      << "The regression needs a small pane whose selected prewarm would change tile dimensions";
  EXPECT_THAT(CachedRasterCoversVisibleArtboard(shell, viewport, /*requireRequestedDensity=*/true),
              testing::Eq(true))
      << "Selecting the ellipse must retain complete visible coverage at the requested density; "
      << RenderCoordinatorTestAccess::overviewScheduleState(
             EditorShellTestAccess::Coordinator(shell), app, shell.viewportForReadback());
  const std::optional<Box2d> originalChromeBounds =
      EditorShellTestAccess::SelectionChromeFirstBoundsDoc(shell);
  ASSERT_TRUE(originalChromeBounds.has_value());
  const svg::RendererBitmap beforeMove = captureFrame(/*contentOnly=*/true);
  ASSERT_FALSE(beforeMove.empty());

  // Background_shine is an ellipse centered at y=10 with radius 262: its source pixels above
  // y=0 are initially off-artboard. Moving it down by 300 brings those pixels into the artboard.
  // Hold the worker result so this screenshot is precisely the first pointer presentation of
  // previously cached target pixels, with no newly rasterized frame to hide a clipped tile.
  EditorShellTestAccess::HoldRenderResultsForPolls(shell, 12);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(300.0, 80.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  (void)captureFrame();
  ASSERT_THAT(app.selectedElement(), testing::Optional(testing::_));
  ASSERT_EQ(app.selectedElement()->unsafeEntityHandle().entity(), shineEntity);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(300.0, 380.0),
      .leftMouseDown = true,
  });
  (void)captureFrame();
  ASSERT_THAT(app.selectedElement(), testing::Optional(testing::_));
  ASSERT_EQ(app.selectedElement()->unsafeEntityHandle().entity(), shineEntity);
  const LayerInspectorStatusReadback firstStatus = shell.layerInspectorStatusForReadback();
  ASSERT_THAT(firstStatus.activeDragPreview, testing::Optional(testing::_));
  EXPECT_NEAR(firstStatus.activeDragPreview->translation.y, 300.0, 1e-6);
  EXPECT_GT(EditorShellTestAccess::SelectionChromePathCount(shell), 0u)
      << "The first moved pixels must keep the selection outline aligned with the live shape";
  const std::optional<Box2d> firstChromeBounds =
      EditorShellTestAccess::SelectionChromeFirstBoundsDoc(shell);
  ASSERT_TRUE(firstChromeBounds.has_value());
  EXPECT_NEAR(firstChromeBounds->topLeft.x, originalChromeBounds->topLeft.x, 1e-6);
  EXPECT_NEAR(firstChromeBounds->bottomRight.x, originalChromeBounds->bottomRight.x, 1e-6);
  EXPECT_NEAR(firstChromeBounds->topLeft.y - originalChromeBounds->topLeft.y, 300.0, 1e-6);
  EXPECT_NEAR(firstChromeBounds->bottomRight.y - originalChromeBounds->bottomRight.y, 300.0, 1e-6);
  const svg::RendererBitmap firstMove = captureFrame(/*contentOnly=*/true);
  ASSERT_THAT(firstMove.empty(), testing::IsFalse());

  shell.asyncRendererForReplay().releaseReplayResultHoldForTesting();
  const std::uint64_t heldDragVersion = app.document().currentFrameVersion();
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(300.0, 380.0),
      .leftMouseReleased = true,
  });
  (void)captureFrame();
  ASSERT_THAT(settleFrame("released", heldDragVersion, /*requireSelectedCache=*/false,
                          /*requireRequestedDensity=*/false),
              testing::Eq(true));
  const std::optional<svg::SVGElement> movedShine =
      app.document().document().querySelector("#Background_shine ellipse");
  ASSERT_TRUE(movedShine.has_value());
  const std::optional<Box2d> movedWorldBounds =
      movedShine->cast<svg::SVGGeometryElement>().worldBounds();
  ASSERT_TRUE(movedWorldBounds.has_value());
  EXPECT_NEAR(movedWorldBounds->topLeft.x, originalWorldBounds->topLeft.x, 1e-6);
  EXPECT_NEAR(movedWorldBounds->bottomRight.x, originalWorldBounds->bottomRight.x, 1e-6);
  EXPECT_NEAR(movedWorldBounds->topLeft.y - originalWorldBounds->topLeft.y, 300.0, 1e-6)
      << " pendingMutations=" << app.document().hasPendingMutations()
      << " rendererBusy=" << shell.asyncRendererForReplay().isBusy()
      << " renderInFlight=" << shell.asyncRendererForReplay().hasRenderInFlightForTesting()
      << " heldResultPolls="
      << shell.asyncRendererForReplay().replayResultHoldPollCountForTesting();
  EXPECT_NEAR(movedWorldBounds->bottomRight.y - originalWorldBounds->bottomRight.y, 300.0, 1e-6);
  const svg::RendererBitmap settled = captureFrame(/*contentOnly=*/true);
  ASSERT_EQ(beforeMove.dimensions, firstMove.dimensions);
  ASSERT_EQ(firstMove.dimensions, settled.dimensions);

  const auto cropDocumentRect = [&](const svg::RendererBitmap& bitmap, const Box2d& documentRect) {
    return CropDocumentRect(bitmap, viewport, window.displayScale(), documentRect);
  };
  // The unobscured upper sky contains visible source pixels removed by this move. Pixelmatch
  // establishes both that the crop changed and that the first held frame already matches the
  // settled composition; the compositor test checks the newly exposed off-artboard source rows.
  const Box2d upperSky = Box2d::FromXYWH(270.0, 40.0, 70.0, 50.0);
  const svg::RendererBitmap beforeSky = cropDocumentRect(beforeMove, upperSky);
  const svg::RendererBitmap firstSky = cropDocumentRect(firstMove, upperSky);
  const svg::RendererBitmap settledSky = cropDocumentRect(settled, upperSky);
  ASSERT_FALSE(beforeSky.empty());
  ASSERT_FALSE(firstSky.empty());
  ASSERT_FALSE(settledSky.empty());
  tests::BitmapGoldenCompareParams signalParams = tests::PixelmatchIdentityParams();
  signalParams.maxMismatchedPixels = std::numeric_limits<int>::max();
  int movedSkyPixels = -1;
  tests::CompareBitmapToBitmap(beforeSky, settledSky, "shine_upper_sky_changed", signalParams,
                               &movedSkyPixels);
  EXPECT_GT(movedSkyPixels, 25) << "The moved shine must visibly change the upper-sky crop";
  // The cached drag tile and a fresh post-release raster can differ along antialiased edges;
  // apply the renderer suite's standard pixelmatch threshold while keeping the mismatch budget
  // far below the changed-sky signal.
  tests::CompareBitmapToBitmap(firstSky, settledSky, "shine_first_held_frame_matches_settled",
                               tests::ApprovedPixelToleranceParams(0.02f, 100));
  if (const char* outputDir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const std::array<std::pair<const char*, const svg::RendererBitmap*>, 3> screenshots = {
        std::pair{"before.png", &beforeMove}, std::pair{"first.png", &firstMove},
        std::pair{"settled.png", &settled}};
    for (const auto& [name, bitmap] : screenshots) {
      const std::filesystem::path path = std::filesystem::path(outputDir) / name;
      (void)svg::RendererImageIO::writeRgbaPixelsToPngFile(
          path.string().c_str(), bitmap->pixels, bitmap->dimensions.x, bitmap->dimensions.y,
          bitmap->rowBytes / 4u);
    }
  }
  // Full-object target tiles may extend outside the artboard; the editor must still clip the
  // composed document at the SVG root. A crop below the artboard stays on the checkerboard.
  const Box2d belowArtboard = Box2d::FromXYWH(460.0, 535.0, 10.0, 10.0);
  const svg::RendererBitmap beforeOutside = cropDocumentRect(beforeMove, belowArtboard);
  const svg::RendererBitmap firstOutside = cropDocumentRect(firstMove, belowArtboard);
  ASSERT_FALSE(beforeOutside.empty()) << "The viewport must expose a point below the SVG root";
  ASSERT_FALSE(firstOutside.empty());
  tests::CompareBitmapToBitmap(firstOutside, beforeOutside, "shine_artboard_clip",
                               tests::PixelmatchIdentityParams());
}

TEST(EditorShellTest, ColdSplashShineDragGetsFullTileBeforeMouseUp) {
  gui::EditorWindow window(TestWindowOptions({
      .title = "Cold splash shine drag pixels",
      .initialWidth = 960,
      .initialHeight = 720,
      .forceOffscreenRenderTarget = true,
      .enableFramebufferReadback = true,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "Framebuffer readback is unavailable on this host";
  }
  ExpectTestWindowScale(window, Vector2i(960, 720), kTestContentScale);
  const EditorSample* splash = FindEditorSample("donner-splash");
  ASSERT_NE(splash, nullptr);
  EditorShell shell(window, OptionsWithSource(splash->source, "donner_splash.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto shine = app.document().document().querySelector("#Background_shine ellipse");
  ASSERT_TRUE(shine.has_value());
  const Entity shineEntity = shine->unsafeEntityHandle().entity();
  const auto captureContent = [&] {
    shell.setContentOnlyCaptureForNextFrameForReplay(true);
    window.beginFrame();
    shell.runFrame();
    return window.endFrameAndReadPixels();
  };
  const auto waitForCurrentFrame = [&](bool requireRequestedDensity = false) {
    const tests::HangGuard guard;
    for (int frame = 0; guard.active(); ++frame) {
      (void)captureContent();
      if (!shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(guard.deadline())) {
        ADD_FAILURE() << "Cold shine render did not complete within the hang guard at frame "
                      << frame << " rendererBusy=" << shell.asyncRendererForReplay().isBusy()
                      << " renderInFlight="
                      << shell.asyncRendererForReplay().hasRenderInFlightForTesting()
                      << " heldResultPolls="
                      << shell.asyncRendererForReplay().replayResultHoldPollCountForTesting();
        return false;
      }
      const LayerInspectorStatusReadback status = shell.layerInspectorStatusForReadback();
      const bool dragTileReady =
          !status.activeDragPreview.has_value() ||
          std::ranges::any_of(status.tiles, [](const LayerInspectorStatusReadback::Tile& tile) {
            return tile.isDragTarget;
          });
      if (!app.document().hasPendingMutations() && !shell.asyncRendererForReplay().isBusy() &&
          EditorShellTestAccess::DisplayedDocVersion(shell) ==
              app.document().currentFrameVersion() &&
          CachedRasterCoversVisibleArtboard(shell, shell.viewportForReadback(),
                                            requireRequestedDensity) &&
          dragTileReady) {
        return true;
      }
    }
    const LayerInspectorStatusReadback status = shell.layerInspectorStatusForReadback();
    const auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
    ADD_FAILURE()
        << "Cold shine frame did not reach its held-pointer cache: "
        << RenderCoordinatorTestAccess::overviewScheduleState(
               EditorShellTestAccess::Coordinator(shell), app, shell.viewportForReadback())
        << " pendingMutations=" << app.document().hasPendingMutations()
        << " dragging=" << status.selectionDragging
        << " activePreview=" << status.activeDragPreview.has_value()
        << " tiles=" << status.tiles.size() << " dragTiles="
        << std::ranges::count_if(
               status.tiles,
               [](const LayerInspectorStatusReadback::Tile& tile) { return tile.isDragTarget; })
        << " renderInFlight=" << shell.asyncRendererForReplay().hasRenderInFlightForTesting()
        << " heldResultPolls="
        << shell.asyncRendererForReplay().replayResultHoldPollCountForTesting()
        << " requireRequestedDensity=" << requireRequestedDensity
        << " presentedRect=" << (frame != nullptr ? frame->rasterDocumentRect() : Box2d())
        << " presentedPixels="
        << (frame != nullptr ? frame->rasterOutputSizePx() : Vector2i::Zero())
        << " presentedOverview=" << (frame != nullptr && frame->usesOverviewRaster())
        << " requestedRect=" << shell.viewportForReadback().rasterViewport().documentRect
        << " requestedPixels=" << shell.viewportForReadback().rasterViewport().outputSizePx;
    return false;
  };
  ASSERT_THAT(waitForCurrentFrame(/*requireRequestedDensity=*/true), testing::Eq(true));
  const ViewportState viewport = shell.viewportForReadback();
  const svg::RendererBitmap before = captureContent();

  // Start from an unselected, already-presented document. Mouse-down and the next move happen
  // back-to-back, without a selection-only prewarm or a mouse-up to settle the renderer.
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(300.0, 80.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  (void)captureContent();
  ASSERT_TRUE(app.selectedElement().has_value());
  ASSERT_EQ(app.selectedElement()->unsafeEntityHandle().entity(), shineEntity);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(300.0, 380.0),
      .leftMouseDown = true,
  });
  (void)captureContent();
  ASSERT_TRUE(waitForCurrentFrame()) << "The selected tile must arrive while the pointer is held";
  const svg::RendererBitmap held = captureContent();
  const LayerInspectorStatusReadback heldStatus = shell.layerInspectorStatusForReadback();
  ASSERT_TRUE(heldStatus.activeDragPreview.has_value());
  EXPECT_NEAR(heldStatus.activeDragPreview->translation.y, 300.0, 1e-6);
  const bool hasFullShineTile =
      std::ranges::any_of(heldStatus.tiles, [](const LayerInspectorStatusReadback::Tile& tile) {
        // A cold capture may rasterize after the +300 transform, so its origin can already be
        // inside the artboard. The complete ellipse spans about 455 by 524 document units.
        return tile.isDragTarget && tile.bitmapDimsDoc.x > 400.0 && tile.bitmapDimsDoc.y > 500.0;
      });
  EXPECT_TRUE(hasFullShineTile)
      << "The first cold drag capture must retain source pixels above the artboard";

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(300.0, 380.0),
      .leftMouseReleased = true,
  });
  (void)captureContent();
  ASSERT_TRUE(waitForCurrentFrame());
  const svg::RendererBitmap settled = captureContent();
  const Box2d sky = Box2d::FromXYWH(270.0, 40.0, 70.0, 50.0);
  const svg::RendererBitmap beforeSky =
      CropDocumentRect(before, viewport, window.displayScale(), sky);
  const svg::RendererBitmap heldSky = CropDocumentRect(held, viewport, window.displayScale(), sky);
  const svg::RendererBitmap settledSky =
      CropDocumentRect(settled, viewport, window.displayScale(), sky);
  ASSERT_FALSE(beforeSky.empty());
  ASSERT_FALSE(heldSky.empty());
  ASSERT_FALSE(settledSky.empty());
  tests::BitmapGoldenCompareParams signalParams = tests::PixelmatchIdentityParams();
  signalParams.maxMismatchedPixels = std::numeric_limits<int>::max();
  int changedPixels = -1;
  tests::CompareBitmapToBitmap(heldSky, beforeSky, "cold_shine_held_pixel_change", signalParams,
                               &changedPixels);
  EXPECT_GT(changedPixels, 0);
  tests::CompareBitmapToBitmap(heldSky, settledSky, "cold_shine_held_matches_settled",
                               tests::ApprovedPixelToleranceParams(0.02f, 100));
}

TEST(EditorShellTest, GeodeMaskedChildrenUpdateCanvasThroughTwoHeldMoves) {
  gui::EditorWindow window(TestWindowOptions({
      .title = "Geode masked child held drag pixels",
      .initialWidth = 1024,
      .initialHeight = 768,
      .forceOffscreenRenderTarget = true,
      .enableFramebufferReadback = true,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "Framebuffer readback is unavailable on this host";
  }
  const EditorSample* splash = FindEditorSample("geode-splash");
  ASSERT_NE(splash, nullptr);
  EditorShell shell(window, OptionsWithSource(splash->source, "geode_splash.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto captureContent = [&] {
    shell.setContentOnlyCaptureForNextFrameForReplay(true);
    window.beginFrame();
    shell.runFrame();
    return window.endFrameAndReadPixels();
  };
  const auto awaitPresentation = [&] {
    for (const tests::HangGuard guard; guard.active();) {
      (void)captureContent();
      if (!shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(guard.deadline())) {
        break;
      }
      if (!app.document().hasPendingMutations() &&
          EditorShellTestAccess::DisplayedDocVersion(shell) ==
              app.document().currentFrameVersion()) {
        return;
      }
    }
    FAIL() << "Geode drag frame did not reach the presenter while the pointer was held: current="
           << app.document().currentFrameVersion()
           << " displayed=" << EditorShellTestAccess::DisplayedDocVersion(shell)
           << " rendererBusy=" << EditorShellTestAccess::RendererBusy(shell);
  };
  awaitPresentation();

  for (const char* id : {"central-crown-face-2", "central-silhouette"}) {
    SCOPED_TRACE(id);
    const std::optional<svg::SVGElement> target =
        app.document().document().querySelector(std::string("#") + id);
    ASSERT_TRUE(target.has_value());
    const std::optional<Box2d> bounds = target->cast<svg::SVGGeometryElement>().worldBounds();
    ASSERT_TRUE(bounds.has_value());
    app.setSelection(*target);
    EditorShellTestAccess::SetRequestRenderAtEndOfFrame(shell);
    (void)captureContent();
    awaitPresentation();

    const ViewportState viewport = shell.viewportForReadback();
    const Vector2d start = (bounds->topLeft + bounds->bottomRight) / 2.0;
    const Box2d contentCrop = Box2d::FromXYWH(bounds->topLeft.x - 8.0, bounds->topLeft.y - 8.0,
                                              bounds->width() + 116.0, bounds->height() + 16.0);
    const svg::RendererBitmap before = captureContent();
    ASSERT_TRUE(EditorShellTestAccess::BeginSelectedShapeDrag(shell, start, *bounds));
    (void)EditorShellTestAccess::MoveSelectedShapeDrag(shell, start + Vector2d(10.0, 0.0));
    awaitPresentation();
    const svg::RendererBitmap firstHeld = captureContent();
    const LayerInspectorStatusReadback firstStatus = shell.layerInspectorStatusForReadback();
    ASSERT_TRUE(firstStatus.activeDragPreview.has_value());
    EXPECT_NEAR(firstStatus.activeDragPreview->translation.x, 10.0, 1e-6);
    const std::optional<Box2d> firstBounds = target->cast<svg::SVGGeometryElement>().worldBounds();
    ASSERT_TRUE(firstBounds.has_value());
    EXPECT_NEAR(firstBounds->topLeft.x - bounds->topLeft.x, 10.0, 1e-6);
    (void)EditorShellTestAccess::MoveSelectedShapeDrag(shell, start + Vector2d(20.0, 0.0));
    awaitPresentation();
    const svg::RendererBitmap secondHeld = captureContent();
    const LayerInspectorStatusReadback secondStatus = shell.layerInspectorStatusForReadback();
    ASSERT_TRUE(secondStatus.activeDragPreview.has_value());
    EXPECT_NEAR(secondStatus.activeDragPreview->translation.x, 20.0, 1e-6);
    const std::optional<Box2d> secondBounds = target->cast<svg::SVGGeometryElement>().worldBounds();
    ASSERT_TRUE(secondBounds.has_value());
    EXPECT_NEAR(secondBounds->topLeft.x - bounds->topLeft.x, 20.0, 1e-6);

    const svg::RendererBitmap beforeCrop =
        CropDocumentRect(before, viewport, window.displayScale(), contentCrop);
    const svg::RendererBitmap firstCrop =
        CropDocumentRect(firstHeld, viewport, window.displayScale(), contentCrop);
    const svg::RendererBitmap secondCrop =
        CropDocumentRect(secondHeld, viewport, window.displayScale(), contentCrop);
    ASSERT_FALSE(beforeCrop.empty());
    ASSERT_FALSE(firstCrop.empty());
    ASSERT_FALSE(secondCrop.empty());
    tests::BitmapGoldenCompareParams signalParams = tests::PixelmatchIdentityParams();
    signalParams.maxMismatchedPixels = std::numeric_limits<int>::max();
    int firstMovePixels = -1;
    int secondMovePixels = -1;
    tests::CompareBitmapToBitmap(firstCrop, beforeCrop, std::string(id) + "_first_held_move",
                                 signalParams, &firstMovePixels);
    tests::CompareBitmapToBitmap(secondCrop, firstCrop, std::string(id) + "_second_held_move",
                                 signalParams, &secondMovePixels);
    if (firstMovePixels == 0 || secondMovePixels == 0) {
      if (const char* outputDir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
        const std::array<std::pair<const char*, const svg::RendererBitmap*>, 3> frames = {
            std::pair{"before", &beforeCrop}, std::pair{"first", &firstCrop},
            std::pair{"second", &secondCrop}};
        for (const auto& [stage, bitmap] : frames) {
          const std::filesystem::path path =
              std::filesystem::path(outputDir) / (std::string(id) + "_" + stage + ".png");
          (void)svg::RendererImageIO::writeRgbaPixelsToPngFile(
              path.string().c_str(), bitmap->pixels, bitmap->dimensions.x, bitmap->dimensions.y,
              bitmap->rowBytes / 4u);
        }
      }
    }
    EXPECT_GT(firstMovePixels, 0)
        << "The canvas stayed frozen through the first held move: current="
        << firstStatus.documentFrameVersion << " displayed=" << firstStatus.displayedDocVersion
        << " tiles=" << firstStatus.tiles.size();
    EXPECT_GT(secondMovePixels, 0)
        << "The canvas stayed frozen through the next held move: current="
        << secondStatus.documentFrameVersion << " displayed=" << secondStatus.displayedDocVersion
        << " tiles=" << secondStatus.tiles.size();

    EditorShellTestAccess::EndSelectedShapeDrag(shell, start + Vector2d(20.0, 0.0));
    EditorShellTestAccess::SetRequestRenderAtEndOfFrame(shell);
    (void)captureContent();
    awaitPresentation();
  }
}

void RunGeodeColdDirectRetinaDrag(std::string_view id, bool selectFromLayers,
                                  bool waitForSelectedPrewarm = false) {
  gui::EditorWindow window(TestWindowOptions({
      .title = "Geode direct Retina pointer drag",
      .initialWidth = 1600,
      .initialHeight = 900,
      .forceOffscreenRenderTarget = true,
      .enableFramebufferReadback = true,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "Retina framebuffer readback is unavailable on this host";
  }
  const EditorSample* splash = FindEditorSample("geode-splash");
  ASSERT_NE(splash, nullptr);
  EditorShell shell(window, OptionsWithSource(splash->source, "geode_splash.svg"));
  ASSERT_TRUE(shell.valid());
  ASSERT_NEAR(window.displayScale(), 2.0, 1e-6);
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto frame = [&](const Vector2d& documentPoint, bool mouseDown, bool capture) {
    const Vector2d screen = shell.viewportForReadback().documentToScreen(documentPoint);
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(static_cast<float>(screen.x), static_cast<float>(screen.y));
    io.AddMouseButtonEvent(0, mouseDown);
    if (capture) {
      shell.setContentOnlyCaptureForNextFrameForReplay(true);
    }
    window.beginFrame();
    shell.runFrame();
    if (capture) {
      return window.endFrameAndReadPixels();
    }
    window.endFrame();
    return svg::RendererBitmap{};
  };
  for (const tests::HangGuard coldGuard; coldGuard.active();) {
    (void)frame(Vector2d(-100.0, -100.0), false, false);
    if (!shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(coldGuard.deadline())) {
      break;
    }
    if (EditorShellTestAccess::DisplayedDocVersion(shell) == app.document().currentFrameVersion()) {
      break;
    }
  }
  ASSERT_EQ(EditorShellTestAccess::DisplayedDocVersion(shell),
            app.document().currentFrameVersion());
  (void)frame(Vector2d(-100.0, -100.0), false, false);
  (void)frame(Vector2d(-100.0, -100.0), false, false);

  const auto target = app.document().document().querySelector("#" + std::string(id));
  ASSERT_TRUE(target.has_value());
  const Entity targetEntity = target->unsafeEntityHandle().entity();
  const auto bounds = target->cast<svg::SVGGeometryElement>().worldBounds();
  ASSERT_TRUE(bounds.has_value());
  std::optional<Vector2d> hitPoint;
  const Vector2d center = (bounds->topLeft + bounds->bottomRight) / 2.0;
  if (selectFromLayers) {
    // This silhouette is covered by later-painted crystal faces, so canvas hit-testing cannot
    // select it. Select it from Layers, then immediately drag its bounds through real mouse input
    // while its speculative prewarm is still in flight.
    hitPoint = center;
  } else {
    double bestDistance = std::numeric_limits<double>::infinity();
    for (int y = 1; y < 17; ++y) {
      for (int x = 1; x < 17; ++x) {
        const Vector2d point =
            bounds->topLeft + Vector2d(bounds->width() * x / 17.0, bounds->height() * y / 17.0);
        const auto hit = app.hitTest(point);
        const double distance = (point - center).lengthSquared();
        if (hit.has_value() && hit->id() == id && distance < bestDistance) {
          hitPoint = point;
          bestDistance = distance;
        }
      }
    }
  }
  ASSERT_TRUE(hitPoint.has_value()) << id << " has no canvas hit point";
  const ViewportState viewport = shell.viewportForReadback();
  const Box2d crop = Box2d::FromXYWH(bounds->topLeft.x - 8.0, bounds->topLeft.y - 8.0,
                                     bounds->width() + 90.0, bounds->height() + 16.0);
  const svg::RendererBitmap before = frame(*hitPoint, false, true);
  ASSERT_GE(before.dimensions.x, 3000);
  ASSERT_GE(before.dimensions.y, 1600);
  const svg::RendererBitmap beforeCrop =
      CropDocumentRect(before, viewport, window.displayScale(), crop);
  ASSERT_FALSE(beforeCrop.empty());
  if (selectFromLayers) {
    shell.asyncRendererForReplay().setReplayRenderDelayForTesting(std::chrono::milliseconds(45));
    app.setSelection(*target);
    EditorShellTestAccess::SetRequestRenderAtEndOfFrame(shell);
    (void)frame(*hitPoint, false, false);
    if (waitForSelectedPrewarm) {
      const auto awaitSelectedPrewarm = [&] {
        for (const tests::HangGuard settledGuard; settledGuard.active();) {
          (void)frame(*hitPoint, false, false);
          if (!shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
                  settledGuard.deadline())) {
            break;
          }
          if (!EditorShellTestAccess::RendererBusy(shell) &&
              EditorShellTestAccess::DisplayedDocVersion(shell) ==
                  app.document().currentFrameVersion()) {
            return true;
          }
        }
        return false;
      };
      ASSERT_TRUE(awaitSelectedPrewarm())
          << "The selected silhouette prewarm did not settle before mouse-down";
      ASSERT_FALSE(EditorShellTestAccess::RendererBusy(shell))
          << "The selected silhouette prewarm did not settle before mouse-down";
      ASSERT_GT(EditorShellTestAccess::DisplayedSelectionBoundsCount(shell), 0u);
      const auto coveringHit = app.hitTest(*hitPoint);
      ASSERT_TRUE(coveringHit.has_value());
      ASSERT_NE(coveringHit->id(), id);
      (void)frame(*hitPoint, true, false);
      (void)frame(*hitPoint, false, false);
      ASSERT_TRUE(app.selectedElement().has_value());
      EXPECT_EQ(app.selectedElement()->id(), coveringHit->id())
          << "A click without dragging must select the topmost covering facet";

      // Restore the silhouette through Layers and verify that a real held drag, unlike the
      // click above, keeps that selection even under the same overlapping facet.
      app.setSelection(*target);
      EditorShellTestAccess::SetRequestRenderAtEndOfFrame(shell);
      (void)frame(*hitPoint, false, false);
      ASSERT_TRUE(awaitSelectedPrewarm());

      // The selection click and the held drag are independent pointer gestures.
      window.beginFrameWithInput({
          .deltaSeconds = ImGui::GetIO().MouseDoubleClickTime + 1.0 / 60.0,
          .mousePosition = shell.viewportForReadback().documentToScreen(*hitPoint),
      });
      shell.runFrame();
      window.endFrame();
    }
  }
  const std::uint64_t displayedBefore = EditorShellTestAccess::DisplayedDocVersion(shell);
  (void)frame(*hitPoint, true, false);
  ASSERT_THAT(ImGui::GetIO().MouseClickedCount[0], ::testing::Eq(1))
      << "The drag must start as a separate single-click gesture";

  tests::BitmapGoldenCompareParams signalParams = tests::PixelmatchIdentityParams();
  signalParams.maxMismatchedPixels = std::numeric_limits<int>::max();
  bool activeDrag = false;
  bool wrongActiveDragEntity = false;
  int requestsPosted = 0;
  for (int move = 1; move <= 24; ++move) {
    (void)frame(*hitPoint + Vector2d(move * 0.5, 0.0), true, false);
    const LayerInspectorStatusReadback status = shell.layerInspectorStatusForReadback();
    if (status.activeDragPreview.has_value()) {
      activeDrag = true;
      wrongActiveDragEntity |= status.activeDragPreview->entity != targetEntity;
    }
    requestsPosted += EditorShellTestAccess::RenderRequestsPosted(shell);
    std::this_thread::sleep_for(std::chrono::milliseconds(3));
  }
  std::ostringstream heldDiagnostics;
  std::optional<svg::RendererBitmap> lastHeldFrame;
  std::optional<svg::RendererBitmap> lastHeldCrop;
  const auto awaitHeldPixels = [&](double dx, std::uint64_t minimumVersion,
                                   const svg::RendererBitmap& prior,
                                   std::string_view phase) -> std::optional<svg::RendererBitmap> {
    const Vector2d pointer = *hitPoint + Vector2d(dx, 0.0);
    const tests::HangGuard guard;
    for (int tick = 0; guard.active(); ++tick) {
      (void)frame(pointer, true, false);
      requestsPosted += EditorShellTestAccess::RenderRequestsPosted(shell);
      const LayerInspectorStatusReadback status = shell.layerInspectorStatusForReadback();
      if (status.activeDragPreview.has_value()) {
        activeDrag = true;
        wrongActiveDragEntity |= status.activeDragPreview->entity != targetEntity;
      }
      const std::uint64_t currentVersion = app.document().currentFrameVersion();
      const bool pendingTransform = app.document().hasPendingMutations();
      const bool rendererBusy = EditorShellTestAccess::RendererBusy(shell);
      if (currentVersion <= minimumVersion || status.displayedDocVersion < currentVersion ||
          pendingTransform) {
        heldDiagnostics << "\n  phase=" << phase << " wait=" << tick
                        << " current=" << currentVersion
                        << " displayed=" << status.displayedDocVersion
                        << " pendingTransform=" << pendingTransform
                        << " rendererBusy=" << rendererBusy;
        (void)shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(guard.deadline());
        continue;
      }
      svg::RendererBitmap held = frame(pointer, true, true);
      requestsPosted += EditorShellTestAccess::RenderRequestsPosted(shell);
      const LayerInspectorStatusReadback capturedStatus = shell.layerInspectorStatusForReadback();
      const svg::RendererBitmap heldCrop =
          CropDocumentRect(held, viewport, window.displayScale(), crop);
      EXPECT_EQ(heldCrop.dimensions, prior.dimensions)
          << "The same fixed document crop changed size during a held drag";
      int baselinePixels = -1;
      int successivePixels = -1;
      if (!heldCrop.empty()) {
        const std::string label =
            std::string(id) + "_" + std::string(phase) + "_" + std::to_string(tick);
        tests::CompareBitmapToBitmap(heldCrop, beforeCrop, label + "_baseline", signalParams,
                                     &baselinePixels);
        tests::CompareBitmapToBitmap(heldCrop, prior, label + "_successive", signalParams,
                                     &successivePixels);
      }
      heldDiagnostics << "\n  phase=" << phase << " capture=" << tick
                      << " current=" << app.document().currentFrameVersion()
                      << " displayed=" << capturedStatus.displayedDocVersion << " translationX="
                      << (capturedStatus.activeDragPreview.has_value()
                              ? capturedStatus.activeDragPreview->translation.x
                              : std::numeric_limits<double>::quiet_NaN())
                      << " baselinePixels=" << baselinePixels
                      << " successivePixels=" << successivePixels << " displayedPreviewX="
                      << (capturedStatus.displayedDragPreview.has_value()
                              ? capturedStatus.displayedDragPreview->translation.x
                              : std::numeric_limits<double>::quiet_NaN())
                      << " activeTileDraws="
                      << capturedStatus.frameCost.directPresentation.activeTileDrawCount
                      << " tiles=" << capturedStatus.tiles.size() << " pendingSelectedRaster="
                      << (capturedStatus.pendingSelectedLayerRasterizationEntity != entt::null)
                      << " pendingSelectedVersion="
                      << capturedStatus.pendingSelectedLayerRasterizationVersion;
      for (const LayerInspectorStatusReadback::Tile& tile : capturedStatus.tiles) {
        if (tile.isDragTarget) {
          heldDiagnostics << " targetTile=" << tile.id << ":" << tile.generation
                          << " presentedX=" << tile.presentedDragTranslationDoc.x
                          << " payload=" << tile.bitmapDimsPx.x << "x" << tile.bitmapDimsPx.y
                          << " metadataOnly=" << tile.metadataOnly;
        }
      }
      int loggedOwnerTiles = 0;
      for (const LayerInspectorStatusReadback::Tile& tile : capturedStatus.tiles) {
        if (tile.kind == RenderResult::CompositedTile::Kind::Layer && loggedOwnerTiles < 12) {
          heldDiagnostics << " ownerTile=" << tile.id << ":" << tile.generation
                          << " presentedX=" << tile.presentedDragTranslationDoc.x
                          << " payload=" << tile.bitmapDimsPx.x << "x" << tile.bitmapDimsPx.y;
          ++loggedOwnerTiles;
        }
      }
      lastHeldFrame = std::move(held);
      lastHeldCrop = heldCrop;
      if (successivePixels > 0 && capturedStatus.activeDragPreview.has_value() &&
          capturedStatus.activeDragPreview->entity == targetEntity &&
          std::abs(capturedStatus.activeDragPreview->translation.x - dx) < 0.5) {
        return heldCrop;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return std::nullopt;
  };
  // The crown is clipped to the unmoved crystal silhouette. Large translations move it entirely
  // outside that clip, making two correctly rendered held frames identical; keep both probes
  // within the visible clip so pixel movement is a meaningful presentation oracle.
  const auto firstHeld = awaitHeldPixels(12.0, displayedBefore, beforeCrop, "first_held");
  if (!firstHeld.has_value() && lastHeldFrame.has_value() && lastHeldCrop.has_value()) {
    if (const char* outputDir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
      const std::array<std::pair<const char*, const svg::RendererBitmap*>, 4> frames = {
          std::pair{"before_full", &before}, std::pair{"last_held_full", &*lastHeldFrame},
          std::pair{"before_crop", &beforeCrop}, std::pair{"last_held_crop", &*lastHeldCrop}};
      for (const auto& [stage, bitmap] : frames) {
        const std::filesystem::path path =
            std::filesystem::path(outputDir) / (std::string(id) + "_" + stage + ".png");
        (void)svg::RendererImageIO::writeRgbaPixelsToPngFile(
            path.string().c_str(), bitmap->pixels, bitmap->dimensions.x, bitmap->dimensions.y,
            bitmap->rowBytes / 4u);
      }
    }
  }
  ASSERT_TRUE(firstHeld.has_value())
      << "The first held position never reached the canvas before mouse-up; posted="
      << requestsPosted << " busy=" << EditorShellTestAccess::RendererBusy(shell)
      << " cropFrameOrigin=" << viewport.documentToScreen(crop.topLeft) * window.displayScale()
      << " cropDims=" << beforeCrop.dimensions.x << "x" << beforeCrop.dimensions.y
      << " framebufferDims=" << before.dimensions.x << "x" << before.dimensions.y
      << heldDiagnostics.str();
  const std::uint64_t firstVersion = app.document().currentFrameVersion();
  (void)frame(*hitPoint + Vector2d(20.0, 0.0), true, false);
  requestsPosted += EditorShellTestAccess::RenderRequestsPosted(shell);
  const auto secondHeld = awaitHeldPixels(20.0, firstVersion, *firstHeld, "second_held");
  ASSERT_TRUE(secondHeld.has_value())
      << "The second held position never produced distinct canvas pixels before mouse-up; "
      << "posted=" << requestsPosted << " busy=" << EditorShellTestAccess::RendererBusy(shell)
      << heldDiagnostics.str();
  ASSERT_TRUE(app.selectedElement().has_value());
  EXPECT_EQ(app.selectedElement()->id(), id)
      << "chosen hitPoint=" << *hitPoint << " center=" << center << " viewportAfter="
      << shell.viewportForReadback().screenToDocument(viewport.documentToScreen(*hitPoint));
  EXPECT_TRUE(activeDrag) << "A direct canvas press did not begin the selected shape drag";
  EXPECT_FALSE(wrongActiveDragEntity) << "The held native gesture dragged another Geode element";
  EXPECT_GT(requestsPosted, 0) << "No raster request was posted during the held gesture";
  (void)frame(*hitPoint + Vector2d(20.0, 0.0), false, false);
}

TEST(EditorShellTest, GeodeCrownColdDirectRetinaDragPresentsBeforeMouseUp) {
  RunGeodeColdDirectRetinaDrag("central-crown-face-2", /*selectFromLayers=*/false);
}

TEST(EditorShellTest, GeodeSilhouetteColdDirectRetinaDragPresentsBeforeMouseUp) {
  RunGeodeColdDirectRetinaDrag("central-silhouette", /*selectFromLayers=*/true);
}

TEST(EditorShellTest, GeodeSilhouetteSettledLayersSelectionStillDragsSelectedShape) {
  RunGeodeColdDirectRetinaDrag("central-silhouette", /*selectFromLayers=*/true,
                               /*waitForSelectedPrewarm=*/true);
}

TEST(EditorShellTest, DragMutationWaitsForQueuedRenderToConsumeItsDocument) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_THAT(shell.valid(), testing::IsTrue());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  EditorApp& app = EditorShellTestAccess::App(shell);
  svg::SVGDocument& document = app.document().document();
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  app.setSelection(*target);
  ASSERT_THAT(EditorShellTestAccess::BeginSelectedShapeDrag(
                  shell, Vector2d(20.0, 20.0), Box2d::FromXYWH(10.0, 12.0, 40.0, 24.0)),
              testing::IsTrue());
  document.setThreadingMode(svg::ThreadingMode::ConcurrentDom);
  const std::uint64_t submittedVersion = app.document().currentFrameVersion();
  {
    // Hold the worker before document acquisition while same-thread UI guards remain available.
    const auto holdWorkerAccess = document.writeAccess();
    AsyncRenderer& renderer =
        EditorShellTestAccess::BeginDelayedRender(shell, std::chrono::milliseconds(0));
    ASSERT_THAT(renderer.isBusy(), testing::IsTrue());
    EXPECT_THAT(EditorShellTestAccess::MoveSelectedShapeDrag(shell, Vector2d(60.0, 40.0)),
                testing::IsFalse());
    EXPECT_EQ(app.document().currentFrameVersion(), submittedVersion);
    EXPECT_THAT(app.document().hasPendingMutations(), testing::IsTrue());
  }
  AsyncRenderer& renderer = shell.asyncRendererForReplay();
  ASSERT_THAT(renderer.waitUntilNoRenderInFlightForTesting(std::chrono::steady_clock::now() +
                                                           std::chrono::seconds(5)),
              testing::IsTrue());
  std::ignore = renderer.pollResult();
  EXPECT_THAT(EditorShellTestAccess::MoveSelectedShapeDrag(shell, Vector2d(60.0, 40.0)),
              testing::IsTrue());
  EXPECT_EQ(app.document().currentFrameVersion(), submittedVersion + 1);
  EXPECT_THAT(app.document().hasPendingMutations(), testing::IsFalse());
  EXPECT_THAT(target->cast<svg::SVGGraphicsElement>().transform(),
              testing::Eq(Transform2d::Translate(Vector2d(40.0, 20.0))));
}

TEST(EditorShellTest, CoalescedFinalPointerAndReleaseCommitTheFinalPoseAndUndo) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg, "coalesced-release.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0, 0, 120, 80));
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);
  ASSERT_TRUE(EditorShellTestAccess::BeginSelectedShapeDrag(shell, Vector2d(20, 20),
                                                            Box2d::FromXYWH(10, 12, 40, 24)));
  ImGuiIO& io = ImGui::GetIO();
  const bool oldTrickle = io.ConfigInputTrickleEventQueue;
  io.ConfigInputTrickleEventQueue = false;
  const auto startScreen = shell.viewportForReadback().documentToScreen(Vector2d(20, 20));
  io.AddMousePosEvent(static_cast<float>(startScreen.x), static_cast<float>(startScreen.y));
  io.AddMouseButtonEvent(0, true);
  window.beginFrame();
  window.endFrame();
  ASSERT_TRUE(EditorShellTestAccess::MoveSelectedShapeDrag(shell, Vector2d(38, 32)));
  const auto finalScreen = shell.viewportForReadback().documentToScreen(Vector2d(52, 40));
  io.AddMousePosEvent(static_cast<float>(finalScreen.x), static_cast<float>(finalScreen.y));
  io.AddMouseButtonEvent(0, false);
  window.beginFrame();
  EXPECT_TRUE(ImGui::IsMouseReleased(ImGuiMouseButton_Left));
  EXPECT_FALSE(ImGui::IsMouseDown(ImGuiMouseButton_Left));
  EditorShellTestAccess::UpdateSelectionDrag(shell);
  window.endFrame();
  io.ConfigInputTrickleEventQueue = oldTrickle;
  const Transform2d expected = Transform2d::Translate(Vector2d(32, 20));
  const auto presentation =
      EditorShellTestAccess::Coordinator(shell).compositedPresentation().diagnostics();
  ASSERT_TRUE(presentation.settlingPreview.has_value());
  EXPECT_EQ(presentation.settlingPreview->translation, Vector2d(32, 20));
  ASSERT_EQ(presentation.settlingPreview->poses.size(), 1u);
  EXPECT_EQ(presentation.settlingPreview->poses.front().documentFromElement, expected);
  ASSERT_TRUE(shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(5)));
  std::ignore = app.flushFrame();
  EXPECT_EQ(target->cast<svg::SVGGraphicsElement>().transform(), expected);
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(target->cast<svg::SVGGraphicsElement>().transform(), Transform2d());
  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(target->cast<svg::SVGGraphicsElement>().transform(), expected);
}

TEST(EditorShellTest, SelectDragKeepsFullPathChrome) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  EditorShellTestAccess::App(shell).setSelection(
      *EditorShellTestAccess::App(shell).document().document().querySelector("#target"));
  RunFramesUntilDisplayedSelectionBounds(window, shell);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(20.0, 20.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  EXPECT_EQ(EditorShellTestAccess::SelectionChromeDetailForActiveTool(shell),
            SelectionChromeDetail::Full);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(30.0, 30.0),
      .leftMouseDown = true,
      .hitElementId = std::string("target"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  EXPECT_EQ(EditorShellTestAccess::SelectionChromeDetailForActiveTool(shell),
            SelectionChromeDetail::Full);
  RunShellFrame(window, shell);
  EXPECT_THAT(EditorShellTestAccess::SelectionChromePathCount(shell), ::testing::Gt(0u));
}

TEST(EditorShellTest, SelectDoubleClickOnTextSwitchesToTextEditingAtClick) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  auto label = EditorShellTestAccess::App(shell).document().document().querySelector("#label");
  ASSERT_TRUE(label.has_value());
  svg::SVGTextElement labelText = label->cast<svg::SVGTextElement>();
  const Box2d extent =
      labelText.withWriteAccess([&labelText](svg::DocumentWriteAccess&, EntityHandle) {
        return labelText.getExtentOfChar(2u);
      });
  const Vector2d clickPoint(extent.topLeft.x + extent.size().x * 0.25,
                            extent.topLeft.y + extent.size().y * 0.5);
  MouseModifiers modifiers;
  modifiers.doubleClick = true;
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = clickPoint,
      .leftMouseDown = true,
      .leftMousePressed = true,
      .modifiers = modifiers,
      .hitElementId = std::string("label"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));
  EXPECT_TRUE(EditorShellTestAccess::TextToolIsEditing(shell));
  EXPECT_EQ(EditorShellTestAccess::TextToolCaretIndex(shell), 2u);
  const FormatBarState before = EditorShellTestAccess::ComputeFormatBarState(shell);
  EXPECT_TRUE(before.visible);
  EditorShellTestAccess::ToggleTextBold(shell);
  EditorShellTestAccess::ToggleTextItalic(shell);
  EditorShellTestAccess::ToggleTextUnderline(shell);
  const FormatBarState after = EditorShellTestAccess::ComputeFormatBarState(shell);
  EXPECT_TRUE(after.bold);
  EXPECT_TRUE(after.italic);
  EXPECT_TRUE(after.underline);
}

TEST(EditorShellTest, SelectToolShortcutsComposeTextBoldItalicAndUnderline) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "shortcut-text.svg"));
  ASSERT_TRUE(shell.valid());
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto label = app.document().document().querySelector("#label");
  ASSERT_TRUE(label.has_value());
  app.setSelection(*label);

  DriveGlobalShortcut(shell, {ImGuiKey_B}, /*ctrl=*/false, /*shift=*/false, /*super=*/true);
  EXPECT_EQ(label->getAttribute("font-weight"), "bold");
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  DriveGlobalShortcut(shell, {ImGuiKey_I}, /*ctrl=*/false, /*shift=*/false, /*super=*/true);
  EXPECT_EQ(label->getAttribute("font-weight"), "bold");
  EXPECT_EQ(label->getAttribute("font-style"), "italic");
  EXPECT_EQ(app.undoTimeline().entryCount(), 2u);
  DriveGlobalShortcut(shell, {ImGuiKey_U}, /*ctrl=*/false, /*shift=*/false, /*super=*/true);
  EXPECT_EQ(label->getAttribute("text-decoration"), "underline");
  EXPECT_EQ(app.undoTimeline().entryCount(), 3u);
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));

  DriveGlobalShortcut(shell, {ImGuiKey_B}, /*ctrl=*/false, /*shift=*/false, /*super=*/true);
  EXPECT_EQ(label->getAttribute("font-weight"), "normal");
  EXPECT_EQ(label->getAttribute("font-style"), "italic");
  EXPECT_EQ(label->getAttribute("text-decoration"), "underline");
  EXPECT_EQ(app.undoTimeline().entryCount(), 4u);

  DriveGlobalShortcut(shell, {ImGuiKey_I}, /*ctrl=*/false, /*shift=*/false, /*super=*/true,
                      /*textInputActive=*/true);
  EXPECT_EQ(label->getAttribute("font-style"), "italic");
  EXPECT_EQ(app.undoTimeline().entryCount(), 4u);
}

TEST(EditorShellTest, SelectToolFormattingShortcutRecordsUndo) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "shortcut-undo.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto label = app.document().document().querySelector("#label");
  ASSERT_TRUE(label.has_value());
  app.setSelection(*label);
  const std::string before(app.document().document().source());

  DriveGlobalShortcut(shell, {ImGuiKey_B}, /*ctrl=*/false, /*shift=*/false, /*super=*/true);
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  EXPECT_EQ(app.document().document().querySelector("#label")->getAttribute("font-weight"), "bold");

  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(app.document().document().source(), before);
  EXPECT_TRUE(app.hasSelection());
  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(app.document().document().querySelector("#label")->getAttribute("font-weight"), "bold");
}

TEST(EditorShellTest, TextToolFamilyChangeUsesOneSessionUndo) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "family-session-undo.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto label = app.document().document().querySelector("#label");
  ASSERT_TRUE(label.has_value());
  svg::SVGTextElement labelText = label->cast<svg::SVGTextElement>();
  const Box2d extent =
      labelText.withWriteAccess([&labelText](svg::DocumentWriteAccess&, EntityHandle) {
        return labelText.getExtentOfChar(2u);
      });
  MouseModifiers modifiers;
  modifiers.doubleClick = true;
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = extent.topLeft + extent.size() * 0.5,
      .leftMouseDown = true,
      .leftMousePressed = true,
      .modifiers = modifiers,
      .hitElementId = std::string("label"),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::TextToolIsEditing(shell));
  const std::string before(app.document().document().source());
  const FormatBarState state = EditorShellTestAccess::ComputeFormatBarState(shell);
  FormatBarActions actions;
  actions.setFontFamily = true;
  actions.fontFamily = "Fira Code";
  EditorShellTestAccess::ApplyFormatBarActions(shell, state, actions);
  EXPECT_EQ(app.undoTimeline().entryCount(), 0u)
      << "the active text session owns family changes until commit";
  ASSERT_TRUE(EditorShellTestAccess::CommitTextEditing(shell));
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  app.undo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(app.document().document().source(), before);
  app.redo();
  ASSERT_TRUE(app.flushFrame());
  EXPECT_EQ(app.document().document().querySelector("#label")->getAttribute("font-family"),
            "Fira Code");
}

TEST(EditorShellTest, DocumentSpaceReplayInputRoutesTextToolPlainClickCreatesNothing) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  const std::string sourceBefore(EditorShellTestAccess::App(shell).document().document().source());

  DriveGlobalShortcut(shell, {ImGuiKey_T});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(30.0, 40.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(30.0, 40.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  EXPECT_FALSE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));
  EXPECT_EQ(EditorShellTestAccess::App(shell).document().document().source(), sourceBefore);
}

TEST(EditorShellTest, DocumentSpaceReplayInputRoutesTextToolDoubleClick) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  DriveGlobalShortcut(shell, {ImGuiKey_T});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));

  MouseModifiers doubleClick;
  doubleClick.doubleClick = true;
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(30.0, 40.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
      .modifiers = doubleClick,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(30.0, 40.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  const svg::SVGElement selected = *EditorShellTestAccess::App(shell).selectedElement();
  EXPECT_EQ(selected.type(), svg::ElementType::Text);
  ASSERT_TRUE(selected.getAttribute("x").has_value());
  EXPECT_EQ(*selected.getAttribute("x"), "30");
  ASSERT_TRUE(selected.getAttribute("y").has_value());
  EXPECT_EQ(*selected.getAttribute("y"), "40");
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find(R"(<text)"),
            std::string_view::npos);
}

TEST(EditorShellTest, DocumentSpaceReplayInputRoutesPenPressReleaseAndHover) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  DriveGlobalShortcut(shell, {ImGuiKey_P});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolActivePathData(shell).empty());

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(10.0, 12.0),
      .leftMouseDown = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  EXPECT_TRUE(EditorShellTestAccess::PenDragFlushedThisFrame(shell));
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolActivePathData(shell).empty());
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(18.0, 24.0),
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
}

TEST(EditorShellTest, PenPreviewTracksLatestPointerBeforeBusyRendererPublishes) {
  auto windowOptions =
      TestWindowOptions({.title = "Pen live preview", .initialWidth = 640, .initialHeight = 480});
  windowOptions.forceOffscreenRenderTarget = true;
  windowOptions.enableFramebufferReadback = true;
  gui::EditorWindow window(windowOptions);
  ASSERT_TRUE(window.valid());
  EditorShell shell(
      window,
      OptionsWithSource(
          R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><rect id="render-demand" x="-100" width="1" height="1"/></svg>)"));
  ASSERT_TRUE(shell.valid());
  DriveGlobalShortcut(shell, {ImGuiKey_P});
  const auto input = [&](Vector2d point, bool down, bool press, bool release) {
    shell.queueDocumentSpaceReplayInputForTesting(
        EditorShellDocumentReplayInput{.documentPoint = point,
                                       .leftMouseDown = down,
                                       .leftMousePressed = press,
                                       .leftMouseReleased = release});
    EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  };
  input({10, 20}, true, true, false);
  input({10, 20}, false, false, true);
  input({50, 20}, true, true, false);
  EditorApp& app = EditorShellTestAccess::App(shell);
  for (const tests::HangGuard guard; guard.active();) {
    RunShellFrame(window, shell);
    if (!EditorShellTestAccess::RendererBusy(shell) && !app.document().hasPendingMutations() &&
        EditorShellTestAccess::DisplayedDocVersion(shell) == app.document().currentFrameVersion()) {
      break;
    }
  }
  const auto oldFrame = EditorShellTestAccess::Coordinator(shell).framePresentation();
  ASSERT_NE(oldFrame, nullptr);
  ASSERT_THAT(oldFrame->replacementPaint(), testing::Ne(std::nullopt));
  using Barrier = AsyncRenderer::CompletionBarrier;
  struct ReleaseBarrier {
    ~ReleaseBarrier() { (void)AsyncRenderer::releaseCompletionBarrierForTesting(); }
  } releaseBarrier;
  EXPECT_EQ(AsyncRenderer::requestCompletionBarrierForTesting(), Barrier::Off);
  AsyncRenderer::armRequestedCompletionBarrierForTesting();
  // Change an off-canvas sibling, preserving the selected path's captured paint and geometry.
  app.applyMutation(EditorCommand::SetAttributeCommand(
      *app.document().document().querySelector("#render-demand"), "x", "-101"));
  ASSERT_TRUE(app.flushFrame());
  AsyncRenderer& renderer =
      EditorShellTestAccess::BeginDelayedRender(shell, std::chrono::milliseconds(0));
  for (const tests::HangGuard guard;
       AsyncRenderer::completionBarrierForTesting() != Barrier::Holding && guard.active();) {
    std::this_thread::sleep_for(tests::kPollInterval);
  }
  ASSERT_EQ(AsyncRenderer::completionBarrierForTesting(), Barrier::Holding);
  ASSERT_TRUE(renderer.isBusy());
  const auto began = std::chrono::steady_clock::now();
  input({60, 40}, true, false, false);
  input({80, 60}, true, false, false);
  RunShellFrame(window, shell);
  RecordProperty("latest_pointer_frame_us", std::chrono::duration_cast<std::chrono::microseconds>(
                                                std::chrono::steady_clock::now() - began)
                                                .count());
  ASSERT_TRUE(renderer.isBusy()) << "Latest pointer must be presented before the worker publishes";
  EXPECT_FALSE(app.document().hasPendingMutations());
  const auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
  ASSERT_NE(frame, nullptr);
  ASSERT_THAT(frame->replacementPaint(), testing::Ne(std::nullopt));
  const Path latest = PathBuilder().moveTo({10, 20}).curveTo({10, 20}, {20, -20}, {50, 20}).build();
  EXPECT_THAT(frame->replacementPaint()->livePathPreview->pathDoc, testing::Eq(latest));
  EXPECT_THAT(frame->replacementPaint()->livePathPreview->pathDoc,
              testing::Ne(oldFrame->replacementPaint()->livePathPreview->pathDoc));
  window.beginFrame();
  shell.runFrame();
  const auto screenshot = window.endFrameAndReadPixels();
  ASSERT_FALSE(screenshot.empty());
  if (const char* output = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR")) {
    const auto file = std::filesystem::path(output) / "pen_latest_pointer_while_busy.png";
    EXPECT_TRUE(svg::RendererImageIO::writeRgbaPixelsToPngFile(
        file.string().c_str(), screenshot.pixels, screenshot.dimensions.x, screenshot.dimensions.y,
        screenshot.rowBytes / 4u));
  }
  input({90, 60}, false, false, true);
  EXPECT_FALSE(app.document().hasPendingMutations());
  RunShellFrame(window, shell);
  const Path released = EditorShellTestAccess::Coordinator(shell)
                            .framePresentation()
                            ->replacementPaint()
                            ->livePathPreview->pathDoc;
  EditorShellTestAccess::BufferPendingClick(shell, {200, 100});
  input({200, 100}, true, false, false);
  input({220, 120}, false, false, true);
  EXPECT_TRUE(EditorShellTestAccess::HasPendingClick(shell));
  EXPECT_THAT(*EditorShellTestAccess::PenPreviewPath(shell), testing::Eq(released));
  RunShellFrame(window, shell);
  EXPECT_EQ(AsyncRenderer::releaseCompletionBarrierForTesting(), Barrier::Holding);
  ASSERT_TRUE(renderer.waitUntilNoRenderInFlightForTesting(std::chrono::steady_clock::now() +
                                                           std::chrono::seconds(2)));
  for (const tests::HangGuard guard; guard.active();) {
    RunShellFrame(window, shell);
    if (!EditorShellTestAccess::RendererBusy(shell) && !app.document().hasPendingMutations() &&
        !EditorShellTestAccess::PenToolIsDraggingAnchor(shell) &&
        EditorShellTestAccess::DisplayedDocVersion(shell) == app.document().currentFrameVersion()) {
      break;
    }
  }
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));
  EXPECT_THAT(
      std::string(app.document().document().querySelector("path")->getAttribute("d")->str()),
      testing::HasSubstr("10 -20 50 20"));
  EXPECT_THAT(
      std::string(app.document().document().querySelector("path")->getAttribute("d")->str()),
      testing::HasSubstr("200 100"));
}

TEST(EditorShellTest, DeferredExistingPenEditsSettleBeforeTheNextPressAndKeepSeparateUndo) {
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  const std::string source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="100" height="100"><path id="p" d="M10 20 L50 20" fill="none" stroke="black"/><rect id="render-demand" x="-100" width="1" height="1"/></svg>)";
  EditorShell shell(window, OptionsWithSource(source));
  ASSERT_TRUE(shell.valid());
  DriveGlobalShortcut(shell, {ImGuiKey_P});
  EditorApp& app = EditorShellTestAccess::App(shell);
  app.setSelection(*app.document().document().querySelector("#p"));
  const auto input = [&](Vector2d point, bool down, bool press, bool release) {
    shell.queueDocumentSpaceReplayInputForTesting(
        EditorShellDocumentReplayInput{.documentPoint = point,
                                       .leftMouseDown = down,
                                       .leftMousePressed = press,
                                       .leftMouseReleased = release,
                                       .modifiers = MouseModifiers{.command = true}});
    EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  };
  input({10, 20}, true, true, false);
  for (const tests::HangGuard guard; guard.active();) {
    RunShellFrame(window, shell);
    if (!EditorShellTestAccess::RendererBusy(shell) && !app.document().hasPendingMutations()) {
      break;
    }
  }
  using Barrier = AsyncRenderer::CompletionBarrier;
  struct ReleaseBarrier {
    ~ReleaseBarrier() { (void)AsyncRenderer::releaseCompletionBarrierForTesting(); }
  } releaseBarrier;
  EXPECT_EQ(AsyncRenderer::requestCompletionBarrierForTesting(), Barrier::Off);
  AsyncRenderer::armRequestedCompletionBarrierForTesting();
  app.applyMutation(EditorCommand::SetAttributeCommand(
      *app.document().document().querySelector("#render-demand"), "x", "-101"));
  ASSERT_TRUE(app.flushFrame());
  AsyncRenderer& renderer =
      EditorShellTestAccess::BeginDelayedRender(shell, std::chrono::milliseconds(0));
  for (const tests::HangGuard guard;
       AsyncRenderer::completionBarrierForTesting() != Barrier::Holding && guard.active();) {
    std::this_thread::sleep_for(tests::kPollInterval);
  }
  ASSERT_EQ(AsyncRenderer::completionBarrierForTesting(), Barrier::Holding);
  input({20, 20}, true, false, false);
  input({30, 20}, false, false, true);
  EditorShellTestAccess::BufferPendingClick(shell, {50, 20}, MouseModifiers{.command = true});
  RunShellFrame(window, shell);
  EXPECT_TRUE(EditorShellTestAccess::HasPendingClick(shell));
  EXPECT_THAT(app.document().document().querySelector("#p")->getAttribute("d"),
              testing::Optional(testing::Eq(RcString("M10 20 L50 20"))));
  EXPECT_EQ(AsyncRenderer::releaseCompletionBarrierForTesting(), Barrier::Holding);
  ASSERT_TRUE(renderer.waitUntilNoRenderInFlightForTesting(std::chrono::steady_clock::now() +
                                                           std::chrono::seconds(2)));
  const Vector2d nextScreen = shell.viewportForReadback().documentToScreen({50, 20});
  ImGui::GetIO().AddMousePosEvent(nextScreen.x, nextScreen.y);
  ImGui::GetIO().AddMouseButtonEvent(0, true);
  for (const tests::HangGuard guard; guard.active();) {
    RunShellFrame(window, shell);
    if (!EditorShellTestAccess::HasPendingClick(shell)) {
      break;
    }
  }
  const std::string firstSource(app.document().document().source());
  EXPECT_THAT(app.document().document().querySelector("#p")->getAttribute("d"),
              testing::Optional(testing::Eq(RcString("M 30 20 L 50 20"))));
  EXPECT_EQ(app.undoTimeline().entryCount(), 1u);
  input({70, 20}, true, false, false);
  input({80, 20}, false, false, true);
  ImGui::GetIO().AddMouseButtonEvent(0, false);
  for (const tests::HangGuard guard; guard.active();) {
    RunShellFrame(window, shell);
    if (!EditorShellTestAccess::RendererBusy(shell) && !app.document().hasPendingMutations() &&
        !EditorShellTestAccess::PenToolIsDraggingAnchor(shell)) {
      break;
    }
  }
  const std::string finalSource(app.document().document().source());
  EXPECT_THAT(app.document().document().querySelector("#p")->getAttribute("d"),
              testing::Optional(testing::Eq(RcString("M 30 20 L 80 20"))));
  EXPECT_EQ(app.undoTimeline().entryCount(), 2u);
  app.undo();
  (void)app.flushFrame();
  EXPECT_EQ(app.document().document().source(), firstSource);
  app.undo();
  (void)app.flushFrame();
  EXPECT_THAT(app.document().document().querySelector("#p")->getAttribute("d"),
              testing::Optional(testing::Eq(RcString("M10 20 L50 20"))));
  app.redo();
  (void)app.flushFrame();
  EXPECT_EQ(app.document().document().source(), firstSource);
  app.redo();
  (void)app.flushFrame();
  EXPECT_EQ(app.document().document().source(), finalSource);
}

TEST(EditorShellTest, SourcePaneVisibilityRevealAndHoverRangesUseCurrentDocumentSource) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  const std::vector<SourceByteRange> ranges =
      EditorShellTestAccess::SourceHoverRangesForElements(shell, {*target});
  ASSERT_FALSE(ranges.empty());
  EXPECT_LT(ranges.front().start, ranges.front().end);

  EditorShellTestAccess::SetSourcePaneVisible(shell, true);
  ASSERT_TRUE(EditorShellTestAccess::Source(shell).setHoverSourceRanges(ranges));
  EditorShellTestAccess::SetSourcePaneVisible(shell, false);
  EXPECT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).hoverSourceRanges().empty());

  EditorShellTestAccess::RevealSourceRange(shell, ranges.front());
  EXPECT_TRUE(EditorShellTestAccess::SourcePaneVisible(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).nextFlashWakeSeconds().has_value());

  EditorShellTestAccess::Source(shell).setText("out of sync");
  EXPECT_TRUE(EditorShellTestAccess::SourceHoverRangesForElements(shell, {*target}).empty());
}

TEST(EditorShellTest, StyleFocusSelectionPathUsesSourceCursorContext) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());

  const std::string sourceText = EditorShellTestAccess::Source(shell).getText();
  const std::size_t styleOffset = sourceText.find("fill");
  ASSERT_NE(styleOffset, std::string::npos);

  std::optional<StyleFocus> styleFocus =
      EditorShellTestAccess::StyleFocusAtSourceOffset(shell, styleOffset);
  ASSERT_TRUE(styleFocus.has_value());
  EXPECT_FALSE(styleFocus->impactedElements.empty());

  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.setSelection(source.getCoordinatesAtByteOffset(styleOffset),
                      source.getCoordinatesAtByteOffset(styleOffset));
  EditorShellTestAccess::ApplyStyleFocus(shell, *styleFocus);

  EXPECT_TRUE(EditorShellTestAccess::SourceFocusOriginatedInStyle(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).selectedElements().empty());

  EditorShellTestAccess::SetSourceFocusMode(shell, true);
  EXPECT_TRUE(EditorShellTestAccess::SourceFocusMode(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).hasFocusPartition());
}

TEST(EditorShellTest, ReferenceHighlightPreviewCombinesElementsAndSourceRanges) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  EditorShellTestAccess::RefreshReferenceHighlightSummaryIfNeeded(shell);
  const ReferenceHighlightSummary& summary = EditorShellTestAccess::ReferenceSummary(shell);
  EXPECT_EQ(summary.referencedElements.size(), 2u);
  EXPECT_TRUE(summary.referencingElements.empty());
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightElements(shell).empty());

  EditorShellTestAccess::SetReferenceHighlightChipHovered(shell, true);
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightChipHovered(shell));
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightActive(shell));
  EXPECT_EQ(EditorShellTestAccess::ReferenceHighlightElements(shell).size(), 2u);
  EXPECT_EQ(EditorShellTestAccess::CombinedSourcePreviewElements(shell).size(), 2u);
  EXPECT_EQ(EditorShellTestAccess::Source(shell).hoverSourceRanges().size(), 2u);

  EditorShellTestAccess::SetReferenceHighlightChipHovered(shell, false);
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightChipHovered(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).hoverSourceRanges().empty());

  EditorShellTestAccess::SetReferenceHighlightChipHovered(shell, false);
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightChipHovered(shell));
}

TEST(EditorShellTest, ReferenceHighlightSummaryClearsForEmptySelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::RefreshReferenceHighlightSummaryIfNeeded(shell);

  EXPECT_EQ(EditorShellTestAccess::ReferenceSummary(shell).totalCount(), 0u);
  EXPECT_EQ(EditorShellTestAccess::LastReferenceHighlightSelectionSize(shell), 0u);
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightElements(shell).empty());
  EXPECT_TRUE(EditorShellTestAccess::CombinedSourcePreviewElements(shell).empty());
}

// A press that changes the selection starts a render, and the reference summary must wait for it.
// Waiting by requesting a frame every frame redraws at display rate for the whole render; in the
// browser those frames hold the GPU owner thread that the render itself is waiting on.
TEST(EditorShellTest, ReferenceHighlightWaitsForRendererWakeInsteadOfRequestingFrames) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());
  AsyncRenderer& renderer = shell.asyncRendererForReplay();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  for (int frame = 0; frame < 40; ++frame) {
    RunShellFrame(window, shell);
    ASSERT_TRUE(renderer.waitUntilNoRenderInFlightForTesting(deadline));
    if (frame >= 3 && !EditorShellTestAccess::RendererBusy(shell)) {
      break;
    }
  }
  ASSERT_FALSE(EditorShellTestAccess::RendererBusy(shell));

  // Start a render that stays in flight, then change the selection behind it.
  renderer.setReplayDocumentAccessBlockedForTesting(true);
  EditorShellTestAccess::Coordinator(shell).requestPresentationRefresh();
  EditorShellTestAccess::SetRequestRenderAtEndOfFrame(shell);
  RunShellFrame(window, shell);
  ASSERT_TRUE(renderer.hasRenderInFlightForTesting());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  // The change may ask for a follow-up frame, but the loop must then be able to park.
  std::uint64_t blockedWakeSerial = window.wakeEventLoopRequestSerialForTesting();
  int quietFrames = 0;
  for (int frame = 0; frame < 6 && quietFrames < 3; ++frame) {
    RunShellFrame(window, shell);
    const std::uint64_t wakeSerial = window.wakeEventLoopRequestSerialForTesting();
    quietFrames = wakeSerial == blockedWakeSerial ? quietFrames + 1 : 0;
    blockedWakeSerial = wakeSerial;
  }
  EXPECT_EQ(quietFrames, 3) << "the editor kept requesting frames while the render was running";
  EXPECT_EQ(EditorShellTestAccess::LastReferenceHighlightSelectionSize(shell), 0u);
  ASSERT_TRUE(renderer.hasRenderInFlightForTesting());

  // Drive the loop the way the browser does: a frame runs only for an outstanding wake. The
  // renderer's own completion and warmup-release wakes must carry the summary to completion.
  renderer.setReplayDocumentAccessBlockedForTesting(false);
  const auto completionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  std::uint64_t consumedWakeSerial = blockedWakeSerial;
  for (int frame = 0; frame < 20; ++frame) {
    if (EditorShellTestAccess::LastReferenceHighlightSelectionSize(shell) == 1u) {
      break;
    }
    while (window.wakeEventLoopRequestSerialForTesting() == consumedWakeSerial &&
           std::chrono::steady_clock::now() < completionDeadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_GT(window.wakeEventLoopRequestSerialForTesting(), consumedWakeSerial)
        << "no wake within 10 s: the demand-driven loop would park with a stale reference summary "
        << "(renderer busy=" << EditorShellTestAccess::RendererBusy(shell) << ")";
    consumedWakeSerial = window.wakeEventLoopRequestSerialForTesting();
    RunShellFrame(window, shell);
  }
  EXPECT_EQ(EditorShellTestAccess::LastReferenceHighlightSelectionSize(shell), 1u);
  EXPECT_EQ(EditorShellTestAccess::ReferenceSummary(shell).referencedElements.size(), 2u);
}

// A sample's first presentation owes the sidebars a snapshot refresh, which only an idle frame may
// take. A render that is already running when it becomes due, such as the render-tree rebuild after
// a canvas commit, must not turn that obligation into a frame request on every frame: in the
// browser those frames hold the GPU owner thread that the render, and a press that cancels it, are
// waiting on.
TEST(EditorShellTest, SidebarRefreshWaitsForRendererWakeInsteadOfRequestingFrames) {
  // The sidebars, and with them the owed refresh, are only drawn in the desktop layout.
  gui::EditorWindow window(TestWindowOptions({
      .title = "Sidebar refresh behind a busy renderer",
      .initialWidth = 1600,
      .initialHeight = 900,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::SetAdaptiveUiLayout(shell, ComputeEditorAdaptiveUiLayout({
                                                        .windowWidth = 1600.0f,
                                                        .windowHeight = 900.0f,
                                                        .preferTouch = false,
                                                    }));
  ASSERT_FALSE(EditorShellTestAccess::AdaptiveUiLayout(shell).compactTouch());
  AsyncRenderer& renderer = shell.asyncRendererForReplay();
  const tests::HangGuard settleGuard;
  for (int frame = 0; frame < 40; ++frame) {
    RunShellFrame(window, shell);
    ASSERT_TRUE(renderer.waitUntilNoRenderInFlightForTesting(settleGuard.deadline()));
    if (frame >= 3 && !EditorShellTestAccess::RendererBusy(shell)) {
      break;
    }
  }
  ASSERT_FALSE(EditorShellTestAccess::RendererBusy(shell));
  EditorShellTestAccess::SetShowSamplePicker(shell, false);
  ASSERT_TRUE(EditorShellTestAccess::ForegroundPresentationAcknowledged(shell));

  // Start a render that stays in flight, then make the sidebar refresh due behind it.
  renderer.setReplayDocumentAccessBlockedForTesting(true);
  EditorShellTestAccess::Coordinator(shell).requestPresentationRefresh();
  EditorShellTestAccess::SetRequestRenderAtEndOfFrame(shell);
  RunShellFrame(window, shell);
  ASSERT_TRUE(renderer.hasRenderInFlightForTesting());
  EditorShellTestAccess::SetSidebarSnapshotRefreshPending(shell, true);

  std::uint64_t blockedWakeSerial = window.wakeEventLoopRequestSerialForTesting();
  int quietFrames = 0;
  for (int frame = 0; frame < 6 && quietFrames < 3; ++frame) {
    RunShellFrame(window, shell);
    const std::uint64_t wakeSerial = window.wakeEventLoopRequestSerialForTesting();
    quietFrames = wakeSerial == blockedWakeSerial ? quietFrames + 1 : 0;
    blockedWakeSerial = wakeSerial;
  }
  EXPECT_EQ(quietFrames, 3) << "the editor kept requesting frames while the render was running";
  EXPECT_TRUE(EditorShellTestAccess::SidebarSnapshotRefreshPending(shell));
  ASSERT_TRUE(renderer.hasRenderInFlightForTesting());

  // Drive the loop the way the browser does: a frame runs only for an outstanding wake. The
  // renderer's completion wake must carry the owed refresh to an idle frame.
  renderer.setReplayDocumentAccessBlockedForTesting(false);
  const tests::HangGuard completionGuard;
  std::uint64_t consumedWakeSerial = blockedWakeSerial;
  for (int frame = 0; frame < 20 && EditorShellTestAccess::SidebarSnapshotRefreshPending(shell);
       ++frame) {
    while (window.wakeEventLoopRequestSerialForTesting() == consumedWakeSerial &&
           completionGuard.active()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ASSERT_GT(window.wakeEventLoopRequestSerialForTesting(), consumedWakeSerial)
        << "no wake before the hang guard: the demand-driven loop would park with the sidebar "
        << "refresh still owed (renderer busy=" << EditorShellTestAccess::RendererBusy(shell)
        << ")";
    consumedWakeSerial = window.wakeEventLoopRequestSerialForTesting();
    RunShellFrame(window, shell);
  }
  EXPECT_FALSE(EditorShellTestAccess::SidebarSnapshotRefreshPending(shell));
}

TEST(EditorShellTest, ShellGeometryHelpersClampToViewportAndSelectionCache) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  EditorShellTestAccess::RefreshReferenceHighlightSummaryIfNeeded(shell);

  const std::optional<Box2d> sizeChip =
      EditorShellTestAccess::SelectionSizeChipScreenRect(shell, "40 x 24", Vector2d(30.0, 42.0));
  ASSERT_TRUE(sizeChip.has_value());
  EXPECT_GE(sizeChip->topLeft.x, 20.0);
  EXPECT_GE(sizeChip->topLeft.y, 30.0);

  const std::optional<Box2d> noSizeChip =
      EditorShellTestAccess::SelectionSizeChipScreenRect(shell, "", Vector2d(30.0, 42.0));
  EXPECT_FALSE(noSizeChip.has_value());

  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightChipScreenRect(shell, "").has_value());
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightChipScreenRect(shell, "-> 2").has_value());

  const Box2d palette = EditorShellTestAccess::ToolPaletteScreenRect(shell, ImVec2(10.0f, 20.0f),
                                                                     ImVec2(500.0f, 300.0f));
  EXPECT_GT(palette.width(), 0.0);
  EXPECT_GT(palette.height(), 0.0);

  const EditorAdaptiveUiLayout compactLandscape = ComputeEditorAdaptiveUiLayout({
      .windowWidth = 844.0f,
      .windowHeight = 390.0f,
      .preferTouch = true,
  });
  EditorShellTestAccess::SetAdaptiveUiLayout(shell, compactLandscape);
  EditorShellTestAccess::SetCompactPanelVisible(shell, true);
  const Box2d compactPalette = EditorShellTestAccess::ToolPaletteScreenRect(
      shell, ImVec2(0.0f, compactLandscape.topBarHeight),
      ImVec2(844.0f, 390.0f - compactLandscape.topBarHeight));
  EXPECT_LE(compactPalette.bottomRight.x, compactLandscape.panelX);
  EXPECT_FLOAT_EQ(compactPalette.width(), 204.0f);
}

TEST(EditorShellTest, PendingPreviewRetriesWhenAdmissionWakePrecedesResultPolling) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  const auto store = shell.fontCatalog().encodedStore();
  ASSERT_NE(store, nullptr);
  ASSERT_FALSE(svg::CatalogFontAssets().empty());
  const auto& asset = svg::CatalogFontAssets().front();
  auto admission = store->tryAcquireDecode(asset.contentId);
  ASSERT_EQ(admission.state, svg::FontFaceLoadState::Resolving);
  const auto requestWake = store->wakeRevision();
  auto result = EditorShellTestAccess::PendingSampleFontResult(
      shell,
      {.family = asset.family,
       .availability = shell.fontCatalog().availability(asset.family, {}),
       .state = svg::FontFaceLoadState::WaitingForAdmission},
      requestWake);

  // The preview document has already returned; only its copied result survives this release.
  admission.reservation.reset();
  ASSERT_GT(store->wakeRevision(), requestWake);
  EditorShellTestAccess::ConsumePreviewResult(shell, std::move(result));
  ASSERT_EQ(EditorShellTestAccess::PendingSampleFontCount(shell), 1u);
  EXPECT_EQ(EditorShellTestAccess::FinishedSampleCount(shell), 0u);
  EditorShellTestAccess::RetryPendingFontPreviews(shell);
  EXPECT_EQ(EditorShellTestAccess::PendingSampleFontCount(shell), 0u);
  EXPECT_EQ(EditorShellTestAccess::FinishedSampleCount(shell), 0u)
      << "Admission eligibility schedules another attempt; it never marks fallback final";
}

TEST(EditorShellTest, HiddenPreviewCancelsItsWaitBeforeAdmissionRelease) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  const auto store = shell.fontCatalog().encodedStore();
  ASSERT_FALSE(svg::CatalogFontAssets().empty());
  const auto& asset = svg::CatalogFontAssets().front();
  auto admission = store->tryAcquireDecode(asset.contentId);
  EditorShellTestAccess::ConsumePreviewResult(
      shell, EditorShellTestAccess::PendingSampleFontResult(
                 shell,
                 {.family = asset.family,
                  .availability = shell.fontCatalog().availability(asset.family, {}),
                  .state = svg::FontFaceLoadState::WaitingForAdmission},
                 store->wakeRevision()));
  ASSERT_EQ(EditorShellTestAccess::PendingSampleFontCount(shell), 1u);
  EditorShellTestAccess::SetShowSamplePicker(shell, false);
  EditorShellTestAccess::UpdateVisiblePreviewTasks(shell);
  admission.reservation.reset();
  EditorShellTestAccess::RetryPendingFontPreviews(shell);
  EXPECT_EQ(EditorShellTestAccess::PendingSampleFontCount(shell), 0u);
  EXPECT_EQ(EditorShellTestAccess::FinishedSampleCount(shell), 0u);
}

TEST(EditorShellTest, CancelledPreviewGenerationRejectsLateResultForTheSameCard) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  auto oldResult = EditorShellTestAccess::PendingSampleFontResult(
      shell, {.family = "Inter", .state = svg::FontFaceLoadState::WaitingForBytes}, 0);
  EditorShellTestAccess::CancelPreviews(shell);
  auto currentResult = EditorShellTestAccess::PendingSampleFontResult(
      shell, {.family = "Inter", .state = svg::FontFaceLoadState::WaitingForBytes}, 0);
  EditorShellTestAccess::ConsumePreviewResult(shell, std::move(oldResult));
  EXPECT_EQ(EditorShellTestAccess::PendingSampleFontCount(shell), 0u);
  EditorShellTestAccess::ConsumePreviewResult(shell, std::move(currentResult));
  EXPECT_EQ(EditorShellTestAccess::PendingSampleFontCount(shell), 1u);
  EXPECT_EQ(EditorShellTestAccess::FinishedSampleCount(shell), 0u);
}

TEST(EditorShellTest, OutputFontDemandDeduplicatesAssetsAndCancelsAfterSourceMutation) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  ASSERT_FALSE(svg::CatalogFontAssets().empty());
  const auto& asset = svg::CatalogFontAssets().front();
  const auto availability = shell.fontCatalog().availability(asset.family, {});
  const std::array<svg::FontFaceDependency, 2> faces = {
      svg::FontFaceDependency{
          .family = asset.family, .request = {.weight = 400}, .availability = availability},
      svg::FontFaceDependency{
          .family = asset.family, .request = {.weight = 700}, .availability = availability},
  };
  EditorShellTestAccess::RememberOutputFonts(shell, faces);
  EXPECT_EQ(EditorShellTestAccess::OutputFontDemandCount(shell), 1u);
  auto& app = EditorShellTestAccess::App(shell);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target);
  app.setSelection(*target);
  ASSERT_TRUE(app.setAttributeOnSelection("fill", "red"));
  ASSERT_TRUE(app.flushFrame());
  EditorShellTestAccess::DrainOutputFonts(shell);
  EXPECT_EQ(EditorShellTestAccess::OutputFontDemandCount(shell), 0u);
  const auto fill = app.document().document().querySelector("#target")->getAttribute("fill");
  ASSERT_TRUE(fill);
  EXPECT_EQ(std::string_view(*fill), "red");
}

TEST(EditorShellTest, FormatBarSnapshotDoesNotWaitForDocumentWriter) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(
      window, OptionsWithSource(
                  R"(<svg xmlns="http://www.w3.org/2000/svg"><text id="label" font-family="serif"
      font-size="24" font-weight="bold">Hello</text></svg>)"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  auto text = app.document().document().querySelector("#label");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);
  const FormatBarState before = EditorShellTestAccess::ComputeFormatBarState(shell);
  ASSERT_TRUE(before.visible);
  app.document().document().setThreadingMode(svg::ThreadingMode::ConcurrentDom);

  std::promise<void> writerReady;
  std::promise<void> releaseWriter;
  auto release = releaseWriter.get_future();
  auto writer = std::async(std::launch::async, [&] {
    auto access = app.document().document().writeAccess();
    writerReady.set_value();
    release.wait();
  });
  writerReady.get_future().wait();
  auto format = std::async(std::launch::async, [&] {
    const bool visible = EditorShellTestAccess::FormatBarShouldShow(shell);
    return std::make_pair(visible, EditorShellTestAccess::ComputeFormatBarState(shell));
  });
  const auto status = format.wait_for(std::chrono::milliseconds(100));
  releaseWriter.set_value();
  writer.get();
  const auto [visible, state] = format.get();
  EXPECT_EQ(status, std::future_status::ready);
  EXPECT_TRUE(visible);
  EXPECT_TRUE(state.visible);
  EXPECT_EQ(state.fontFamily, before.fontFamily);
  EXPECT_EQ(state.fontSize, before.fontSize);
  EXPECT_EQ(state.bold, before.bold);
}

// An edit from the format bar drops the text's computed style until the next
// render. The bar keeps drawing its snapshot meanwhile, so it must also keep
// reserving its input region; otherwise a quick second click falls through to
// the canvas.
TEST(EditorShellTest, FormatBarKeepsItsInputRegionWhileAnEditAwaitsRestyle) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(
                                R"(<svg xmlns="http://www.w3.org/2000/svg"><g font-size="20">
      <text id="label">Hello</text></g></svg>)"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  auto text = app.document().document().querySelector("#label");
  ASSERT_TRUE(text.has_value());
  app.setSelection(*text);
  ASSERT_THAT(EditorShellTestAccess::ComputeFormatBarState(shell).visible, testing::IsTrue());

  ASSERT_THAT(app.setAttributeOnSelection("font-size", "30"), testing::IsTrue());
  ASSERT_THAT(app.flushFrame(), testing::IsTrue());
  ASSERT_THAT(text->computedStyleIfPresent(), testing::IsNull())
      << "precondition: the edit leaves the text without a computed style until restyled";

  const FormatBarState state = EditorShellTestAccess::ComputeFormatBarState(shell);
  EXPECT_THAT(state.visible, testing::IsTrue());
  EXPECT_THAT(EditorShellTestAccess::FormatBarShouldShow(shell), testing::Eq(state.visible));
}

TEST(EditorShellTest, AnimationPreviewDefersSidebarWorkUntilEditIsRestored) {
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(
                                R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="16">
        <rect width="8" height="8" fill="red">
          <animate attributeName="x" from="0" to="16" dur="2s"/>
        </rect>
      </svg>)"));
  ASSERT_TRUE(shell.valid());
  ASSERT_FALSE(EditorShellTestAccess::RendererBusy(shell));
  auto& document = EditorShellTestAccess::App(shell).document();
  const std::string source(document.document().source());
  window.beginFrame();
  EditorShellTestAccess::RenderSidebars(shell);
  window.endFrame();
  const auto inspections = EditorShellTestAccess::LayerThumbnailCacheInspections(shell);
  ASSERT_GT(inspections, 0u);
  const auto refreshCount = EditorShellTestAccess::LayerThumbnailRefreshCount(shell);
  const auto renderAttempts = EditorShellTestAccess::LayerThumbnailRenderAttempts(shell);

  EditorShellTestAccess::SetAnimationPreview(shell, true);
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 10.0);
  ASSERT_TRUE(EditorShellTestAccess::IsAnimationPreview(shell));
  ASSERT_FALSE(EditorShellTestAccess::AnimationResetPending(shell));
  EditorShellTestAccess::SetSidebarSnapshotRefreshPending(shell, true);
  for (const double time : {0.5, 1.0, 1.5}) {
    SCOPED_TRACE(time);
    ASSERT_TRUE(document.setAnimationTime(time));
    window.beginFrame();
    const auto wakeSerial = window.wakeEventLoopRequestSerialForTesting();
    EditorShellTestAccess::RenderSidebars(shell);
    EXPECT_EQ(window.wakeEventLoopRequestSerialForTesting(), wakeSerial);
    EXPECT_EQ(EditorShellTestAccess::LayerThumbnailCacheInspections(shell), inspections);
    EXPECT_EQ(EditorShellTestAccess::LayerThumbnailRefreshCount(shell), refreshCount);
    EXPECT_EQ(EditorShellTestAccess::LayerThumbnailRenderAttempts(shell), renderAttempts);
    EXPECT_TRUE(EditorShellTestAccess::SidebarSnapshotRefreshPending(shell));
    window.endFrame();
  }

  EditorShellTestAccess::SetAnimationPreview(shell, false);
  ASSERT_TRUE(EditorShellTestAccess::AnimationResetPending(shell));
  window.beginFrame();
  const auto resetWakeSerial = window.wakeEventLoopRequestSerialForTesting();
  EditorShellTestAccess::RenderSidebars(shell);
  EXPECT_EQ(window.wakeEventLoopRequestSerialForTesting(), resetWakeSerial);
  EXPECT_EQ(EditorShellTestAccess::LayerThumbnailCacheInspections(shell), inspections);
  EXPECT_EQ(EditorShellTestAccess::LayerThumbnailRefreshCount(shell), refreshCount);
  EXPECT_EQ(EditorShellTestAccess::LayerThumbnailRenderAttempts(shell), renderAttempts);
  window.endFrame();
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 11.0);
  ASSERT_FALSE(EditorShellTestAccess::AnimationResetPending(shell));
  EXPECT_DOUBLE_EQ(document.document().currentTime(), 0.0);
  window.beginFrame();
  EditorShellTestAccess::RenderSidebars(shell);
  EXPECT_GT(EditorShellTestAccess::LayerThumbnailCacheInspections(shell), inspections);
  EXPECT_GT(EditorShellTestAccess::LayerThumbnailRefreshCount(shell), refreshCount);
  window.endFrame();
  EXPECT_EQ(document.document().source(), source);
}

TEST(EditorShellTest, AnimationPreviewDefersFontPreviewWorkUntilEditIsRestored) {
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(
      window,
      OptionsWithSource(R"(<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0"/>)"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::PrepareSingleFontPreviewRequest(shell, "Bebas Neue");
  const auto requested = EditorShellTestAccess::RequestedAuxiliaryPreviewCount(shell);
  EditorShellTestAccess::SetAnimationPreview(shell, true);
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 10.0);
  ASSERT_TRUE(EditorShellTestAccess::IsAnimationPreview(shell));
  ASSERT_FALSE(EditorShellTestAccess::AnimationResetPending(shell));
  EditorShellTestAccess::AdvanceVisiblePreviews(shell);
  EXPECT_EQ(EditorShellTestAccess::RequestedAuxiliaryPreviewCount(shell), requested);
  EXPECT_EQ(EditorShellTestAccess::PendingFontPreviewCount(shell), 1u);
  EditorShellTestAccess::SetAnimationPreview(shell, false);
  EditorShellTestAccess::AdvanceVisiblePreviews(shell);
  EXPECT_EQ(EditorShellTestAccess::RequestedAuxiliaryPreviewCount(shell), requested);
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 11.0);
  EditorShellTestAccess::AdvanceVisiblePreviews(shell);
  EXPECT_GT(EditorShellTestAccess::RequestedAuxiliaryPreviewCount(shell), requested);
}

TEST(EditorShellTest, AnimationPreviewCancelsAndPreservesActiveFontPreview) {
  gui::EditorWindow window = MakeHiddenWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(
      window,
      OptionsWithSource(R"(<svg xmlns="http://www.w3.org/2000/svg" width="0" height="0"/>)"));
  ASSERT_TRUE(shell.valid());
  auto& worker = EditorShellTestAccess::Coordinator(shell).asyncRenderer();
  worker.setSampleThumbnailRenderDelayForTesting(std::chrono::seconds(5));
  EditorShellTestAccess::PrepareSingleFontPreviewRequest(shell, "Bebas Neue");
  EditorShellTestAccess::AdvanceVisiblePreviews(shell);
  for (const tests::HangGuard guard;
       guard.active() && !worker.sampleThumbnailRenderStats().active;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  ASSERT_TRUE(worker.sampleThumbnailRenderStats().active);
  EditorShellTestAccess::SetAnimationPreview(shell, true);
  EXPECT_EQ(EditorShellTestAccess::PendingFontPreviewCount(shell), 1u);
  for (const tests::HangGuard guard;
       guard.active() && worker.sampleThumbnailRenderStats().active;) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_FALSE(worker.sampleThumbnailRenderStats().active);
  EXPECT_EQ(worker.sampleThumbnailRenderStats().cancelled, 1u);
  EXPECT_EQ(EditorShellTestAccess::CachedFontPreviewCount(shell), 0u);
  worker.setSampleThumbnailRenderDelayForTesting(std::chrono::milliseconds(0));
  EditorShellTestAccess::SetAnimationPreview(shell, false);
  EditorShellTestAccess::AdvanceAnimationPreview(shell, 11.0);
  FormatBarFontPreview preview;
  for (const tests::HangGuard guard; guard.active() && !preview.available();) {
    EditorShellTestAccess::AdvanceVisiblePreviews(shell);
    preview = EditorShellTestAccess::FontPreviewForFamily(shell, "Bebas Neue");
    if (!preview.available()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  EXPECT_TRUE(preview.available()) << "Returning to Edit must resume the cancelled font preview";
}

TEST(EditorShellTest, TextFormatBarPrewarmsCatalogFamilyInItsOwnFace) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(R"(<svg xmlns="http://www.w3.org/2000/svg"/>)"));
  ASSERT_TRUE(shell.valid());
  EXPECT_EQ(EditorShellTestAccess::CachedFontPreviewCount(shell), 0u)
      << "Background previews must leave construction responsive";

  EditorShellTestAccess::RequestFontPreviews(shell, {"Bebas Neue"});
  FormatBarFontPreview preview;
  for (const tests::HangGuard guard; guard.active() && !preview.available();) {
    EditorShellTestAccess::AdvanceFontPreviewGeneration(shell);
    preview = EditorShellTestAccess::FontPreviewForFamily(shell, "Bebas Neue");
    if (!preview.available()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }
  EXPECT_TRUE(preview.available());
  EXPECT_EQ(EditorShellTestAccess::CachedFontPreviewCount(shell), 1u);
  EXPECT_EQ(preview.width, 196.0f);
  EXPECT_EQ(preview.height, 24.0f);
}

TEST(EditorShellTest, FontPreviewWarmsFromDiskBeforeWorkerRender) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  const auto cachePath =
      std::filesystem::temp_directory_path() /
      ("donner-preview-warm-" + std::to_string(reinterpret_cast<std::uintptr_t>(&window)));
  std::filesystem::create_directories(cachePath);
  auto options = OptionsWithSource(R"(<svg xmlns="http://www.w3.org/2000/svg"/>)");
  options.editorBuildInfo = "test-build\ntest-commit\n";
  options.fontPreviewCachePath = cachePath.string();
  {
    EditorShell shell(window, options);
    ASSERT_TRUE(shell.valid());
    EditorShellTestAccess::RequestFontPreviews(shell, {"Bebas Neue"});
    FormatBarFontPreview preview;
    for (const tests::HangGuard guard; guard.active() && !preview.available();) {
      EditorShellTestAccess::AdvanceFontPreviewGeneration(shell);
      preview = EditorShellTestAccess::FontPreviewForFamily(shell, "Bebas Neue");
      if (!preview.available()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    }
    ASSERT_TRUE(preview.available());
    const auto dependencies = EditorShellTestAccess::FontPreviewDependencies(shell, "Bebas Neue");
    ASSERT_EQ(dependencies.size(), 1u);
    EXPECT_EQ(dependencies.front().family, "bebas neue");
    EXPECT_FALSE(dependencies.front().availability.contentId.empty());
  }
  std::size_t svgCount = 0;
  for (const auto& entry : std::filesystem::directory_iterator(cachePath)) {
    if (entry.path().extension() == ".svg") {
      ++svgCount;
    }
  }
  EXPECT_EQ(svgCount, 1u);

  {
    EditorShell shell(window, options);
    ASSERT_TRUE(shell.valid());
    EditorShellTestAccess::RequestFontPreviews(shell, {"Bebas Neue"});
    EditorShellTestAccess::AdvanceFontPreviewGeneration(shell);
    EXPECT_EQ(EditorShellTestAccess::CachedFontPreviewCount(shell), 1u);
    EXPECT_TRUE(EditorShellTestAccess::FontPreviewForFamily(shell, "Bebas Neue").available());
  }
  std::filesystem::remove_all(cachePath);
}

TEST(EditorShellTest, PrivateUiRenderHelpersCoverPaneToolbarAndPanelStates) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  if (!ImGui::GetIO().Fonts->IsBuilt()) {
    ImGui::GetIO().Fonts->Build();
  }
  window.beginFrame();
  ImGuiIO& io = ImGui::GetIO();
  ASSERT_FALSE(io.Fonts->Fonts.empty());
  EditorShellTestAccess::RenderSourcePane(shell, /*paneOriginY=*/0.0f, /*paneHeight=*/180.0f,
                                          /*paneWidth=*/260.0f, io.Fonts->Fonts[0]);

  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(280.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(340.0f, 220.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellPrivateUiHost", nullptr, kHostFlags);
  EditorShellTestAccess::RenderFillStrokeToolbarWidget(shell);
  EditorShellTestAccess::RenderLayerPanelContents(shell);
  ImGui::End();

  EditorShellTestAccess::RenderSourcePaneSplitter(shell, /*windowWidth=*/640.0f,
                                                  /*paneOriginY=*/0.0f, /*paneHeight=*/220.0f,
                                                  /*sourcePaneWidth=*/260.0f);
  window.endFrame();
  const ImTextureID fontTextureBeforeSecondShell = ImGui::GetIO().Fonts->TexID;
  ASSERT_NE(fontTextureBeforeSecondShell, 0u);

  EditorShellTestAccess::SetSourcePaneVisible(shell, false);
  EditorShellTestAccess::SetSourceFocusMode(shell, true);
  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>"));
  ASSERT_TRUE(invalidShell.valid());
  EXPECT_EQ(ImGui::GetIO().Fonts->TexID, fontTextureBeforeSecondShell)
      << "A second shell sharing the ImGui context must reuse its font atlas.";

  if (!ImGui::GetIO().Fonts->IsBuilt()) {
    ImGui::GetIO().Fonts->Build();
  }
  window.beginFrame();
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(240.0f, 80.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellInvalidToolbarHost", nullptr, kHostFlags);
  EditorShellTestAccess::RenderFillStrokeToolbarWidget(invalidShell);
  EditorShellTestAccess::RenderLayerPanelContents(invalidShell);
  ImGui::End();
  EditorShellTestAccess::RenderSourcePaneSplitter(shell, /*windowWidth=*/640.0f,
                                                  /*paneOriginY=*/0.0f, /*paneHeight=*/220.0f,
                                                  /*sourcePaneWidth=*/0.0f);
  window.endFrame();

  EXPECT_TRUE(shell.valid());
  EXPECT_TRUE(invalidShell.valid());
}

TEST(EditorShellTest, SecondShellReusesAtlasFontsWithoutRenaming) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  ImFontAtlas& atlas = *ImGui::GetIO().Fonts;
  const int fontCount = atlas.Fonts.size();
  ASSERT_GT(fontCount, 0);
  ImFont* const codeFont = EditorShellTestAccess::CodeFont(shell);
  ASSERT_NE(codeFont, nullptr);

  EditorShell second(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(second.valid());
  EXPECT_EQ(atlas.Fonts.size(), fontCount)
      << "A second shell must reuse the atlas fonts, not insert duplicates";
  EXPECT_EQ(EditorShellTestAccess::CodeFont(second), codeFont);
  EXPECT_EQ(EditorShellTestAccess::UiBoldFont(second), EditorShellTestAccess::UiBoldFont(shell));

  for (ImFont* font : atlas.Fonts) {
    ASSERT_NE(font, nullptr);
    ASSERT_NE(font->ConfigData, nullptr);
    for (int i = 0; i < font->ConfigDataCount; ++i) {
      const std::string name = font->ConfigData[i].Name;
      EXPECT_EQ(name.find("Donner"), std::string::npos)
          << "Editor fonts must keep their original ImGui names, found: " << name;
    }
  }

  ASSERT_EQ(codeFont->ConfigDataCount, 2);
  EXPECT_TRUE(codeFont->ConfigData[1].MergeMode)
      << "The code font must keep its merged symbol range";
}

TEST(EditorShellTest, SourcePaneSplitterDragCollapsesPane) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::SetSourcePaneVisible(shell, true);

  const Box2d sourceRect = RenderSourcePaneSplitterFrame(
      window, shell, /*sourcePaneWidth=*/260.0f, ImVec2(-100.0f, -100.0f), /*mouseDown=*/false);
  const ImVec2 sourceCenter(
      static_cast<float>((sourceRect.topLeft.x + sourceRect.bottomRight.x) * 0.5),
      static_cast<float>((sourceRect.topLeft.y + sourceRect.bottomRight.y) * 0.5));
  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/260.0f, sourceCenter,
                                /*mouseDown=*/true);
  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/260.0f,
                                ImVec2(sourceCenter.x - 120.0f, sourceCenter.y),
                                /*mouseDown=*/true);

  EXPECT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));
}

TEST(EditorShellTest, SourcePaneSplitterDragExpandsVisiblePane) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  EditorShellTestAccess::SetSourcePaneVisible(shell, true);
  EditorShellTestAccess::SetSourcePaneWidth(shell, 260.0f);
  const Box2d sourceRect = RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/260.0f,
                                                         ImVec2(-100.0f, -100.0f),
                                                         /*mouseDown=*/false,
                                                         /*windowWidth=*/1000.0f);
  const ImVec2 sourceCenter(
      static_cast<float>((sourceRect.topLeft.x + sourceRect.bottomRight.x) * 0.5),
      static_cast<float>((sourceRect.topLeft.y + sourceRect.bottomRight.y) * 0.5));
  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/260.0f, sourceCenter,
                                /*mouseDown=*/true, /*windowWidth=*/1000.0f);
  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/260.0f,
                                ImVec2(sourceCenter.x + 80.0f, sourceCenter.y),
                                /*mouseDown=*/true, /*windowWidth=*/1000.0f);

  EXPECT_TRUE(EditorShellTestAccess::SourcePaneVisible(shell));
  EXPECT_GT(EditorShellTestAccess::SourcePaneWidth(shell), 260.0f);
}

TEST(EditorShellTest, SourcePaneRevealRailOpensOnClick) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  ASSERT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));

  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/0.0f, ImVec2(-100.0f, -100.0f),
                                /*mouseDown=*/false);
  const ImVec2 center(16.0f, 110.0f);
  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/0.0f, center,
                                /*mouseDown=*/false);
  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/0.0f, center,
                                /*mouseDown=*/true);
  RenderSourcePaneSplitterFrame(window, shell, /*sourcePaneWidth=*/0.0f, center,
                                /*mouseDown=*/false);

  EXPECT_TRUE(EditorShellTestAccess::SourcePaneVisible(shell));
}

TEST(EditorShellTest, FillStrokeToolbarMouseHitTestingCoversChipsSwatchesAndTooltips) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kPaintToolbarSvg, "paint_toolbar.svg"));
  ASSERT_TRUE(shell.valid());
  svg::SVGDocument& document = EditorShellTestAccess::App(shell).document().document();

  auto selectById = [&](std::string_view id) {
    std::optional<svg::SVGElement> element = document.querySelector("#" + std::string(id));
    ASSERT_TRUE(element.has_value()) << id;
    EditorShellTestAccess::App(shell).setSelection(*element);
  };

  constexpr ImVec2 kCursor(20.0f, 40.0f);
  constexpr ImVec2 kStrokeChip(98.0f, 72.0f);
  constexpr ImVec2 kFillChip(98.0f, 49.0f);
  constexpr ImVec2 kStrokeSwatchOnly(57.0f, 76.0f);
  constexpr ImVec2 kFillSwatchOnly(28.0f, 48.0f);
  constexpr ImVec2 kWidgetBackground(88.0f, 80.0f);

  selectById("local");
  RunFramesUntilDisplayedSelectionBounds(window, shell);
  EditorShellTestAccess::SetSourcePaneVisible(shell, false);
  ClickToolbar(window, shell, kCursor, kFillChip);
  ClickToolbar(window, shell, kCursor, kStrokeChip);
  RenderToolbarFrame(window, shell, kCursor, kStrokeChip, /*mouseDown=*/false);
  ClickToolbar(window, shell, kCursor, kFillSwatchOnly);
  ClickToolbar(window, shell, kCursor, kStrokeSwatchOnly);
  ClickToolbar(window, shell, kCursor, kWidgetBackground);

  for (std::string_view id : {"missing", "contextual", "styled-none-attribute", "external"}) {
    selectById(id);
    RenderToolbarFrame(window, shell, kCursor, kFillChip, /*mouseDown=*/false);
  }

  EXPECT_TRUE(shell.valid());
}

struct PaintSwapFallbackCase {
  const char* name;
  const char* value;
  const char* reference = "#missing";
};

class PaintSwapFallbackTest : public testing::TestWithParam<PaintSwapFallbackCase> {};

TEST_P(PaintSwapFallbackTest, ToolbarSwapPreservesPaintAndSourceRoundTrip) {
  const PaintSwapFallbackCase testCase = GetParam();
  const auto sourceFor = [](std::string_view fill, std::string_view stroke) {
    return std::string(R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64" )") +
           R"(color="#336699"><defs><linearGradient id="paint">)" +
           R"(<stop offset="0" stop-color="red"/><stop offset="1" stop-color="blue"/>)" +
           R"(</linearGradient></defs><rect id="target" x="12" y="12" width="40" height="40" )" +
           R"(stroke-width="4" fill=")" + std::string(fill) + R"(" stroke=")" +
           std::string(stroke) + R"("/></svg>)";
  };
  const std::string referencedPaint =
      std::string("url(") + testCase.reference + ")" +
      (std::string_view(testCase.value).empty() ? "" : std::string(" ") + testCase.value);
  const std::string initialSource = sourceFor(referencedPaint, "blue");
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(initialSource, "paint_fallback.svg"));
  ASSERT_EQ(shell.valid(), true);
  EditorApp& app = EditorShellTestAccess::App(shell);
  svg::SVGDocument& document = app.document().document();
  const auto target = document.querySelector("#target");
  ASSERT_THAT(target, testing::Ne(std::nullopt));
  app.setSelection(*target);
  svg::Renderer actualRenderer;
  actualRenderer.draw(document);
  const svg::RendererBitmap before = actualRenderer.takeSnapshot();
  const svg::PaintServer initialFill = target->getComputedStyle().fill.get().value();
  const svg::PaintServer initialStroke = target->getComputedStyle().stroke.get().value();

  constexpr ImVec2 kCursor(20.0f, 40.0f);
  const auto layout = internal::ComputeFillStrokeWidgetLayout(kCursor, ImVec2(138.0f, 70.0f));
  const ImVec2 swapCenter((layout.swapMin.x + layout.swapMax.x) * 0.5f,
                          (layout.swapMin.y + layout.swapMax.y) * 0.5f);
  ClickToolbar(window, shell, kCursor, swapCenter);
  actualRenderer.draw(document);
  EXPECT_THAT(target->getComputedStyle().fill.get(), testing::Optional(initialStroke));
  EXPECT_THAT(target->getComputedStyle().stroke.get(), testing::Optional(initialFill));

  EditorApp expected;
  ASSERT_EQ(expected.loadFromString(sourceFor("blue", referencedPaint)), true);
  svg::Renderer expectedRenderer;
  expectedRenderer.draw(expected.document().document());
  tests::CompareBitmapToBitmap(actualRenderer.takeSnapshot(), expectedRenderer.takeSnapshot(),
                               std::string("paint_swap_") + testCase.name,
                               tests::PixelmatchIdentityParams());

  EditorApp roundTrip;
  ASSERT_EQ(roundTrip.loadFromString(document.source()), true);
  const auto reparsed = roundTrip.document().document().querySelector("#target");
  ASSERT_THAT(reparsed, testing::Ne(std::nullopt));
  EXPECT_THAT(reparsed->getComputedStyle().fill.get(), testing::Optional(initialStroke));
  EXPECT_THAT(reparsed->getComputedStyle().stroke.get(), testing::Optional(initialFill));

  ClickToolbar(window, shell, kCursor, swapCenter);
  actualRenderer.draw(document);
  EXPECT_THAT(target->getComputedStyle().fill.get(), testing::Optional(initialFill));
  EXPECT_THAT(target->getComputedStyle().stroke.get(), testing::Optional(initialStroke));
  tests::CompareBitmapToBitmap(actualRenderer.takeSnapshot(), before,
                               std::string("paint_swap_back_") + testCase.name,
                               tests::PixelmatchIdentityParams());
}

INSTANTIATE_TEST_SUITE_P(UnresolvedPaint, PaintSwapFallbackTest,
                         testing::Values(PaintSwapFallbackCase{"red", "red"},
                                         PaintSwapFallbackCase{"alpha", "#33669980"},
                                         PaintSwapFallbackCase{"currentColor", "currentColor"},
                                         PaintSwapFallbackCase{"none", "none"},
                                         PaintSwapFallbackCase{"referenceOnly", "", "#paint"}),
                         [](const testing::TestParamInfo<PaintSwapFallbackCase>& info) {
                           return info.param.name;
                         });

TEST(EditorShellTest, FillStrokeSwapAppearanceDoesNotTrackWorkerBusyState) {
  for (const bool preview : {false, true}) {
    SCOPED_TRACE(preview);
    gui::EditorWindow window = MakeHiddenWindow();
    ASSERT_THAT(window.valid(), testing::IsTrue());
    EditorShell shell(window, OptionsWithSource(kInitialSvg, "animation_toolbar.svg"));
    ASSERT_THAT(shell.valid(), testing::IsTrue());
    EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0, 0, 120, 80));
    EditorShellTestAccess::SetAnimationPreview(shell, preview);
    constexpr ImVec2 kCursor(20.0f, 40.0f);
    constexpr ImU32 kEnabledSwap = IM_COL32(215, 222, 232, 255);
    constexpr ImU32 kDisabledSwap = IM_COL32(120, 126, 134, 255);
    const auto checkAppearance = [&] {
      EXPECT_THAT(DrawDataContainsColor(kEnabledSwap), testing::Eq(!preview));
      EXPECT_THAT(DrawDataContainsColor(kDisabledSwap), testing::Eq(preview));
    };
    RenderToolbarFrame(window, shell, kCursor, ImVec2(-100, -100), false);
    checkAppearance();
    AsyncRenderer& renderer = shell.asyncRendererForReplay();
    renderer.setReplayDocumentAccessBlockedForTesting(true);
    EditorShellTestAccess::BeginDelayedRender(shell, std::chrono::milliseconds(0));
    ASSERT_THAT(renderer.isBusy(), testing::IsTrue());
    RenderToolbarFrame(window, shell, kCursor, ImVec2(-100, -100), false);
    checkAppearance();
    renderer.setReplayDocumentAccessBlockedForTesting(false);
    renderer.cancelInFlight();
    ASSERT_THAT(renderer.waitUntilNoRenderInFlightForTesting(std::chrono::steady_clock::now() +
                                                             std::chrono::seconds(2)),
                testing::IsTrue());
    std::ignore = renderer.pollResult();
    RenderToolbarFrame(window, shell, kCursor, ImVec2(-100, -100), false);
    checkAppearance();
  }
}

TEST(EditorShellTest, FillStrokeToolbarKeepsChosenPaintVisibleWhileRendererIsBusy) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  std::optional<svg::SVGElement> target =
      EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  constexpr std::string_view kDifferentAuthoringFill = "#ff00aa";
  EditorShellTestAccess::App(shell).setActiveFill(kDifferentAuthoringFill);
  constexpr ImVec2 kCursor(20.0f, 40.0f);
  RenderToolbarFrame(window, shell, kCursor, ImVec2(-100.0f, -100.0f), /*mouseDown=*/false);
  const std::uint64_t liveSelectionReadsBeforeHandoff =
      EditorShellTestAccess::ToolbarLiveSelectionIdentityReads(shell);
  AsyncRenderer& renderer =
      EditorShellTestAccess::BeginDelayedRender(shell, std::chrono::milliseconds(500));
  ASSERT_TRUE(renderer.isBusy());

  RenderToolbarFrame(window, shell, kCursor, ImVec2(-100.0f, -100.0f), /*mouseDown=*/false);
  EXPECT_EQ(EditorShellTestAccess::ToolbarLiveSelectionIdentityReads(shell),
            liveSelectionReadsBeforeHandoff)
      << "A busy toolbar frame must reuse the pre-handoff selection identity without resolving "
         "the worker-owned registry";
  EXPECT_TRUE(DrawDataContainsColor(IM_COL32(0x33, 0x66, 0xcc, 0xff)))
      << "The selected element's fill swatch must not change while its render is in flight";
  EXPECT_FALSE(DrawDataContainsColor(IM_COL32(0xff, 0x00, 0xaa, 0xff)))
      << "A busy frame must not replace the selected fill with the authoring fill";

  renderer.cancelInFlight();
  EXPECT_TRUE(renderer.waitUntilNoRenderInFlightForTesting(std::chrono::steady_clock::now() +
                                                           std::chrono::seconds(2)));
  std::ignore = renderer.pollResult();
  renderer.setReplayRenderDelayForTesting(std::chrono::milliseconds(0));
}

TEST(EditorShellTest, ReplayCostEstimatesReadTheDocumentWhileTheWorkerRendersIt) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  AsyncRenderer& renderer =
      EditorShellTestAccess::BeginDelayedRender(shell, std::chrono::milliseconds(20));
  ASSERT_TRUE(renderer.isBusy());

  // The replay harness estimates every frame's cost on this thread while the render worker
  // prepares the same document for its frame, so the estimate has to read the document the way
  // any other reader does.
  int estimates = 0;
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  do {
    const repro::ReplayInputFrameCost cost =
        shell.estimateReplayInputCostForTesting(repro::ReproFrame{});
    EXPECT_TRUE(cost.valid);
    ++estimates;
    std::this_thread::sleep_for(std::chrono::microseconds(200));
  } while (!renderer.waitUntilNoRenderInFlightForTesting(std::chrono::steady_clock::now()) &&
           std::chrono::steady_clock::now() < deadline);

  EXPECT_THAT(estimates, testing::Gt(0));
  EXPECT_TRUE(renderer.waitUntilNoRenderInFlightForTesting(std::chrono::steady_clock::now() +
                                                           std::chrono::seconds(2)));
  std::ignore = renderer.pollResult();
  renderer.setReplayRenderDelayForTesting(std::chrono::milliseconds(0));
}

TEST(EditorShellTest, FillStrokeToolbarStaysVisuallyStableDuringShapeDrag) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  std::optional<svg::SVGElement> target =
      EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  ASSERT_TRUE(EditorShellTestAccess::BeginSelectedShapeDrag(
      shell, Vector2d(20.0, 20.0), Box2d::FromXYWH(10.0, 12.0, 40.0, 24.0)));

  constexpr ImVec2 kCursor(20.0f, 40.0f);
  constexpr ImU32 kEnabledSwap = IM_COL32(215, 222, 232, 255);
  constexpr ImU32 kDisabledSwap = IM_COL32(120, 126, 134, 255);
  const auto expectStableDragChrome = [&]() {
    EXPECT_TRUE(DrawDataContainsColor(kDisabledSwap));
    EXPECT_FALSE(DrawDataContainsColor(kEnabledSwap));
    EXPECT_TRUE(DrawDataContainsColor(IM_COL32(255, 0, 0, 255))) << "Fill swatch changed";
    EXPECT_TRUE(DrawDataContainsColor(IM_COL32(0, 0, 255, 255))) << "Stroke swatch changed";
  };

  RenderToolbarFrame(window, shell, kCursor, ImVec2(-100.0f, -100.0f), /*mouseDown=*/false);
  expectStableDragChrome();
}

TEST(EditorShellTest, FillStrokeToolbarCapturesNewSelectionBeforeFreezingShapeDrag) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window,
                    OptionsWithSource(kPaintSnapshotSelectionChangeSvg, "paint_selection.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  svg::SVGDocument& document = EditorShellTestAccess::App(shell).document().document();
  std::optional<svg::SVGElement> first = document.querySelector("#first");
  std::optional<svg::SVGElement> second = document.querySelector("#second");
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());

  constexpr ImVec2 kCursor(20.0f, 40.0f);
  EditorShellTestAccess::App(shell).setSelection(*first);
  RenderToolbarFrame(window, shell, kCursor, ImVec2(-100.0f, -100.0f), /*mouseDown=*/false);
  ASSERT_TRUE(DrawDataContainsColor(IM_COL32(255, 0, 0, 255)));

  EditorShellTestAccess::App(shell).setSelection(*second);
  ASSERT_TRUE(EditorShellTestAccess::BeginSelectedShapeDrag(
      shell, Vector2d(75.0, 20.0), Box2d::FromXYWH(65.0, 12.0, 40.0, 24.0)));
  RenderToolbarFrame(window, shell, kCursor, ImVec2(-100.0f, -100.0f), /*mouseDown=*/false);

  EXPECT_TRUE(DrawDataContainsColor(IM_COL32(0, 0, 255, 255)))
      << "The drag must freeze the newly selected element's fill";
  EXPECT_FALSE(DrawDataContainsColor(IM_COL32(255, 0, 0, 255)))
      << "The drag must not replay the previous selection's fill";
}

TEST(EditorShellTest, ToolPaletteSelectCommitsOpenPenPath) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  DriveGlobalShortcut(shell, {ImGuiKey_P});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(5.0, 6.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(25.0, 16.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(25.0, 16.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  constexpr ImVec2 kPaneOrigin(20.0f, 30.0f);
  constexpr ImVec2 kContentRegion(400.0f, 260.0f);
  const Box2d palette =
      EditorShellTestAccess::ToolPaletteScreenRect(shell, kPaneOrigin, kContentRegion);
  const ImVec2 selectCenter(static_cast<float>(palette.topLeft.x + 20.0),
                            static_cast<float>(palette.topLeft.y + 20.0));

  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion,
                         ImVec2(selectCenter.x, selectCenter.y), /*mouseDown=*/false);
  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, selectCenter,
                         /*mouseDown=*/false);
  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, selectCenter,
                         /*mouseDown=*/true);
  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, selectCenter,
                         /*mouseDown=*/false);

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);
}

TEST(EditorShellTest, HighlightSelectionSourceHandlesTextOriginAndClearedSelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  EditorShellTestAccess::SetSourceSelectionOriginatedInText(shell, true);
  EditorShellTestAccess::App(shell).setSelection(*target);

  EXPECT_TRUE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));
  EXPECT_FALSE(EditorShellTestAccess::SourceSelectionOriginatedInText(shell));
  EXPECT_FALSE(EditorShellTestAccess::PreserveSourceEditFocusCursor(shell));
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).hasFocusPartition());

  EXPECT_FALSE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));

  EditorShellTestAccess::App(shell).clearSelection();
  EXPECT_TRUE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));
  EXPECT_FALSE(EditorShellTestAccess::Source(shell).hasFocusPartition());
}

TEST(EditorShellTest, HighlightSelectionSourceClearsStalePreserveFlag) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  ASSERT_TRUE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));

  EditorShellTestAccess::SetSourceFocusMode(shell, false);
  EditorShellTestAccess::SetPreserveSourceEditFocusCursor(shell, true);

  EXPECT_FALSE(EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell));
  EXPECT_FALSE(EditorShellTestAccess::PreserveSourceEditFocusCursor(shell));
}

TEST(EditorShellTest, ReferenceHighlightActiveAndChipRectUseSelectionBoundsCache) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  RunFramesUntilDisplayedSelectionBounds(window, shell);
  ASSERT_GT(EditorShellTestAccess::DisplayedSelectionBoundsCount(shell), 0u);
  EditorShellTestAccess::RefreshReferenceHighlightSummaryIfNeeded(shell);

  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightElements(shell).empty());
  EditorShellTestAccess::SetReferenceHighlightActive(shell, true);
  EXPECT_EQ(EditorShellTestAccess::ReferenceHighlightElements(shell).size(), 2u);
  EXPECT_EQ(EditorShellTestAccess::CombinedSourcePreviewElements(shell).size(), 2u);

  const std::optional<Box2d> chipRect =
      EditorShellTestAccess::ReferenceHighlightChipScreenRect(shell, "refs 2");
  ASSERT_TRUE(chipRect.has_value());
  EXPECT_GT(chipRect->width(), 0.0);
  EXPECT_GT(chipRect->height(), 0.0);
  EXPECT_GE(chipRect->topLeft.x, shell.viewportForReadback().imageScreenRect().topLeft.x);
  EXPECT_GE(chipRect->topLeft.y, shell.viewportForReadback().imageScreenRect().topLeft.y);
}

TEST(EditorShellTest, ReferenceHighlightChipRendersHoverAndClickStates) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kReferencedSvg, "referenced.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  RunFramesUntilDisplayedSelectionBounds(window, shell);
  ASSERT_GT(EditorShellTestAccess::DisplayedSelectionBoundsCount(shell), 0u);
  EditorShellTestAccess::RefreshReferenceHighlightSummaryIfNeeded(shell);

  const std::optional<Box2d> chipRect =
      EditorShellTestAccess::ReferenceHighlightChipScreenRect(shell, "-> 2");
  ASSERT_TRUE(chipRect.has_value());
  const ImVec2 chipCenter(
      static_cast<float>((chipRect->topLeft.x + chipRect->bottomRight.x) * 0.5),
      static_cast<float>((chipRect->topLeft.y + chipRect->bottomRight.y) * 0.5));

  RenderReferenceHighlightChipFrame(window, shell, chipCenter, /*mouseDown=*/false);
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightChipHovered(shell));
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightActive(shell));

  RenderReferenceHighlightChipFrame(window, shell, chipCenter, /*mouseDown=*/true);
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightActive(shell));
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightElements(shell).empty());

  RenderReferenceHighlightChipFrame(window, shell, ImVec2(-100.0f, -100.0f),
                                    /*mouseDown=*/false);
  EXPECT_FALSE(EditorShellTestAccess::ReferenceHighlightChipHovered(shell));
  EXPECT_TRUE(EditorShellTestAccess::ReferenceHighlightActive(shell));
}

namespace {

bool DrawDataContainsPositionChip(EditorShell& shell, const Box2d& documentBounds,
                                  const ViewportState& viewport) {
  const std::string label = internal::SelectionPositionChipLabel(documentBounds);
  const auto rect = EditorShellTestAccess::SelectionSizeChipScreenRect(
      shell, label, viewport.documentToScreen(documentBounds.topLeft));
  if (!rect.has_value() || ImGui::GetDrawData() == nullptr) {
    return false;
  }
  ImFont* font = EditorShellTestAccess::SelectionChipFont(shell);
  ImDrawList expected(ImGui::GetDrawListSharedData());
  expected._ResetForNewFrame();
  expected.PushClipRectFullScreen();
  expected.PushTextureID(font->ContainerAtlas->TexID);
  expected.AddText(font, std::max(13.0f, ImGui::GetFontSize() - 2.0f),
                   ImVec2(static_cast<float>(rect->topLeft.x) + 6.0f,
                          static_cast<float>(rect->topLeft.y) + 3.0f),
                   IM_COL32_WHITE, label.c_str());
  expected.PopTextureID();
  expected.PopClipRect();
  if (expected.VtxBuffer.empty()) {
    return false;
  }
  const auto sameGlyphVertex = [](const ImDrawVert& actual, const ImDrawVert& reference) {
    return actual.pos.x == reference.pos.x && actual.pos.y == reference.pos.y &&
           actual.uv.x == reference.uv.x && actual.uv.y == reference.uv.y;
  };
  const ImDrawData& actual = *ImGui::GetDrawData();
  for (int index = 0; index < actual.CmdListsCount; ++index) {
    const auto& vertices = actual.CmdLists[index]->VtxBuffer;
    if (std::search(vertices.begin(), vertices.end(), expected.VtxBuffer.begin(),
                    expected.VtxBuffer.end(), sameGlyphVertex) != vertices.end()) {
      return true;
    }
  }
  return false;
}

}  // namespace

TEST(EditorShellTest, TextMovePositionChipTracksPresentedBoundsWhileWorkerIsHeld) {
  gui::EditorWindow window(TestWindowOptions({
      .title = "Text move coordinate chip",
      .initialWidth = 960,
      .initialHeight = 720,
      .forceOffscreenRenderTarget = true,
      .enableFramebufferReadback = true,
  }));
  ASSERT_THAT(window.valid(), testing::Eq(true));
  EditorShell shell(window, OptionsWithSource(
                                R"(<svg xmlns="http://www.w3.org/2000/svg" width="480" height="320">
        <rect width="480" height="320" fill="white"/>
        <text id="target" x="90" y="130" font-size="30">Move this text</text>
      </svg>)"));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  app.setSelection(*target);
  RunFramesUntilDisplayedSelectionBounds(window, shell);
  auto frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
  ASSERT_NE(frame, nullptr);
  ASSERT_THAT(frame->selectionBounds(), testing::Not(testing::IsEmpty()));
  const Box2d originalBounds = CombinedSelectionBounds(frame->selectionBounds());
  const Vector2d start = (originalBounds.topLeft + originalBounds.bottomRight) * 0.5;
  const auto capture = [&](const Vector2d& point, bool pressed, std::string_view name) {
    shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
        .documentPoint = point, .leftMouseDown = true, .leftMousePressed = pressed});
    window.beginFrame();
    shell.runFrame();
    const svg::RendererBitmap bitmap = window.endFrameAndReadPixels();
    const char* directory = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
    if (directory != nullptr && !name.empty()) {
      const auto path = std::filesystem::path(directory) / name;
      EXPECT_THAT(svg::RendererImageIO::writeRgbaPixelsToPngFile(
                      path.string().c_str(), bitmap.pixels, bitmap.dimensions.x,
                      bitmap.dimensions.y, bitmap.rowBytes / 4u),
                  testing::Eq(true));
    }
  };
  EditorShellTestAccess::HoldRenderResultsForPolls(shell, 64);
  capture(start, true, {});
  bool sawRetainedRaster = false;
  for (const Vector2d delta : {Vector2d(20.0, 12.0), Vector2d(40.0, 24.0), Vector2d(60.0, 36.0)}) {
    capture(start + delta, false, "text-drag-held.png");
    frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
    ASSERT_NE(frame, nullptr);
    ASSERT_THAT(frame->selectionBounds(), testing::Not(testing::IsEmpty()));
    ASSERT_THAT(shell.layerInspectorStatusForReadback().activeDragPreview,
                testing::Optional(testing::_));
    sawRetainedRaster |= !frame->followsPointer();
    const Box2d bounds = CombinedSelectionBounds(frame->selectionBounds());
    EXPECT_THAT(DrawDataContainsPositionChip(shell, bounds, frame->viewport()), testing::Eq(true))
        << "The actual chip glyphs must show " << internal::SelectionPositionChipLabel(bounds)
        << " at the same bounds as the presented text; followsPointer=" << frame->followsPointer();
  }
  EXPECT_THAT(sawRetainedRaster, testing::Eq(true))
      << "The regression must exercise held text pixels, rather than only an affine tile preview";
  EXPECT_GT(shell.asyncRendererForReplay().replayResultHoldPollCountForTesting(), 0u)
      << "A real worker result must be held during the observed motion frames";
  shell.asyncRendererForReplay().releaseReplayResultHoldForTesting();
  bool advanced = false;
  for (const tests::HangGuard guard; guard.active();) {
    capture(start + Vector2d(60.0, 36.0), false, {});
    if (!shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(guard.deadline())) {
      break;
    }
    frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
    if (frame != nullptr && !frame->selectionBounds().empty()) {
      const Box2d bounds = CombinedSelectionBounds(frame->selectionBounds());
      if (bounds.topLeft.x > originalBounds.topLeft.x + 50.0) {
        capture(start + Vector2d(60.0, 36.0), false, "text-drag-accepted.png");
        frame = EditorShellTestAccess::Coordinator(shell).framePresentation();
        const Box2d acceptedBounds = CombinedSelectionBounds(frame->selectionBounds());
        EXPECT_THAT(DrawDataContainsPositionChip(shell, acceptedBounds, frame->viewport()),
                    testing::Eq(true));
        advanced = true;
        break;
      }
    }
  }
  EXPECT_THAT(advanced, testing::Eq(true))
      << "Accepted text pixels must advance the chip coordinates";
}

TEST(EditorShellTest, SelectionChipBoundsIgnoreUnpresentedGestureIntent) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  RunFramesUntilDisplayedSelectionBounds(window, shell);
  ASSERT_GT(EditorShellTestAccess::DisplayedSelectionBoundsCount(shell), 0u);

  const std::optional<Vector2d> baseAnchor =
      EditorShellTestAccess::SelectionChipAnchorScreen(shell);
  ASSERT_TRUE(baseAnchor.has_value());

  SelectTool::ActiveGesturePreview movePreview{
      .kind = SelectTool::ActiveGestureKind::Move,
      .startBoundsDoc = Box2d::FromXYWH(10.0, 12.0, 40.0, 24.0),
      .documentFromStartDocument = Transform2d::Translate(Vector2d(5.0, 7.0)),
  };
  const std::optional<Vector2d> moveAnchor =
      EditorShellTestAccess::SelectionChipAnchorScreen(shell, movePreview);
  const std::optional<Box2d> moveScreenBounds =
      EditorShellTestAccess::SelectionChipScreenBounds(shell, movePreview);
  ASSERT_TRUE(moveAnchor.has_value());
  ASSERT_TRUE(moveScreenBounds.has_value());
  EXPECT_THAT(*moveAnchor, testing::Eq(*baseAnchor));
  EXPECT_GT(moveScreenBounds->width(), 0.0);
  EXPECT_GT(moveScreenBounds->height(), 0.0);

  SelectTool::ActiveGesturePreview rotatePreview = movePreview;
  rotatePreview.kind = SelectTool::ActiveGestureKind::Rotate;
  rotatePreview.documentFromStartDocument = Transform2d::Rotate(0.25);
  const std::optional<Vector2d> rotateAnchor =
      EditorShellTestAccess::SelectionChipAnchorScreen(shell, rotatePreview);
  ASSERT_TRUE(rotateAnchor.has_value());
  EXPECT_THAT(*rotateAnchor, testing::Eq(*moveAnchor));

  const SelectionTransformHandleIntent resizeIntent{
      .kind = SelectionTransformHandleKind::Resize,
      .corner = SelectionTransformCorner::TopLeft,
  };
  RenderSelectionSizeChipFrame(window, shell, resizeIntent, std::nullopt);
  RenderSelectionSizeChipFrame(window, shell, SelectionTransformHandleIntent{}, movePreview);
  RenderSelectionSizeChipFrame(window, shell, SelectionTransformHandleIntent{}, rotatePreview);
  EXPECT_TRUE(shell.valid());

  EditorApp& app = EditorShellTestAccess::App(shell);
  app.setSelection(app.document().document().svgElement());
  EXPECT_THAT(EditorShellTestAccess::SelectionChipAnchorScreen(shell), testing::Eq(std::nullopt))
      << "A retained frame for another selection must not supply chip coordinates";
  app.clearSelection();
  EXPECT_THAT(EditorShellTestAccess::SelectionChipAnchorScreen(shell), testing::Eq(std::nullopt));
  app.setSelection(*target);
  EXPECT_THAT(EditorShellTestAccess::SelectionChipAnchorScreen(shell),
              testing::Optional(testing::_));
  ASSERT_THAT(app.loadFromString(kInitialSvg), testing::Eq(true));
  const auto replacement = app.document().document().querySelector("#target");
  ASSERT_THAT(replacement, testing::Optional(testing::_));
  app.setSelection(*replacement);
  EXPECT_THAT(EditorShellTestAccess::SelectionChipAnchorScreen(shell), testing::Eq(std::nullopt))
      << "Reused entity identifiers from a new document must not reuse the old frame's chip";
}

TEST(EditorShellTest, SourceHoverRangeGuardsRejectEmptyNoDocumentAndDirtySource) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kStyledSvg, "styled.svg"));
  ASSERT_TRUE(shell.valid());
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());

  EXPECT_TRUE(EditorShellTestAccess::SourceHoverRangesForElements(shell, {}).empty());

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  EXPECT_TRUE(EditorShellTestAccess::SourceHoverRangesForElements(invalidShell, {*target}).empty());

  EditorShellTestAccess::Source(shell).insertText(" ");
  EXPECT_TRUE(EditorShellTestAccess::SourceHoverRangesForElements(shell, {*target}).empty());
}

TEST(EditorShellTest, SourcePaneVisibilityNoOpsWhenAlreadyMatching) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  ASSERT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));
  EditorShellTestAccess::SetSourcePaneVisible(shell, false);
  EXPECT_FALSE(EditorShellTestAccess::SourcePaneVisible(shell));

  EditorShellTestAccess::SetSourcePaneVisible(shell, true);
  EXPECT_TRUE(EditorShellTestAccess::SourcePaneVisible(shell));
  EditorShellTestAccess::SetSourcePaneVisible(shell, true);
  EXPECT_TRUE(EditorShellTestAccess::SourcePaneVisible(shell));
}

TEST(EditorShellTest, SelectAllCanvasAndTextSelectionPreconditions) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  EXPECT_TRUE(EditorShellTestAccess::CanvasHasSelectableElements(shell));
  EditorShellTestAccess::SelectAllCanvasElements(shell);
  EXPECT_GT(EditorShellTestAccess::App(shell).selectedElements().size(), 1u);
  EXPECT_FALSE(EditorShellTestAccess::SelectionIsAllText(shell));

  auto label = EditorShellTestAccess::App(shell).document().document().querySelector("#label");
  ASSERT_TRUE(label.has_value());
  EditorShellTestAccess::App(shell).setSelection(*label);
  EXPECT_TRUE(EditorShellTestAccess::SelectionIsAllText(shell));

  EditorShellTestAccess::App(shell).clearSelection();
  EXPECT_FALSE(EditorShellTestAccess::SelectionIsAllText(shell));

  EditorShell invalidShell(window, OptionsWithSource("<svg><rect></svg>", "broken.svg"));
  ASSERT_TRUE(invalidShell.valid());
  EXPECT_FALSE(EditorShellTestAccess::CanvasHasSelectableElements(invalidShell));
  EditorShellTestAccess::SelectAllCanvasElements(invalidShell);
  EXPECT_TRUE(EditorShellTestAccess::App(invalidShell).selectedElements().empty());
}

TEST(EditorShellTest, InspectorTextInputKeepsSelectAllFromChangingCanvasSelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  app.setSelection(*target);

  DriveGlobalShortcut(shell, {ImGuiKey_A}, /*ctrl=*/false, /*shift=*/false, /*super=*/true,
                      /*textInputActive=*/true);

  ASSERT_THAT(app.selectedElements(), testing::SizeIs(1));
  EXPECT_THAT(app.selectedElement()->id(), testing::Eq("target"));
}

TEST(EditorShellTest, ConvertSelectedTextToOutlinesGuardsNonTextSelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  const std::string sourceBefore(EditorShellTestAccess::App(shell).document().document().source());

  EditorShellTestAccess::ConvertSelectedTextToOutlines(shell);
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);

  EXPECT_EQ(EditorShellTestAccess::App(shell).document().document().source(), sourceBefore);
  EXPECT_TRUE(EditorShellTestAccess::LastConvertTextError(shell).empty());
  EXPECT_TRUE(EditorShellTestAccess::App(shell).document().document().querySelector("#target"));
}

TEST(EditorShellTest, ConvertSelectedTextToOutlinesReplacesTextWithPathGroup) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  auto label = EditorShellTestAccess::App(shell).document().document().querySelector("#label");
  ASSERT_TRUE(label.has_value());
  EditorShellTestAccess::App(shell).setSelection(*label);

  EditorShellTestAccess::ConvertSelectedTextToOutlines(shell);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));

  svg::SVGDocument& document = EditorShellTestAccess::App(shell).document().document();
  EXPECT_FALSE(document.querySelector("#label").has_value());
  ASSERT_TRUE(document.querySelector("#label_outlines").has_value());
  EXPECT_TRUE(document.querySelector("#label_outlines_0").has_value());
  EXPECT_NE(document.source().find(R"(id="label_outlines")"), std::string_view::npos);
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElements().size(), 1u);
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElements().front().id(), "label_outlines");
  EXPECT_TRUE(EditorShellTestAccess::LastConvertTextError(shell).empty());
}

TEST(EditorShellTest, ConvertSelectedTextToOutlinesCapacityRefusalPreservesText) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  auto& app = EditorShellTestAccess::App(shell);
  svg::SVGDocument& document = app.document().document();
  const auto label = document.querySelector("#label");
  ASSERT_THAT(label, testing::Ne(std::nullopt));
  app.setSelection(*label);
  const std::string sourceBefore(document.source());
  document.xmlDocument().setSourceEditTreeLimits(0, 128);

  EditorShellTestAccess::ConvertSelectedTextToOutlines(shell);
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);

  EXPECT_THAT(document.source(), testing::Eq(sourceBefore));
  EXPECT_THAT(document.querySelector("#label"), testing::Optional(testing::Eq(*label)));
  EXPECT_THAT(document.querySelector("#label_outlines"), testing::Eq(std::nullopt));
  EXPECT_THAT(app.selectedElements(), testing::ElementsAre(*label));
  EXPECT_THAT(app.canUndo(), testing::Eq(false));
  EXPECT_THAT(app.isDirty(), testing::Eq(false));
  EXPECT_THAT(app.document().lastParseError(), testing::Eq(std::nullopt));
}

TEST(EditorShellTest, ConvertMultipleTextElementsToOutlinesUsesOneUndoEntry) {
  constexpr std::string_view source = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="180" height="90">
  <text id="first" x="10" y="30">First</text>
  <text id="second" x="10" y="65">Second</text>
</svg>)svg";
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(source, "multiple-text.svg"));
  ASSERT_EQ(shell.valid(), true);
  auto& app = EditorShellTestAccess::App(shell);
  auto& document = app.document().document();
  svg::Renderer renderer;
  renderer.draw(document);
  app.setSelection({*document.querySelector("#first"), *document.querySelector("#second")});
  const std::string sourceBefore(document.source());

  EditorShellTestAccess::ConvertSelectedTextToOutlines(shell);
  ASSERT_THAT(EditorShellTestAccess::LastConvertTextError(shell), testing::IsEmpty());
  ASSERT_EQ(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell), true);
  EXPECT_EQ(document.querySelector("text").has_value(), false);
  EXPECT_EQ(document.querySelector("#first_outlines").has_value(), true);
  EXPECT_EQ(document.querySelector("#second_outlines").has_value(), true);
  EXPECT_THAT(app.selectedElements(), testing::SizeIs(2));
  ASSERT_EQ(app.canUndo(), true);

  app.undo();
  ASSERT_EQ(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell), true);
  EXPECT_EQ(app.document().document().source(), sourceBefore);
  EXPECT_EQ(app.canUndo(), false);
}

TEST(EditorShellTest, ConvertMultipleTextOutlinesCapacityRefusalRollsBackSelection) {
  constexpr std::string_view source =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg"><text id="first">ABC</text><text id="second">ABC</text></svg>)svg";
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(source, "multiple-text.svg"));
  ASSERT_TRUE(shell.valid());
  auto& app = EditorShellTestAccess::App(shell);
  svg::SVGDocument& document = app.document().document();
  svg::Renderer renderer;
  renderer.draw(document);
  const auto first = document.querySelector("#first");
  const auto second = document.querySelector("#second");
  ASSERT_THAT(first, testing::Ne(std::nullopt));
  ASSERT_THAT(second, testing::Ne(std::nullopt));
  app.setSelection({*first, *second});
  const std::string sourceBefore(document.source());
  document.xmlDocument().setSourceEditTreeLimits(10, 128);

  EditorShellTestAccess::ConvertSelectedTextToOutlines(shell);
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);

  EXPECT_THAT(document.source(), testing::Eq(sourceBefore));
  EXPECT_THAT(document.querySelector("#first"), testing::Optional(testing::Eq(*first)));
  EXPECT_THAT(document.querySelector("#second"), testing::Optional(testing::Eq(*second)));
  EXPECT_THAT(document.querySelector("#first_outlines"), testing::Eq(std::nullopt));
  EXPECT_THAT(document.querySelector("#second_outlines"), testing::Eq(std::nullopt));
  EXPECT_THAT(app.selectedElements(), testing::ElementsAre(*first, *second));
  EXPECT_THAT(app.canUndo(), testing::Eq(false));
  EXPECT_THAT(app.isDirty(), testing::Eq(false));
}

TEST(EditorShellTest, RenderPaneDeferredEmptyClickStartsMarqueeAfterHold) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(400.0, 300.0));
  EditorShellTestAccess::SetPendingSelectClickStartSeconds(shell, -1.0);

  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2(640.0f, 480.0f);
  io.AddMousePosEvent(-50.0f, -50.0f);
  io.AddMouseButtonEvent(0, true);

  window.beginFrame();
  EditorShellTestAccess::RenderRenderPane(
      shell, Vector2d(0.0, 0.0), Vector2d(320.0, 220.0),
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
  window.endFrame();

  EXPECT_FALSE(EditorShellTestAccess::HasPendingClick(shell));
  EXPECT_TRUE(EditorShellTestAccess::SelectToolIsMarqueeing(shell));
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  io.AddMouseButtonEvent(0, false);
  window.beginFrame();
  window.endFrame();
}

// With the root `<svg>` selected, the buffered canvas click must not take the selected-bounds
// re-drag: the root's bounds cover the whole canvas, which kept every click on the root.
TEST(EditorShellTest, RenderPaneClickSelectsChildWhileRootIsSelected) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  EditorApp& app = EditorShellTestAccess::App(shell);
  app.setSelection(app.document().document().svgElement());

  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(30.0, 24.0));
  RenderPaneMouseFrame(window, shell, Vector2d(30.0, 24.0), /*mouseDown=*/false);

  EXPECT_THAT(app.selectedElement(),
              testing::Optional(
                  testing::Property("id", &svg::SVGElement::id, testing::Eq(RcString("target")))));
}

TEST(EditorShellTest, RenderPaneTextToolPlainClickCreatesNothing) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  const std::string sourceBefore(EditorShellTestAccess::App(shell).document().document().source());

  DriveGlobalShortcut(shell, {ImGuiKey_T});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));

  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(24.0, 34.0));
  RenderPaneMouseFrame(window, shell, Vector2d(24.0, 34.0), /*mouseDown=*/false);
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  EXPECT_EQ(EditorShellTestAccess::App(shell).document().document().source(), sourceBefore);
}

TEST(EditorShellTest, RenderPaneTextToolDoubleClickCreatesTextSession) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  DriveGlobalShortcut(shell, {ImGuiKey_T});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));

  MouseModifiers doubleClick;
  doubleClick.doubleClick = true;
  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(24.0, 34.0), doubleClick);
  RenderPaneMouseFrame(window, shell, Vector2d(24.0, 34.0), /*mouseDown=*/false,
                       /*renderPaneOrigin=*/Vector2d(20.0, 30.0),
                       /*renderPaneSize=*/Vector2d(400.0, 260.0), /*shift=*/false,
                       /*option=*/false, /*command=*/false, /*doubleClick=*/true);
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);

  ASSERT_TRUE(EditorShellTestAccess::App(shell).selectedElement().has_value());
  const std::string_view source = EditorShellTestAccess::App(shell).document().document().source();
  EXPECT_NE(source.find(R"(x="24")"), std::string_view::npos);
  EXPECT_NE(source.find(R"(y="34")"), std::string_view::npos);
  EXPECT_NE(source.find(R"(<text)"), std::string_view::npos);
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));
}

TEST(EditorShellTest, RenderPanePenToolClickDragAndCommitOpenPath) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  DriveGlobalShortcut(shell, {ImGuiKey_P});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(12.0, 18.0));
  RenderPaneMouseFrame(window, shell, Vector2d(18.0, 24.0), /*mouseDown=*/false);
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolActivePathData(shell).empty());

  MouseModifiers constrainedClick;
  constrainedClick.shift = true;
  constrainedClick.pixelsPerDocUnit = shell.viewportForReadback().pixelsPerDocUnit();
  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(42.0, 24.0), constrainedClick);
  RenderPaneMouseFrame(window, shell, Vector2d(42.0, 24.0), /*mouseDown=*/false);
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  DriveGlobalShortcut(shell, {ImGuiKey_Enter});
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);
}

TEST(EditorShellTest, RenderPanePenToolDragsAnchorAndCommitsOnDoubleClick) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  DriveGlobalShortcut(shell, {ImGuiKey_P});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(12.0, 18.0));
  RenderPaneMouseFrame(window, shell, Vector2d(12.0, 18.0), /*mouseDown=*/true);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));

  RenderPaneMouseFrame(window, shell, Vector2d(22.0, 28.0), /*mouseDown=*/true);
  EXPECT_TRUE(EditorShellTestAccess::PenDragFlushedThisFrame(shell));
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));

  RenderPaneMouseFrame(window, shell, Vector2d(22.0, 28.0), /*mouseDown=*/false);
  EXPECT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDraggingAnchor(shell));

  EditorShellTestAccess::BufferPendingClick(shell, Vector2d(42.0, 24.0));
  RenderPaneMouseFrame(window, shell, Vector2d(42.0, 24.0), /*mouseDown=*/false);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  RenderPaneMouseFrame(window, shell, Vector2d(42.0, 24.0), /*mouseDown=*/true,
                       Vector2d(20.0, 30.0), Vector2d(400.0, 260.0),
                       /*shift=*/false, /*option=*/false, /*command=*/false,
                       /*doubleClick=*/true);
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("<path"),
            std::string_view::npos);
}

TEST(EditorShellTest, GlobalShortcutDeleteWhilePenDraftingRemovesAnchorBeforeCanvasDelete) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());

  DriveGlobalShortcut(shell, {ImGuiKey_P});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));

  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 18.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(12.0, 18.0),
      .leftMouseReleased = true,
  });
  EditorShellTestAccess::ApplyPendingDocumentSpaceReplayInput(shell);
  ASSERT_TRUE(EditorShellTestAccess::PenToolIsDrafting(shell));

  DriveGlobalShortcut(shell, {ImGuiKey_Delete});

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));
  EXPECT_FALSE(EditorShellTestAccess::PenToolIsDrafting(shell));
  EXPECT_TRUE(EditorShellTestAccess::App(shell).hasDocument());
}

TEST(EditorShellTest, SourceFontShortcutsResizeTextAndPreserveCanvasZoomAndSelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  ImGui::GetIO().ConfigInputTrickleEventQueue = false;
  EditorShellTestAccess::SetSourcePaneVisible(shell, true);
  EditorShellTestAccess::SetSourceFocusMode(shell, false);
  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.setWordWrapEnabled(false);
  source.selectAndFocus({1, 0}, {1, 4});
  const auto drawSource = [&] {
    if (ImGuiWindow* sourceWindow = FindSourceEditorWindow()) {
      ImGui::FocusWindow(sourceWindow);
    }
    EditorShellTestAccess::RenderSourcePane(shell, 0.0f, 400.0f, 500.0f, window.editorFonts().code);
  };
  const auto renderSource = [&] {
    window.beginFrame();
    drawSource();
    window.endFrame();
    return SourceEditorContentHeight();
  };
  renderSource();
  ASSERT_NE(FindSourceEditorWindow(), nullptr);
  ImGui::FocusWindow(FindSourceEditorWindow());
  const float originalHeight = renderSource();
  ASSERT_GT(originalHeight, 0.0f);
  ASSERT_TRUE(source.isFocused());
  const std::string selectedText = source.getSelectedText();
  ASSERT_FALSE(selectedText.empty());
  const double canvasZoom = shell.viewportForReadback().zoom;

  DriveGlobalShortcut(shell, {ImGuiKey_Equal}, false, true, true, false, drawSource);
  const float enlargedHeight = renderSource();
  EXPECT_GT(enlargedHeight, originalHeight);
  EXPECT_DOUBLE_EQ(shell.viewportForReadback().zoom, canvasZoom);
  EXPECT_EQ(source.getSelectedText(), selectedText);

  DriveGlobalShortcut(shell, {ImGuiKey_Minus}, false, false, true, false, drawSource);
  EXPECT_NEAR(renderSource(), originalHeight, 0.01f);
  DriveGlobalShortcut(shell, {ImGuiKey_KeypadSubtract}, true, false, false, false, drawSource);
  EXPECT_LT(renderSource(), originalHeight);
  DriveGlobalShortcut(shell, {ImGuiKey_0}, false, false, true, false, drawSource);
  EXPECT_NEAR(renderSource(), originalHeight, 0.01f);
  DriveGlobalShortcut(shell, {ImGuiKey_KeypadAdd}, true, false, false, false, drawSource);
  EXPECT_GT(renderSource(), originalHeight);
  DriveGlobalShortcut(shell, {ImGuiKey_0}, true, false, false, false, drawSource);
  EXPECT_NEAR(renderSource(), originalHeight, 0.01f);
  EXPECT_DOUBLE_EQ(shell.viewportForReadback().zoom, canvasZoom);
  EXPECT_EQ(source.getSelectedText(), selectedText);
}

TEST(EditorShellTest, SourceFontMenuActionsResizeTextClampAndReset) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::SetSourcePaneVisible(shell, true);
  EditorShellTestAccess::SetSourceFocusMode(shell, false);
  TextEditor& source = EditorShellTestAccess::Source(shell);
  source.setWordWrapEnabled(false);
  const auto renderSource = [&] {
    window.beginFrame();
    EditorShellTestAccess::RenderSourcePane(shell, 0.0f, 400.0f, 500.0f, window.editorFonts().code);
    window.endFrame();
    return SourceEditorContentHeight();
  };
  const float originalHeight = renderSource();
  const double canvasZoom = shell.viewportForReadback().zoom;
  MenuBarActions actions;
  actions.increaseSourceFontSize = true;
  for (int i = 0; i < 30; ++i) {
    EditorShellTestAccess::ApplyMenuActions(shell, actions);
  }
  EXPECT_NEAR(renderSource(), originalHeight * 3.0f, 0.01f);
  actions = {};
  actions.decreaseSourceFontSize = true;
  for (int i = 0; i < 30; ++i) {
    EditorShellTestAccess::ApplyMenuActions(shell, actions);
  }
  EXPECT_NEAR(renderSource(), originalHeight * 0.5f, 0.01f);
  actions = {};
  actions.resetSourceFontSize = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_NEAR(renderSource(), originalHeight, 0.01f);
  EXPECT_DOUBLE_EQ(shell.viewportForReadback().zoom, canvasZoom);
  EXPECT_FLOAT_EQ(window.editorFonts().code->Scale, 1.0f);
}

MATCHER_P(LiesWithin, outer, "lies within " + testing::PrintToString(outer)) {
  constexpr double kTolerance = 1e-6;
  return arg.topLeft.x >= outer.topLeft.x - kTolerance &&
         arg.topLeft.y >= outer.topLeft.y - kTolerance &&
         arg.bottomRight.x <= outer.bottomRight.x + kTolerance &&
         arg.bottomRight.y <= outer.bottomRight.y + kTolerance;
}

/// Screen rectangle of the render pane.
Box2d PaneScreenRect(const ViewportState& viewport) {
  return Box2d(viewport.paneOrigin, viewport.paneOrigin + viewport.paneSize);
}

/// Open \p source in a 1280x700 editor, a typical laptop window, and run frames
/// until the shell latches the document's initial view.
/// @return False when the initial view never latched.
bool OpenAndLatchInitialView(gui::EditorWindow& window, EditorShell& shell) {
  for (const tests::HangGuard guard;
       !EditorShellTestAccess::ViewportInitialized(shell) && guard.active();) {
    RunShellFrame(window, shell);
  }
  return EditorShellTestAccess::ViewportInitialized(shell);
}

gui::EditorWindow MakeQaSizedWindow() {
  return gui::EditorWindow(
      TestWindowOptions({.title = "Zoom to fit", .initialWidth = 1280, .initialHeight = 700}, 1.0));
}

constexpr std::string_view kSmallArtboardSvg = R"svg(
  <svg xmlns="http://www.w3.org/2000/svg" width="200" height="100">
    <rect width="200" height="100" fill="#2f6fed"/>
  </svg>)svg";

// The Geode Splash sample (1536x1024) is larger than the canvas of a typical window. It must
// open with the whole artboard visible and clear of the floating tool palette, not cropped at 100%.
TEST(EditorShellTest, DocumentLargerThanTheCanvasOpensFittedToIt) {
  gui::EditorWindow window = MakeQaSizedWindow();
  ASSERT_TRUE(window.valid());
  const EditorSample* splash = FindEditorSample("geode-splash");
  ASSERT_NE(splash, nullptr);
  EditorShell shell(window, OptionsWithSource(splash->source, "geode_splash.svg"));
  ASSERT_TRUE(OpenAndLatchInitialView(window, shell));

  const ViewportState& viewport = shell.viewportForReadback();
  const Box2d artboard = viewport.imageScreenRect();
  const Box2d pane = PaneScreenRect(viewport);
  EXPECT_THAT(viewport.zoom, testing::Lt(1.0));
  EXPECT_THAT(artboard, LiesWithin(pane)) << "the artboard opened cropped";
  EXPECT_THAT(artboard.topLeft.y,
              testing::Ge(EditorShellTestAccess::ToolPaletteRect(shell).bottomRight.y))
      << "the tool palette covers the top of the fitted artboard";
  EXPECT_THAT(std::max(artboard.width() / pane.width(), artboard.height() / pane.height()),
              testing::Gt(0.75))
      << "a fitted artboard fills the canvas on its limiting axis";
}

/// Press Cmd (Ctrl) plus \p key through real shell frames, as the keyboard delivers it.
void PressCommandShortcutInFrames(gui::EditorWindow& window, EditorShell& shell, ImGuiKey key) {
  ImGuiIO& io = ImGui::GetIO();
  io.AddKeyEvent(ImGuiMod_Ctrl, true);
  io.AddKeyEvent(key, true);
  RunShellFrame(window, shell);
  io.AddKeyEvent(key, false);
  io.AddKeyEvent(ImGuiMod_Ctrl, false);
  RunShellFrame(window, shell);
}

// A large document opens in exactly the view Zoom to Fit produces, so pressing Cmd+0 after opening
// it, or after Cmd+1, returns to that view.
TEST(EditorShellTest, ZoomToFitMatchesTheViewALargeDocumentOpensIn) {
  gui::EditorWindow window = MakeQaSizedWindow();
  ASSERT_TRUE(window.valid());
  const EditorSample* splash = FindEditorSample("geode-splash");
  ASSERT_NE(splash, nullptr);
  EditorShell shell(window, OptionsWithSource(splash->source, "geode_splash.svg"));
  ASSERT_TRUE(OpenAndLatchInitialView(window, shell));
  RunShellFrame(window, shell);
  const ViewportState opened = shell.viewportForReadback();
  ASSERT_THAT(opened.zoom, testing::Lt(1.0));

  PressCommandShortcutInFrames(window, shell, ImGuiKey_1);
  ASSERT_DOUBLE_EQ(shell.viewportForReadback().zoom, 1.0);
  PressCommandShortcutInFrames(window, shell, ImGuiKey_0);
  const ViewportState fitted = shell.viewportForReadback();
  EXPECT_DOUBLE_EQ(fitted.zoom, opened.zoom);
  EXPECT_EQ(fitted.imageScreenRect(), opened.imageScreenRect());
}

TEST(EditorShellTest, DocumentThatFitsOpensAtActualSize) {
  gui::EditorWindow window = MakeQaSizedWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(kSmallArtboardSvg));
  ASSERT_TRUE(OpenAndLatchInitialView(window, shell));
  EXPECT_DOUBLE_EQ(shell.viewportForReadback().zoom, 1.0);
}

// Cmd+0 fits the artboard to the canvas, and Cmd+1 returns to 100%, as in Illustrator.
TEST(EditorShellTest, ZoomToFitAndActualSizeShortcuts) {
  gui::EditorWindow window = MakeQaSizedWindow();
  ASSERT_TRUE(window.valid());
  EditorShell shell(window, OptionsWithSource(kSmallArtboardSvg));
  ASSERT_TRUE(OpenAndLatchInitialView(window, shell));
  const Box2d pane = PaneScreenRect(shell.viewportForReadback());

  DriveGlobalShortcut(shell, {ImGuiKey_0}, /*ctrl=*/true);
  RunShellFrame(window, shell);
  const ViewportState fitted = shell.viewportForReadback();
  EXPECT_THAT(fitted.zoom, testing::Gt(2.0)) << "Cmd+0 must enlarge a small artboard to fit";
  EXPECT_THAT(fitted.imageScreenRect(), LiesWithin(pane));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_1}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));
  EXPECT_DOUBLE_EQ(shell.viewportForReadback().zoom, 1.0);
}

TEST(EditorShellTest, CanvasZoomControlClickPreservesLegacyReplayBehavior) {
  for (const bool legacy : {true, false}) {
    SCOPED_TRACE(legacy ? "legacy recording" : "current editor");
    gui::EditorWindow window = MakeQaSizedWindow();
    ASSERT_TRUE(window.valid());
    EditorShellOptions options = OptionsWithSource(kSmallArtboardSvg);
    options.useLegacyCanvasZoomControls = legacy;
    EditorShell shell(window, options);
    ASSERT_TRUE(OpenAndLatchInitialView(window, shell));
    PressCommandShortcutInFrames(window, shell, ImGuiKey_Equal);
    const double zoomed = shell.viewportForReadback().zoom;
    ASSERT_THAT(zoomed, testing::Gt(1.0));
    const Box2d control = EditorShellTestAccess::CanvasZoomControlRect(shell);
    const Vector2d center = (control.topLeft + control.bottomRight) * 0.5;
    ImGuiIO& io = ImGui::GetIO();
    io.AddMousePosEvent(static_cast<float>(center.x), static_cast<float>(center.y));
    RunShellFrame(window, shell);
    io.AddMouseButtonEvent(0, true);
    RunShellFrame(window, shell);
    io.AddMouseButtonEvent(0, false);
    RunShellFrame(window, shell);

    EXPECT_DOUBLE_EQ(shell.viewportForReadback().zoom, legacy ? 1.0 : zoomed);
    EXPECT_EQ(!ImGui::GetCurrentContext()->OpenPopupStack.empty(), !legacy);
  }
}

TEST(EditorShellTest, GlobalShortcutsSwitchToolsZoomAndSourceFocus) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));

  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_P});
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_T});
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_Escape});
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_P});
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_V});
  EXPECT_TRUE(EditorShellTestAccess::ActiveToolIsSelect(shell));

  const bool sourceFocusBefore = EditorShellTestAccess::SourceFocusMode(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_Enter}, /*ctrl=*/true);
  EXPECT_NE(EditorShellTestAccess::SourceFocusMode(shell), sourceFocusBefore);

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_Equal}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_Minus}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  DriveGlobalShortcut(shell, {ImGuiKey_Equal}, /*ctrl=*/true);
  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_0}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_KeypadAdd}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_KeypadSubtract}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  const bool sourceFocusBeforeKeypadEnter = EditorShellTestAccess::SourceFocusMode(shell);
  DriveGlobalShortcut(shell, {ImGuiKey_KeypadEnter}, /*ctrl=*/true);
  EXPECT_NE(EditorShellTestAccess::SourceFocusMode(shell), sourceFocusBeforeKeypadEnter);
}

TEST(EditorShellTest, EscapeExitsGroupEditEvenWhenSelectionIsActive) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  constexpr std::string_view kGroupSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
        <g id="scope"><rect id="leaf" x="10" y="10" width="40" height="40"/></g>
      </svg>)svg";
  EditorShell shell(window, OptionsWithSource(kGroupSvg, "group.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const svg::SVGElement scope = *app.document().document().querySelector("#scope");
  const svg::SVGElement leaf = *app.document().document().querySelector("#leaf");
  ASSERT_TRUE(app.enterGroupEdit(scope));
  app.setSelection(leaf);
  ASSERT_TRUE(app.hasSelection());

  DriveGlobalShortcut(shell, {ImGuiKey_Escape});

  EXPECT_FALSE(app.editingScope().has_value());
  EXPECT_EQ(app.selectedElement(), std::optional<svg::SVGElement>(scope));
}

TEST(EditorShellTest, EditingScopeBreadcrumbMouseClicksExitOneNestedLevelAtATime) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  constexpr std::string_view kNestedSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
        <g id="outer"><g id="inner"><rect id="leaf" width="40" height="40"/></g></g>
      </svg>)svg";
  EditorShell shell(window, OptionsWithSource(kNestedSvg, "nested.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto outer = app.document().document().querySelector("#outer");
  const auto inner = app.document().document().querySelector("#inner");
  const auto leaf = app.document().document().querySelector("#leaf");
  ASSERT_TRUE(outer.has_value());
  ASSERT_TRUE(inner.has_value());
  ASSERT_TRUE(leaf.has_value());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 120.0));
  const std::string originalSource = EditorShellTestAccess::Source(shell).getText();
  const auto generation = app.document().documentGeneration();
  EXPECT_FALSE(RenderEditingScopeBreadcrumbFrame(window, shell, "unused").has_value());
  ASSERT_TRUE(app.enterGroupEdit(*outer));
  ASSERT_TRUE(app.enterGroupEdit(*inner));
  app.setSelection(*leaf);

  const auto innerItem =
      RenderEditingScopeBreadcrumbFrame(window, shell, "<  inner##exit_group_edit");
  ASSERT_TRUE(innerItem.has_value());
  EXPECT_EQ(innerItem->id, innerItem->expectedLabelId);
  const ImVec2 innerCenter = innerItem->rect.GetCenter();
  const auto hovered =
      RenderEditingScopeBreadcrumbFrame(window, shell, "<  inner##exit_group_edit", innerCenter);
  ASSERT_TRUE(hovered.has_value());
  EXPECT_TRUE(hovered->hovered);
  RenderEditingScopeBreadcrumbFrame(window, shell, "<  inner##exit_group_edit", innerCenter,
                                    /*mouseDown=*/true);
  EXPECT_THAT(app.editingScope(), testing::Optional(*inner));
  RenderEditingScopeBreadcrumbFrame(window, shell, "<  inner##exit_group_edit", innerCenter);
  EXPECT_THAT(app.editingScope(), testing::Optional(*outer));
  EXPECT_THAT(app.selectedElement(), testing::Optional(*inner));
  EXPECT_EQ(EditorShellTestAccess::Source(shell).getText(), originalSource);

  const auto outerItem =
      RenderEditingScopeBreadcrumbFrame(window, shell, "<  outer##exit_group_edit");
  ASSERT_TRUE(outerItem.has_value());
  EXPECT_EQ(outerItem->id, outerItem->expectedLabelId);
  const ImVec2 outerCenter = outerItem->rect.GetCenter();
  RenderEditingScopeBreadcrumbFrame(window, shell, "<  outer##exit_group_edit", outerCenter);
  RenderEditingScopeBreadcrumbFrame(window, shell, "<  outer##exit_group_edit", outerCenter,
                                    /*mouseDown=*/true);
  RenderEditingScopeBreadcrumbFrame(window, shell, "<  outer##exit_group_edit", outerCenter);
  EXPECT_FALSE(app.editingScope().has_value());
  EXPECT_THAT(app.selectedElement(), testing::Optional(*outer));
  EXPECT_FALSE(RenderEditingScopeBreadcrumbFrame(window, shell, "unused").has_value());
  EXPECT_EQ(EditorShellTestAccess::Source(shell).getText(), originalSource);
  EXPECT_EQ(app.document().document().source(), originalSource);
  EXPECT_EQ(app.document().documentGeneration(), generation);
  EXPECT_THAT(inner->parentElement(), testing::Optional(*outer));
  EXPECT_THAT(leaf->parentElement(), testing::Optional(*inner));
}

TEST(EditorShellTest, EditingScopeBreadcrumbAnonymousGroupUsesClickableFallbackLabel) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  constexpr std::string_view kAnonymousSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
        <g><rect id="leaf" width="40" height="40"/></g>
      </svg>)svg";
  EditorShell shell(window, OptionsWithSource(kAnonymousSvg, "anonymous.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto leaf = app.document().document().querySelector("#leaf");
  ASSERT_TRUE(leaf.has_value());
  const auto group = leaf->parentElement();
  ASSERT_TRUE(group.has_value());
  ASSERT_TRUE(group->id().empty());
  ASSERT_TRUE(app.enterGroupEdit(*group));
  app.setSelection(*leaf);
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 120.0));
  const std::string originalSource = EditorShellTestAccess::Source(shell).getText();

  const auto item = RenderEditingScopeBreadcrumbFrame(window, shell, "<  Group##exit_group_edit");
  ASSERT_TRUE(item.has_value());
  EXPECT_EQ(item->id, item->expectedLabelId);
  const ImVec2 center = item->rect.GetCenter();
  RenderEditingScopeBreadcrumbFrame(window, shell, "<  Group##exit_group_edit", center);
  RenderEditingScopeBreadcrumbFrame(window, shell, "<  Group##exit_group_edit", center,
                                    /*mouseDown=*/true);
  RenderEditingScopeBreadcrumbFrame(window, shell, "<  Group##exit_group_edit", center);
  EXPECT_FALSE(app.editingScope().has_value());
  EXPECT_THAT(app.selectedElement(), testing::Optional(*group));
  EXPECT_EQ(EditorShellTestAccess::Source(shell).getText(), originalSource);
  EXPECT_EQ(app.document().document().source(), originalSource);
  EXPECT_THAT(leaf->parentElement(), testing::Optional(*group));
}

TEST(EditorShellTest, EditingScopeBreadcrumbOutsideClickPreservesScopeAndSelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  constexpr std::string_view kGroupSvg =
      R"svg(<svg xmlns="http://www.w3.org/2000/svg" width="120" height="120">
        <g id="scope"><rect id="leaf" width="40" height="40"/></g>
      </svg>)svg";
  EditorShell shell(window, OptionsWithSource(kGroupSvg, "outside.svg"));
  ASSERT_TRUE(shell.valid());
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto scope = app.document().document().querySelector("#scope");
  const auto leaf = app.document().document().querySelector("#leaf");
  ASSERT_TRUE(scope.has_value());
  ASSERT_TRUE(leaf.has_value());
  ASSERT_TRUE(app.enterGroupEdit(*scope));
  app.setSelection(*leaf);
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 120.0));
  const std::string originalSource = EditorShellTestAccess::Source(shell).getText();

  const auto item = RenderEditingScopeBreadcrumbFrame(window, shell, "<  scope##exit_group_edit");
  ASSERT_TRUE(item.has_value());
  const ImVec2 outside = item->rect.Max + ImVec2(20.0f, 20.0f);
  const auto hovered =
      RenderEditingScopeBreadcrumbFrame(window, shell, "<  scope##exit_group_edit", outside);
  ASSERT_TRUE(hovered.has_value());
  EXPECT_FALSE(hovered->hovered);
  RenderEditingScopeBreadcrumbFrame(window, shell, "<  scope##exit_group_edit", outside,
                                    /*mouseDown=*/true);
  RenderEditingScopeBreadcrumbFrame(window, shell, "<  scope##exit_group_edit", outside);
  EXPECT_THAT(app.editingScope(), testing::Optional(*scope));
  EXPECT_THAT(app.selectedElement(), testing::Optional(*leaf));
  EXPECT_EQ(EditorShellTestAccess::Source(shell).getText(), originalSource);
  EXPECT_EQ(app.document().document().source(), originalSource);
}

TEST(EditorShellTest, GlobalShortcutsRouteFileDialogsSaveAndQuit) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  const std::filesystem::path savePath = TempPathForTest("shortcut_save.svg");
  std::filesystem::remove(savePath);
  EditorShell shell(window, OptionsWithSource(kInitialSvg, savePath.string()));
  ASSERT_TRUE(shell.valid());

  DriveGlobalShortcut(shell, {ImGuiKey_O}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::OpenFileModalRequested(shell));

  DriveGlobalShortcut(shell, {ImGuiKey_S}, /*ctrl=*/true);
  ASSERT_TRUE(std::filesystem::exists(savePath));
  EXPECT_NE(ReadTextFile(savePath).find("target"), std::string::npos);

  DriveGlobalShortcut(shell, {ImGuiKey_S}, /*ctrl=*/true, /*shift=*/true);
  EXPECT_TRUE(EditorShellTestAccess::SaveFileModalRequested(shell));

  glfwSetWindowShouldClose(window.rawHandle(), GLFW_FALSE);
  DriveGlobalShortcut(shell, {ImGuiKey_Q}, /*ctrl=*/true);
  EXPECT_NE(glfwWindowShouldClose(window.rawHandle()), 0);
}

TEST(EditorShellTest, GlobalShortcutsRouteCanvasSelectionAndShapeClipboard) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::UseInMemoryShapeClipboard(shell);

  DriveGlobalShortcut(shell, {ImGuiKey_A}, /*ctrl=*/true);
  EXPECT_GT(EditorShellTestAccess::App(shell).selectedElements().size(), 1u);

  DriveGlobalShortcut(shell, {ImGuiKey_A}, /*ctrl=*/true, /*shift=*/true);
  EXPECT_FALSE(EditorShellTestAccess::App(shell).hasSelection());

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  DriveGlobalShortcut(shell, {ImGuiKey_C}, /*ctrl=*/true);
  ASSERT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EXPECT_NE(EditorShellTestAccess::ShapeClipboardText(shell).find("target"), std::string::npos);

  DriveGlobalShortcut(shell, {ImGuiKey_V}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("target_pasted"),
            std::string_view::npos);

  DriveGlobalShortcut(shell, {ImGuiKey_F}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("target_pasted2"),
            std::string_view::npos);

  auto pasted =
      EditorShellTestAccess::App(shell).document().document().querySelector("#target_pasted");
  ASSERT_TRUE(pasted.has_value());
  EditorShellTestAccess::App(shell).setSelection(*pasted);
  DriveGlobalShortcut(shell, {ImGuiKey_X}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(
      EditorShellTestAccess::App(shell).document().document().querySelector("#target_pasted"));
}

TEST(EditorShellTest, GlobalShortcutsDeleteUndoRedoAndReorderSelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  constexpr std::string_view kStackedSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <rect id="back" x="0" y="0" width="10" height="10"/>
  <rect id="middle" x="20" y="0" width="10" height="10"/>
  <rect id="front" x="40" y="0" width="10" height="10"/>
</svg>
)svg";
  EditorShell shell(window, OptionsWithSource(kStackedSvg, "stacked.svg"));
  ASSERT_TRUE(shell.valid());

  auto middle = EditorShellTestAccess::App(shell).document().document().querySelector("#middle");
  ASSERT_TRUE(middle.has_value());
  EditorShellTestAccess::App(shell).setSelection(*middle);

  const std::string beforeForward(EditorShellTestAccess::App(shell).document().document().source());
  DriveGlobalShortcut(shell, {ImGuiKey_RightBracket}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  const std::string afterForward(EditorShellTestAccess::App(shell).document().document().source());
  EXPECT_NE(afterForward, beforeForward);

  DriveGlobalShortcut(shell, {ImGuiKey_LeftBracket}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  const std::string afterBackward(EditorShellTestAccess::App(shell).document().document().source());
  EXPECT_NE(afterBackward, afterForward);

  DriveGlobalShortcut(shell, {ImGuiKey_RightBracket}, /*ctrl=*/true, /*shift=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  DriveGlobalShortcut(shell, {ImGuiKey_LeftBracket}, /*ctrl=*/true, /*shift=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));

  middle = EditorShellTestAccess::App(shell).document().document().querySelector("#middle");
  ASSERT_TRUE(middle.has_value());
  EditorShellTestAccess::App(shell).setSelection(*middle);
  DriveGlobalShortcut(shell, {ImGuiKey_Delete});
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).document().document().querySelector("#middle"));

  DriveGlobalShortcut(shell, {ImGuiKey_Z}, /*ctrl=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_TRUE(EditorShellTestAccess::App(shell).document().document().querySelector("#middle"));

  DriveGlobalShortcut(shell, {ImGuiKey_Z}, /*ctrl=*/true, /*shift=*/true);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).document().document().querySelector("#middle"));

  middle = EditorShellTestAccess::App(shell).document().document().querySelector("#front");
  ASSERT_TRUE(middle.has_value());
  EditorShellTestAccess::App(shell).setSelection(*middle);
  DriveGlobalShortcut(shell, {ImGuiKey_Backspace});
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).document().document().querySelector("#front"));
}

TEST(EditorShellTest, MenuActionsRouteCanvasClipboardHistorySelectionAndViewState) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ConfigureViewport(shell, Box2d::FromXYWH(0.0, 0.0, 120.0, 80.0));
  EditorShellTestAccess::UseInMemoryShapeClipboard(shell);

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  MenuBarActions actions;
  actions.copy = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  ASSERT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EXPECT_NE(EditorShellTestAccess::ShapeClipboardText(shell).find("target"), std::string::npos);

  actions = MenuBarActions{};
  actions.cut = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).document().document().querySelector("#target"));

  actions = MenuBarActions{};
  actions.undo = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_TRUE(EditorShellTestAccess::App(shell).document().document().querySelector("#target"));

  actions = MenuBarActions{};
  actions.redo = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).document().document().querySelector("#target"));

  actions = MenuBarActions{};
  actions.revertFile = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::App(shell).document().document().querySelector("#target"));

  actions = MenuBarActions{};
  actions.paste = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_TRUE(
      EditorShellTestAccess::App(shell).document().document().querySelector("#target_pasted"));

  actions = MenuBarActions{};
  actions.pasteInFront = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_TRUE(
      EditorShellTestAccess::App(shell).document().document().querySelector("#target_pasted2"));

  actions = MenuBarActions{};
  actions.selectAllCanvas = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_GT(EditorShellTestAccess::App(shell).selectedElements().size(), 1u);

  actions = MenuBarActions{};
  actions.deselectAllCanvas = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_FALSE(EditorShellTestAccess::App(shell).hasSelection());

  actions = MenuBarActions{};
  actions.selectAll = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_EQ(EditorShellTestAccess::Source(shell).getSelectedText(),
            EditorShellTestAccess::Source(shell).getText());

  actions = MenuBarActions{};
  actions.deselectAll = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::Source(shell).getSelectedText().empty());

  auto label = EditorShellTestAccess::App(shell).document().document().querySelector("#label");
  ASSERT_TRUE(label.has_value());
  EditorShellTestAccess::App(shell).setSelection(*label);
  actions = MenuBarActions{};
  actions.convertTextToOutlines = true;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_TRUE(
      EditorShellTestAccess::App(shell).document().document().querySelector("#label_outlines"));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  const bool sourceFocusBefore = EditorShellTestAccess::SourceFocusMode(shell);
  const bool compositorDebugBefore = EditorShellTestAccess::ShowCompositorDebugPanel(shell);
  const PerfOverlayMode perfOverlayBefore = EditorShellTestAccess::GetPerfOverlayMode(shell);
  ASSERT_EQ(perfOverlayBefore, PerfOverlayMode::Off);

  actions = MenuBarActions{};
  actions.zoomIn = true;
  actions.zoomOut = true;
  actions.actualSize = true;
  actions.toggleSourceFocusMode = true;
  actions.toggleCompositorDebugPanel = true;
  actions.setPerfOverlayMode = true;
  actions.perfOverlayMode = PerfOverlayMode::FullGraph;
  EditorShellTestAccess::ApplyMenuActions(shell, actions);

  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));
  EXPECT_NE(EditorShellTestAccess::SourceFocusMode(shell), sourceFocusBefore);
  EXPECT_NE(EditorShellTestAccess::ShowCompositorDebugPanel(shell), compositorDebugBefore);
  EXPECT_EQ(EditorShellTestAccess::GetPerfOverlayMode(shell), PerfOverlayMode::FullGraph);
}

TEST(EditorShellTest, SourceFocusedMenuUndoAndRedoKeepDocumentHistoryUntouched) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  TextEditor& source = EditorShellTestAccess::Source(shell);
  const std::string original = source.getText();
  source.setSelection(Coordinates(0, 0), Coordinates(0, 0));
  source.insertText(" ");
  ASSERT_NE(source.getText(), original);
  ASSERT_TRUE(source.canUndo());

  MenuBarActions actions;
  actions.undo = true;
  EditorShellTestAccess::ApplyMenuHistoryActions(shell, actions, /*sourcePaneFocused=*/true);
  EXPECT_EQ(source.getText(), original);
  EXPECT_EQ(EditorShellTestAccess::PendingHistoryActionCount(shell), 0u);

  actions = MenuBarActions{};
  actions.redo = true;
  EditorShellTestAccess::ApplyMenuHistoryActions(shell, actions, /*sourcePaneFocused=*/true);
  EXPECT_NE(source.getText(), original);
  EXPECT_EQ(EditorShellTestAccess::PendingHistoryActionCount(shell), 0u);
}

TEST(EditorShellTest, MenuActionsRouteDialogAndExportRequestsWithoutCurrentPath) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShellOptions options;
  options.initialSource = std::string(kInitialSvg);
  EditorShell shell(window, std::move(options));
  ASSERT_TRUE(shell.valid());

  MenuBarActions actions;
  actions.openAbout = true;
  actions.openFile = true;
  actions.saveFile = true;
  actions.saveFileAs = true;
  actions.exportViewportSvg = true;
  actions.exportViewportSvgWithOverlay = true;
  actions.quit = true;
  actions.undo = true;
  actions.redo = true;

  EditorShellTestAccess::ApplyMenuActions(shell, actions);

  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExport(shell));
  EXPECT_TRUE(EditorShellTestAccess::PendingViewportExportOverlay(shell));
  EXPECT_TRUE(shell.valid());
}

TEST(EditorShellTest, ShapeClipboardCopyCutAndPasteUseCanvasSelection) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::UseInMemoryShapeClipboard(shell);

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);

  EditorShellTestAccess::CopySelectedShapesToClipboard(shell);
  EXPECT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EXPECT_NE(EditorShellTestAccess::ShapeClipboardText(shell).find("target"), std::string::npos);

  EditorShellTestAccess::PasteShapesFromClipboard(shell, /*inFront=*/false);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_NE(EditorShellTestAccess::App(shell).document().document().source().find("target_pasted"),
            std::string_view::npos);

  auto pasted =
      EditorShellTestAccess::App(shell).document().document().querySelector("#target_pasted");
  ASSERT_TRUE(pasted.has_value());
  EditorShellTestAccess::App(shell).setSelection(*pasted);
  EditorShellTestAccess::CutSelectedShapesToClipboard(shell);
  EXPECT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  EXPECT_EQ(EditorShellTestAccess::App(shell).document().document().source().find("target_pasted"),
            std::string_view::npos);

  EditorShellTestAccess::ClearShapeClipboard(shell);
  const std::string_view sourceBeforeNullClipboardPaste =
      EditorShellTestAccess::App(shell).document().document().source();
  EditorShellTestAccess::CopySelectedShapesToClipboard(shell);
  EXPECT_FALSE(EditorShellTestAccess::ShapeClipboardHasText(shell));
  EditorShellTestAccess::PasteShapesFromClipboard(shell, /*inFront=*/true);
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);
  EXPECT_EQ(EditorShellTestAccess::App(shell).document().document().source(),
            sourceBeforeNullClipboardPaste);
}

TEST(EditorShellTest, ShapeClipboardRejectsMalformedAndPastesIntoSelectedGroup) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  constexpr std::string_view kGroupedSvg = R"svg(
<svg xmlns="http://www.w3.org/2000/svg" width="120" height="80" viewBox="0 0 120 80">
  <g id="group"><circle id="inside" cx="12" cy="12" r="4"/></g>
  <rect id="target" x="10" y="12" width="40" height="24" fill="#3366cc"/>
</svg>
)svg";
  EditorShell shell(window, OptionsWithSource(kGroupedSvg, "grouped.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::UseInMemoryShapeClipboard(shell);

  EditorShellTestAccess::SetShapeClipboardText(shell, "<rect");
  const std::string_view sourceBeforeMalformedPaste =
      EditorShellTestAccess::App(shell).document().document().source();
  EditorShellTestAccess::PasteShapesFromClipboard(shell, /*inFront=*/false);
  (void)EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell);
  EXPECT_EQ(EditorShellTestAccess::App(shell).document().document().source(),
            sourceBeforeMalformedPaste);

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  auto group = EditorShellTestAccess::App(shell).document().document().querySelector("#group");
  ASSERT_TRUE(target.has_value());
  ASSERT_TRUE(group.has_value());

  EditorShellTestAccess::App(shell).setSelection(*target);
  EditorShellTestAccess::CopySelectedShapesToClipboard(shell);
  ASSERT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));

  EditorShellTestAccess::App(shell).setSelection(*group);
  EditorShellTestAccess::PasteShapesFromClipboard(shell, /*inFront=*/false);
  EXPECT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  const std::string_view source = EditorShellTestAccess::App(shell).document().document().source();
  const std::size_t groupOffset = source.find(R"(id="group")");
  const std::size_t pastedOffset = source.find(R"(id="target_pasted")");
  const std::size_t groupCloseOffset = source.find("</g>");
  ASSERT_NE(groupOffset, std::string_view::npos);
  ASSERT_NE(pastedOffset, std::string_view::npos);
  ASSERT_NE(groupCloseOffset, std::string_view::npos);
  EXPECT_GT(pastedOffset, groupOffset);
  EXPECT_LT(pastedOffset, groupCloseOffset);
}

TEST(EditorShellTest, SamplePickerAppearsBeforeGeneratingThumbnailsAcrossFrames) {
  // Every card must be visible before this test requires every thumbnail.
  gui::EditorWindow window(TestWindowOptions({
      .title = "Donner complete sample picker test",
      .initialWidth = 960,
      .initialHeight = 720,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  const std::size_t sampleCount = GetEditorSampleCatalog().size();
  ASSERT_GT(sampleCount, 0u);

  // The first frame must paint the complete picker before any sample SVG parsing or raster work
  // starts. This keeps thumbnail generation off the startup-to-carousel critical path.
  //
  // Thumbnail rasterization lives in `prepareFrame()`, which `RunEditorFrame` calls *before*
  // `beginFrame()` so raster work never runs inside an active ImGui frame. Rendering the picker
  // through `runFrame()` alone must therefore never rasterize.
  EditorShellTestAccess::SetShowSamplePicker(shell, true);
  EXPECT_EQ(EditorShellTestAccess::SampleThumbnailCursor(shell), 0u);

  window.beginFrame();
  shell.runFrame();
  window.endFrame();

  ASSERT_EQ(EditorShellTestAccess::VisibleSamplePreviewCount(shell), sampleCount);
  EXPECT_TRUE(EditorShellTestAccess::ShowSamplePicker(shell));
  EXPECT_EQ(EditorShellTestAccess::SampleThumbnailCursor(shell), 0u);
  EXPECT_EQ(EditorShellTestAccess::SampleThumbnailGeneratedCount(shell), 0u);

  // Later frames may start bounded background work and publish one result at a time. Drive them
  // exactly as `RunEditorFrame` does: prepare, then begin/run/end the ImGui frame.
  std::size_t previousGeneratedCount = 0u;
  const auto completionDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < completionDeadline) {
    shell.prepareFrame();
    window.beginFrame();
    shell.runFrame();
    window.endFrame();
    const std::size_t generatedCount = EditorShellTestAccess::SampleThumbnailGeneratedCount(shell);
    EXPECT_LE(generatedCount, previousGeneratedCount + 1u)
        << "Thumbnail results must be published incrementally from one bounded worker slot";
    previousGeneratedCount = generatedCount;
    if (generatedCount >= sampleCount) {
      break;
    }
    // Completion is worker-event driven, not frame-count driven. Under parallel macOS test load a
    // thumbnail may legitimately take longer than the former fixed 500-frame/500-ms polling
    // window even though the worker is making progress.
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  EXPECT_TRUE(EditorShellTestAccess::ShowSamplePicker(shell));
  EXPECT_EQ(EditorShellTestAccess::SampleThumbnailSlotCount(shell), sampleCount);
  EXPECT_EQ(EditorShellTestAccess::SampleThumbnailCursor(shell), sampleCount);
  // The built-in catalog is valid SVG, so every slot should rasterize to a bitmap
  // that the picker can upload as a texture.
  EXPECT_EQ(EditorShellTestAccess::SampleThumbnailGeneratedCount(shell), sampleCount);

  // Every card must contain a real, source-dependent Donner render. The hash includes dimensions,
  // row stride, and pixel bytes, so a shared placeholder bitmap cannot satisfy this assertion.
  const auto fingerprint = [](const svg::RendererBitmap& bitmap) {
    constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
    constexpr std::uint64_t kFnvPrime = 1099511628211ULL;
    std::uint64_t hash = kFnvOffset;
    const auto mix = [&](std::uint64_t value) {
      hash ^= value;
      hash *= kFnvPrime;
    };
    mix(static_cast<std::uint64_t>(bitmap.dimensions.x));
    mix(static_cast<std::uint64_t>(bitmap.dimensions.y));
    mix(bitmap.rowBytes);
    for (const std::uint8_t byte : bitmap.pixels) {
      mix(byte);
    }
    return hash;
  };
  std::vector<std::uint64_t> fingerprints;
  fingerprints.reserve(sampleCount);
  for (std::size_t index = 0; index < sampleCount; ++index) {
    const std::optional<svg::RendererBitmap>& bitmap =
        EditorShellTestAccess::SampleThumbnailBitmap(shell, index);
    ASSERT_TRUE(bitmap.has_value()) << "Missing catalog thumbnail at index " << index;
    ASSERT_FALSE(bitmap->empty()) << "Empty catalog thumbnail at index " << index;
    fingerprints.push_back(fingerprint(*bitmap));
  }
  for (std::size_t lhs = 0; lhs < fingerprints.size(); ++lhs) {
    for (std::size_t rhs = lhs + 1u; rhs < fingerprints.size(); ++rhs) {
      EXPECT_NE(fingerprints[lhs], fingerprints[rhs])
          << "Samples " << lhs << " and " << rhs << " reused the same preview pixels";
    }
  }

  // Re-running frames past the end of the catalog is idempotent: the cursor stays
  // clamped and no additional thumbnails are produced.
  shell.prepareFrame();
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EXPECT_EQ(EditorShellTestAccess::SampleThumbnailCursor(shell), sampleCount);
  EXPECT_EQ(EditorShellTestAccess::SampleThumbnailGeneratedCount(shell), sampleCount);
}

TEST(EditorShellTest, GeodeSplashSampleClickPublishesFirstDocumentFrame) {
  gui::EditorWindow window(TestWindowOptions({
      .title = "Geode Splash sample load regression",
      .initialWidth = 960,
      .initialHeight = 720,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }
  EditorShellOptions options = OptionsWithSource(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="640" height="400" viewBox="0 0 640 400"/>)");
  options.showWelcome = true;
  EditorShell shell(window, options);
  ASSERT_TRUE(shell.valid());

  shell.prepareFrame();
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  EditorShellTestAccess::QueueSampleLoad(shell, "geode-splash");
  EditorShellTestAccess::ProcessPendingSampleLoad(shell);
  ASSERT_EQ(EditorShellTestAccess::ActiveSampleId(shell), "geode-splash");
  ASSERT_TRUE(EditorShellTestAccess::SamplePresentationPending(shell));

  bool displayed = false;
  for (const tests::HangGuard guard; guard.active();) {
    shell.prepareFrame();
    window.beginFrame();
    shell.runFrame();
    window.endFrame();
    if (!EditorShellTestAccess::SamplePresentationPending(shell) &&
        EditorShellTestAccess::DisplayedDocVersion(shell) ==
            EditorShellTestAccess::App(shell).document().currentFrameVersion()) {
      displayed = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  EXPECT_TRUE(displayed) << "sample frame did not present: rendererBusy="
                         << EditorShellTestAccess::RendererBusy(shell)
                         << ", version=" << EditorShellTestAccess::DisplayedDocVersion(shell)
                         << ", pending=" << EditorShellTestAccess::SamplePresentationPending(shell);
}

TEST(EditorShellTest, MainDocumentRenderTakesPriorityOverNewCarouselThumbnailWork) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  // Drain the editor's initial document render before opening the picker so this test controls the
  // priority transition precisely.
  for (const tests::HangGuard guard;
       guard.active() && (EditorShellTestAccess::RendererBusy(shell) ||
                          EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));) {
    window.beginFrame();
    shell.runFrame();
    window.endFrame();
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_FALSE(EditorShellTestAccess::RendererBusy(shell));

  EditorShellTestAccess::SetShowSamplePicker(shell, true);
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
  ASSERT_EQ(EditorShellTestAccess::SampleThumbnailGeneratedCount(shell), 0u);

  // On the next picker frame a low-priority thumbnail may be submitted. A simultaneous main
  // document request must still enter the normal busy state immediately and no thumbnail result
  // may have been synchronously generated on the UI thread.
  shell.asyncRendererForReplay().setReplayRenderDelayForTesting(std::chrono::milliseconds(75));
  EditorShellTestAccess::SetRequestRenderAtEndOfFrame(shell);
  window.beginFrame();
  shell.runFrame();
  window.endFrame();

  EXPECT_TRUE(EditorShellTestAccess::RendererBusy(shell));
  EXPECT_EQ(EditorShellTestAccess::SampleThumbnailGeneratedCount(shell), 0u);
  shell.asyncRendererForReplay().setReplayRenderDelayForTesting(std::chrono::milliseconds(0));
}

TEST(EditorShellTest, TeardownKeepsFontProviderUntilTextThumbnailWorkerStopsAndDetachesWake) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  const svg::FontFamilyProvider* previousProvider = svg::FontManager::DefaultFontProvider();
  auto shell = std::make_unique<EditorShell>(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell->valid());
  const svg::FontFamilyProvider* shellProvider = &shell->fontCatalog();
  ASSERT_EQ(svg::FontManager::DefaultFontProvider(), shellProvider);

  const EditorSample* textSample = FindEditorSample("text-style");
  ASSERT_NE(textSample, nullptr);
  svg::Renderer thumbnailRoot;

  std::mutex wakeMutex;
  std::condition_variable wakeCv;
  bool wakeEntered = false;
  bool releaseWake = false;
  std::atomic<int> wakeCount{0};
  shell->asyncRendererForReplay().setWakeCallback([&] {
    wakeCount.fetch_add(1, std::memory_order_relaxed);
    std::unique_lock<std::mutex> lock(wakeMutex);
    wakeEntered = true;
    wakeCv.notify_all();
    wakeCv.wait(lock, [&] { return releaseWake; });
  });
  ASSERT_TRUE(shell->asyncRendererForReplay().requestSampleThumbnail(SampleThumbnailRenderRequest{
      .key = 91u,
      .source = std::string(textSample->source),
      .dimensions = Vector2i(192, 120),
      .nativeRenderer = &thumbnailRoot,
  }));

  {
    std::unique_lock<std::mutex> lock(wakeMutex);
    ASSERT_TRUE(wakeCv.wait_for(lock, std::chrono::seconds(5), [&] { return wakeEntered; }))
        << "Expected the text thumbnail worker to reach its completion wake";
  }

  std::atomic<bool> destroyStarted{false};
  std::atomic<bool> destroyFinished{false};
  std::thread destroyer([&] {
    destroyStarted.store(true, std::memory_order_release);
    shell.reset();
    destroyFinished.store(true, std::memory_order_release);
  });
  while (!destroyStarted.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  // The worker is deliberately latched in its copied wake callback. Destruction therefore cannot
  // finish, and the FontCatalog must remain installed until the renderer has joined that worker.
  const auto providerDeadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(250);
  while (svg::FontManager::DefaultFontProvider() == shellProvider &&
         std::chrono::steady_clock::now() < providerDeadline) {
    std::this_thread::yield();
  }
  EXPECT_EQ(svg::FontManager::DefaultFontProvider(), shellProvider)
      << "EditorShell detached/destroyed its font provider before AsyncRenderer joined";
  EXPECT_FALSE(destroyFinished.load(std::memory_order_acquire));

  {
    std::lock_guard<std::mutex> lock(wakeMutex);
    releaseWake = true;
  }
  wakeCv.notify_all();
  destroyer.join();

  EXPECT_TRUE(destroyFinished.load(std::memory_order_acquire));
  EXPECT_EQ(svg::FontManager::DefaultFontProvider(), previousProvider);
  const int wakesAfterTeardown = wakeCount.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::milliseconds(25));
  EXPECT_EQ(wakeCount.load(std::memory_order_relaxed), wakesAfterTeardown)
      << "A copied renderer wake callback ran after EditorShell teardown completed";
}

TEST(EditorShellTest, PendingEndOfFrameRenderDoesNotStarveSidebarSnapshotRefresh) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  ASSERT_FALSE(EditorShellTestAccess::RendererBusy(shell));
  ASSERT_EQ(EditorShellTestAccess::VisibleLayerRowCount(shell), 0u);

  EditorShellTestAccess::SetRequestRenderAtEndOfFrame(shell);
  window.beginFrame();
  EditorShellTestAccess::RenderSidebars(shell);
  window.endFrame();

  EXPECT_GT(EditorShellTestAccess::VisibleLayerRowCount(shell), 0u);
}

TEST(EditorShellTest, SamplePickerRendersDiscardConfirmationForDirtyDocument) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  // Dirty the document so a queued sample load must ask before discarding edits.
  EditorShellTestAccess::Source(shell).setText("<svg>unsaved edits</svg>");
  EditorShellTestAccess::QueueSampleLoad(shell, "basic-shapes");
  EXPECT_TRUE(EditorShellTestAccess::ShowSamplePicker(shell));

  // Driving frames runs processPendingSampleLoad(), which flags the load as
  // needing confirmation, and renderSamplePicker() then draws the modal. The
  // pending load and picker both survive because nothing has confirmed yet.
  for (int frame = 0; frame < 3; ++frame) {
    window.beginFrame();
    shell.runFrame();
    window.endFrame();
  }

  EXPECT_TRUE(EditorShellTestAccess::PendingSampleLoadNeedsConfirmation(shell));
  EXPECT_EQ(EditorShellTestAccess::PendingSampleLoad(shell), "basic-shapes");
  EXPECT_TRUE(EditorShellTestAccess::ShowSamplePicker(shell));
}

namespace {

/// Run one editor frame with an injected mouse state (compact-UI click driver).
void RunFrameWithMouse(gui::EditorWindow& window, EditorShell& shell, const ImVec2& mouse,
                       bool mouseDown) {
  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, mouseDown);
  window.beginFrame();
  shell.runFrame();
  window.endFrame();
}

const DocumentPixelCapture* WaitForDocumentPixelCapture(gui::EditorWindow& window,
                                                        EditorShell& shell) {
  for (const tests::HangGuard guard; guard.active();) {
    RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
    if (!shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(guard.deadline())) {
      return nullptr;
    }
    RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
    if (const DocumentPixelCapture* capture = EditorShellTestAccess::PixelCapture(shell)) {
      return capture;
    }
    // The canvas-size commit debounce uses wall-clock time. Let its idle wake mature between
    // frames instead of assuming four fast test frames are longer than that delay.
    std::this_thread::sleep_for(std::chrono::milliseconds(15));
  }
  return nullptr;
}

svg::RendererBitmap CaptureFrameWithMouse(gui::EditorWindow& window, EditorShell& shell,
                                          const ImVec2& mouse) {
  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(mouse.x, mouse.y);
  io.AddMouseButtonEvent(0, false);
  window.beginFrame();
  shell.runFrame();
  return window.endFrameAndReadPixels();
}

void WriteEyedropperScreenshot(const svg::RendererBitmap& bitmap, std::string_view name) {
  const char* outputDir = std::getenv("TEST_UNDECLARED_OUTPUTS_DIR");
  ASSERT_NE(outputDir, nullptr);
  ASSERT_FALSE(bitmap.empty());
  const std::filesystem::path path = std::filesystem::path(outputDir) / name;
  EXPECT_TRUE(svg::RendererImageIO::writeRgbaPixelsToPngFile(
      path.string().c_str(), bitmap.pixels, bitmap.dimensions.x, bitmap.dimensions.y,
      bitmap.rowBytes / 4u));
}

svg::RendererBitmap CapturePaintWidgetFrame(gui::EditorWindow& window, EditorShell& shell,
                                            const ImVec2& cursor) {
  ImGuiIO& io = ImGui::GetIO();
  io.AddMousePosEvent(-1.0f, -1.0f);
  io.AddMouseButtonEvent(0, false);
  window.beginFrame();
  constexpr ImGuiWindowFlags kHostFlags =
      ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings;
  ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(220.0f, 100.0f), ImGuiCond_Always);
  ImGui::Begin("EditorShellPaintWidgetCaptureHost", nullptr, kHostFlags);
  ImGui::SetCursorScreenPos(cursor);
  EditorShellTestAccess::RenderFillStrokeToolbarWidget(shell);
  ImGui::End();
  return window.endFrameAndReadPixels();
}

/// Hover, press, and release at @p pos; ImGui buttons fire on release.
void ClickAt(gui::EditorWindow& window, EditorShell& shell, const ImVec2& pos) {
  RunFrameWithMouse(window, shell, pos, /*mouseDown=*/false);
  RunFrameWithMouse(window, shell, pos, /*mouseDown=*/true);
  RunFrameWithMouse(window, shell, pos, /*mouseDown=*/false);
}

enum class HistoryAvailability {
  None,
  UndoOnly,
  RedoOnly,
  UndoAndRedo,
};

HistoryAvailability CurrentHistoryAvailability(const EditorApp& app) {
  if (app.canUndo()) {
    return app.canRedo() ? HistoryAvailability::UndoAndRedo : HistoryAvailability::UndoOnly;
  }
  return app.canRedo() ? HistoryAvailability::RedoOnly : HistoryAvailability::None;
}

bool RunFramesUntilHistoryState(gui::EditorWindow& window, EditorShell& shell,
                                HistoryAvailability expectedAvailability) {
  for (const tests::HangGuard guard; guard.active();) {
    if (CurrentHistoryAvailability(EditorShellTestAccess::App(shell)) == expectedAvailability) {
      return true;
    }
    RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), /*mouseDown=*/false);
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

/// Center of the Nth compact top-bar button (Open, Undo, Redo, Layers,
/// Inspector), derived from the same layout math as renderCompactTopBar.
ImVec2 CompactTopBarButtonCenter(const gui::EditorWindow& window, const EditorShell& shell,
                                 int buttonIndex) {
  const EditorAdaptiveUiLayout& layout = EditorShellTestAccess::AdaptiveUiLayout(shell);
  const float targetSize = std::max(44.0f, layout.toolButtonSize);
  constexpr float kButtonGap = 4.0f;
  constexpr int kButtonCount = 5;
  const float controlsWidth = targetSize * kButtonCount + kButtonGap * (kButtonCount - 1);
  const float controlsX =
      std::max(8.0f, static_cast<float>(window.windowSize().x) - controlsWidth - 8.0f);
  const float x = controlsX + static_cast<float>(buttonIndex) * (targetSize + kButtonGap);
  return ImVec2(x + targetSize * 0.5f, 4.0f + targetSize * 0.5f);
}

}  // namespace

TEST(EditorShellTest, CompactTopBarButtonsToggleSheetsAndSamplePicker) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  // 640x480 is under the compact height threshold, so the shell selects the
  // compact-touch chrome with the five-button top bar.
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), /*mouseDown=*/false);
  ASSERT_TRUE(EditorShellTestAccess::AdaptiveUiLayout(shell).compactTouch());
  EXPECT_FALSE(EditorShellTestAccess::CompactPanelVisible(shell));

  // Layers button opens the layers sheet.
  ClickAt(window, shell, CompactTopBarButtonCenter(window, shell, 3));
  EXPECT_TRUE(EditorShellTestAccess::CompactPanelVisible(shell));
  EXPECT_FALSE(EditorShellTestAccess::CompactInspectorSheet(shell));

  // Inspector button switches the sheet to the inspector. (That the sheet
  // itself renders is proven by CompactSheetHeaderCloseButtonHidesPanel, which
  // clicks a button that only exists inside the rendered sheet.)
  ClickAt(window, shell, CompactTopBarButtonCenter(window, shell, 4));
  EXPECT_TRUE(EditorShellTestAccess::CompactPanelVisible(shell));
  EXPECT_TRUE(EditorShellTestAccess::CompactInspectorSheet(shell));

  // Clicking Inspector again dismisses the sheet.
  ClickAt(window, shell, CompactTopBarButtonCenter(window, shell, 4));
  EXPECT_FALSE(EditorShellTestAccess::CompactPanelVisible(shell));

  // The Open button clears any pending sample load and shows the picker.
  EXPECT_FALSE(EditorShellTestAccess::ShowSamplePicker(shell));
  ClickAt(window, shell, CompactTopBarButtonCenter(window, shell, 0));
  EXPECT_TRUE(EditorShellTestAccess::ShowSamplePicker(shell));
  EXPECT_TRUE(EditorShellTestAccess::PendingSampleLoad(shell).empty());
}

TEST(EditorShellTest, CompactUndoRedoButtonsDriveDocumentHistory) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::UseInMemoryShapeClipboard(shell);

  // Record an undoable mutation: paste a copy of the selected shape.
  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  EditorShellTestAccess::CopySelectedShapesToClipboard(shell);
  EditorShellTestAccess::PasteShapesFromClipboard(shell, /*inFront=*/false);
  ASSERT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  ASSERT_TRUE(EditorShellTestAccess::App(shell).canUndo());

  shell.asyncRendererForReplay().setReplayRenderDelayForTesting(std::chrono::milliseconds(200));
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), /*mouseDown=*/false);
  ASSERT_TRUE(EditorShellTestAccess::AdaptiveUiLayout(shell).compactTouch());
  ASSERT_TRUE(shell.asyncRendererForReplay().isBusy());

  // Undo requested while the renderer owns the document is deferred until the worker releases
  // its read access, then removes the pasted shape from the document history.
  ClickAt(window, shell, CompactTopBarButtonCenter(window, shell, 1));
  ASSERT_TRUE(RunFramesUntilHistoryState(window, shell, HistoryAvailability::RedoOnly));
  EXPECT_FALSE(EditorShellTestAccess::App(shell).canUndo());
  EXPECT_TRUE(EditorShellTestAccess::App(shell).canRedo());

  // Redo restores it.
  ClickAt(window, shell, CompactTopBarButtonCenter(window, shell, 2));
  ASSERT_TRUE(RunFramesUntilHistoryState(window, shell, HistoryAvailability::UndoOnly));
  EXPECT_TRUE(EditorShellTestAccess::App(shell).canUndo());
  EXPECT_FALSE(EditorShellTestAccess::App(shell).canRedo());
}

TEST(EditorShellTest, DeferredHistoryPreservesRepeatedCommands) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg, "initial.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::UseInMemoryShapeClipboard(shell);

  auto target = EditorShellTestAccess::App(shell).document().document().querySelector("#target");
  ASSERT_TRUE(target.has_value());
  EditorShellTestAccess::App(shell).setSelection(*target);
  EditorShellTestAccess::CopySelectedShapesToClipboard(shell);
  for (int i = 0; i < 2; ++i) {
    EditorShellTestAccess::PasteShapesFromClipboard(shell, /*inFront=*/false);
    ASSERT_TRUE(EditorShellTestAccess::FlushQueuedMutationAndRefreshOverlay(shell));
  }

  shell.asyncRendererForReplay().setReplayRenderDelayForTesting(std::chrono::milliseconds(200));
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), /*mouseDown=*/false);
  ASSERT_TRUE(shell.asyncRendererForReplay().isBusy());

  EditorShellTestAccess::RequestUndo(shell);
  EditorShellTestAccess::RequestUndo(shell);
  EXPECT_EQ(EditorShellTestAccess::PendingHistoryActionCount(shell), 2u);
  ASSERT_TRUE(RunFramesUntilHistoryState(window, shell, HistoryAvailability::RedoOnly));
  EXPECT_EQ(EditorShellTestAccess::PendingHistoryActionCount(shell), 0u);

  ASSERT_TRUE(shell.asyncRendererForReplay().isBusy());
  EditorShellTestAccess::RequestRedo(shell);
  EditorShellTestAccess::RequestRedo(shell);
  EXPECT_EQ(EditorShellTestAccess::PendingHistoryActionCount(shell), 2u);
  ASSERT_TRUE(RunFramesUntilHistoryState(window, shell, HistoryAvailability::UndoOnly));
  EXPECT_EQ(EditorShellTestAccess::PendingHistoryActionCount(shell), 0u);
}

TEST(EditorShellTest, CompactSheetHeaderCloseButtonHidesPanel) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());

  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), /*mouseDown=*/false);
  ASSERT_TRUE(EditorShellTestAccess::AdaptiveUiLayout(shell).compactTouch());
  EditorShellTestAccess::SetCompactPanelVisible(shell, true);
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), /*mouseDown=*/false);
  EXPECT_TRUE(EditorShellTestAccess::CompactPanelVisible(shell));

  // The close button is a headerHeight-square invisible button flush to the
  // sheet's top-right content corner (default window padding is 8 px).
  const EditorAdaptiveUiLayout& layout = EditorShellTestAccess::AdaptiveUiLayout(shell);
  const float headerHeight = std::max(44.0f, layout.toolButtonSize);
  const ImVec2 closeCenter(layout.panelX + layout.panelWidth - 8.0f - headerHeight * 0.5f,
                           layout.panelY + 8.0f + headerHeight * 0.5f);
  ClickAt(window, shell, closeCenter);
  EXPECT_FALSE(EditorShellTestAccess::CompactPanelVisible(shell))
      << "Clicking the sheet header's close button must hide the compact panel.";
}

TEST(EditorShellTest, ToolbarSwapUpdatesEverySelectedElementInOneUndoStep) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  const std::string source =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">)"
      R"(<rect id="first" x="4" y="4" width="24" height="24" style="opacity: 0.5" )"
      R"(fill="red" stroke="blue" stroke-width="2"/>)"
      R"(<rect id="second" x="36" y="4" width="24" height="24" )"
      R"(fill="green" stroke="black" stroke-width="2"/>)"
      R"(</svg>)";
  EditorShell shell(window, OptionsWithSource(source, "paint_swap_multi.svg"));
  ASSERT_EQ(shell.valid(), true);
  EditorApp& app = EditorShellTestAccess::App(shell);
  svg::SVGDocument& document = app.document().document();
  const auto first = document.querySelector("#first");
  const auto second = document.querySelector("#second");
  ASSERT_THAT(first, testing::Ne(std::nullopt));
  ASSERT_THAT(second, testing::Ne(std::nullopt));
  app.setSelection(std::vector<svg::SVGElement>{*first, *second});
  ASSERT_THAT(app.selectedElements(), testing::SizeIs(2));
  const svg::PaintServer firstFill = first->getComputedStyle().fill.get().value();
  const svg::PaintServer firstStroke = first->getComputedStyle().stroke.get().value();
  const std::string before(document.source());

  constexpr ImVec2 kCursor(20.0f, 40.0f);
  const auto layout = internal::ComputeFillStrokeWidgetLayout(kCursor, ImVec2(138.0f, 70.0f));
  const ImVec2 swapCenter((layout.swapMin.x + layout.swapMax.x) * 0.5f,
                          (layout.swapMin.y + layout.swapMax.y) * 0.5f);
  ClickToolbar(window, shell, kCursor, swapCenter);

  // Every selected element receives the swapped paints from the first element through one merged
  // style write per element, with unrelated declarations preserved.
  for (const svg::SVGElement& element : app.selectedElements()) {
    EXPECT_THAT(element.getComputedStyle().fill.get(), testing::Optional(firstStroke));
    EXPECT_THAT(element.getComputedStyle().stroke.get(), testing::Optional(firstFill));
  }
  const auto firstStyle = first->getAttribute("style");
  ASSERT_TRUE(firstStyle.has_value());
  EXPECT_THAT(std::string(*firstStyle), testing::HasSubstr("opacity: 0.5"));
  const std::string afterFirstSwap(document.source());
  EXPECT_NE(afterFirstSwap, before);
  ASSERT_TRUE(app.canUndo());
  ASSERT_TRUE(app.undoTimeline().nextUndoLabel().has_value());
  EXPECT_EQ(*app.undoTimeline().nextUndoLabel(), "Swap fill and stroke");

  // A second swap restores the original values on the source element.
  ClickToolbar(window, shell, kCursor, swapCenter);
  for (const svg::SVGElement& element : app.selectedElements()) {
    EXPECT_THAT(element.getComputedStyle().fill.get(), testing::Optional(firstFill));
    EXPECT_THAT(element.getComputedStyle().stroke.get(), testing::Optional(firstStroke));
  }
  const std::string afterSecondSwap(document.source());
  EXPECT_NE(afterSecondSwap, afterFirstSwap);

  // Each swap is one coherent undo step for the whole selection.
  app.undo();
  app.flushFrame();
  EXPECT_EQ(document.source(), afterFirstSwap);
  app.undo();
  app.flushFrame();
  EXPECT_EQ(document.source(), before);
  app.redo();
  app.flushFrame();
  EXPECT_EQ(document.source(), afterFirstSwap);
  app.redo();
  app.flushFrame();
  EXPECT_EQ(document.source(), afterSecondSwap);
}

TEST(EditorShellTest, EyedropperShortcutRespectsTextInputAndRestoresPreviousTool) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_THAT(shell.valid(), testing::Eq(true));

  DriveGlobalShortcut(shell, {ImGuiKey_I}, /*ctrl=*/false, /*shift=*/false, /*super=*/false,
                      /*textInputActive=*/true);
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsSelect(shell), testing::Eq(true));
  ImGui::GetIO().WantTextInput = false;
  DriveGlobalShortcut(shell, {ImGuiKey_P});
  DriveGlobalShortcut(shell, {ImGuiKey_I});
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsEyedropper(shell), testing::Eq(true));
  DriveGlobalShortcut(shell, {ImGuiKey_Escape});
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsPen(shell), testing::Eq(true));

  DriveGlobalShortcut(shell, {ImGuiKey_I});
  DriveGlobalShortcut(shell, {ImGuiKey_V});
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsSelect(shell), testing::Eq(true));
}

TEST(EditorShellTest, ReplayToolSwitchCancelsEyedropperCapture) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  for (const std::string_view tool : {"pen", "text", "select"}) {
    ASSERT_THAT(EditorShellTestAccess::ArmEyedropper(shell, /*stroke=*/false), testing::Eq(true));
    ASSERT_THAT(EditorShellTestAccess::EyedropperCaptureEnabled(shell), testing::Eq(true));
    EditorShellTestAccess::ApplyReplayAction(shell,
                                             repro::ReproAction{
                                                 .kind = repro::ReproAction::Kind::SetActiveTool,
                                                 .tool = std::string(tool),
                                             });
    EXPECT_THAT(EditorShellTestAccess::EyedropperCaptureEnabled(shell), testing::Eq(false));
    EXPECT_THAT(EditorShellTestAccess::ActiveToolIsEyedropper(shell), testing::Eq(false));
  }
}

TEST(EditorShellTest, DeferredRenderRegistersWarmupReleaseWake) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  std::atomic<int> wakes = 0;
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  auto& renderer = shell.asyncRendererForReplay();
  renderer.setWakeCallback([&] { ++wakes; });
  renderer.stageCompositorWarmupForTesting(/*pending=*/false, /*active=*/true);
  EditorShellTestAccess::SetRequestRenderAtEndOfFrame(shell);
  EditorShellTestAccess::ApplyDeferredRenderRequest(shell);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));
  EXPECT_EQ(wakes.load(), 0);
  renderer.completeCompositorWarmupForTesting();
  EXPECT_EQ(wakes.load(), 1)
      << "The shell must register its retained render demand before waiting for warmup.";
}

TEST(EditorShellTest, ViewportDeadlineRegistersWarmupWakeWithoutPendingRenderRequest) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  std::atomic<int> wakes = 0;
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  auto& renderer = shell.asyncRendererForReplay();
  renderer.setWakeCallback([&] { ++wakes; });
  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  EditorShellTestAccess::ExpireViewportSettleWake(shell);
  renderer.stageCompositorWarmupForTesting(/*pending=*/false, /*active=*/true);
  EXPECT_TRUE(EditorShellTestAccess::Coordinator(shell).hasPendingViewportSettle());
  EXPECT_EQ(EditorShellTestAccess::ViewportRenderWake(shell), std::nullopt);
  EXPECT_EQ(EditorShellTestAccess::ViewportRenderWake(shell), std::nullopt);
  EXPECT_FALSE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));
  EXPECT_EQ(wakes.load(), 0);
  renderer.completeCompositorWarmupForTesting();
  EXPECT_EQ(wakes.load(), 1)
      << "Hiding the deadline behind warmup must register one wake before the host parks.";
  EXPECT_THAT(EditorShellTestAccess::ViewportRenderWake(shell),
              testing::Optional(testing::Eq(0.0f)));
  EditorShellTestAccess::QueueIdleRenderRefresh(shell);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));
}

TEST(EditorShellTest, IdleViewportSettleWakeQueuesRenderAndWaitsBehindPicker) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  EditorShellTestAccess::ExpireViewportSettleWake(shell);
  ASSERT_THAT(shell.nextIdleWakeSeconds(), testing::Optional(testing::Eq(0.0f)));
  EditorShellTestAccess::QueueIdleRenderRefresh(shell);
  EXPECT_TRUE(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell));

  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  EditorShellTestAccess::SetShowSamplePicker(shell, true);
  EXPECT_EQ(EditorShellTestAccess::ViewportRenderWake(shell), std::nullopt);
  EditorShellTestAccess::SetShowSamplePicker(shell, false);
  EXPECT_THAT(EditorShellTestAccess::ViewportRenderWake(shell),
              testing::Optional(testing::Eq(0.0f)));
}

TEST(EditorShellTest, IdleEyedropperArmingAndCanvasCommitWakeDispatchRender) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
  ASSERT_TRUE(shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(3)));
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);

  ASSERT_THAT(EditorShellTestAccess::ArmEyedropper(shell, /*stroke=*/false), testing::Eq(true));
  EXPECT_THAT(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell), testing::Eq(true))
      << "Arming on an idle canvas must submit the first capture without another input event.";
  const DocumentPixelCapture* capture = WaitForDocumentPixelCapture(window, shell);
  ASSERT_NE(capture, nullptr) << "The idle arm did not produce a document pixel capture.";

  // Represent the next idle frame after the first capture request has cleared its one-shot flag.
  ASSERT_TRUE(shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(3)));
  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  ASSERT_THAT(EditorShellTestAccess::RequestRenderAtEndOfFrame(shell), testing::Eq(false));
  EditorShellTestAccess::SetExpiredEyedropperCanvasCommitWake(shell);
  ASSERT_THAT(shell.nextIdleWakeSeconds(), testing::Optional(testing::Eq(0.0f)));
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
  EXPECT_THAT(EditorShellTestAccess::EyedropperCanvasCommitWakePending(shell), testing::Eq(false))
      << "A timer-driven idle frame must reach RenderCoordinator::maybeRequestRender.";
  shell.asyncRendererForReplay().cancelInFlight();
  ASSERT_TRUE(shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(3)));
}

TEST(EditorShellTest, CancelledIdleEyedropperCaptureRepostsThroughShellFrame) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
  ASSERT_TRUE(shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(3)));
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
  ASSERT_THAT(EditorShellTestAccess::ArmEyedropper(shell, /*stroke=*/false), testing::Eq(true));
  const DocumentPixelCapture* capture = WaitForDocumentPixelCapture(window, shell);
  ASSERT_NE(capture, nullptr);

  EditorShellTestAccess::RestartEyedropperCapture(shell);
  EditorShellTestAccess::ClearRequestRenderAtEndOfFrame(shell);
  shell.asyncRendererForReplay().setReplayRenderDelayForTesting(std::chrono::milliseconds(500));
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
  ASSERT_THAT(EditorShellTestAccess::EyedropperCaptureRequestPending(shell), testing::Eq(true));
  ASSERT_THAT(EditorShellTestAccess::RendererBusy(shell), testing::Eq(true));

  shell.asyncRendererForReplay().cancelInFlight();
  ASSERT_TRUE(shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(3)));
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
  EXPECT_THAT(EditorShellTestAccess::RendererBusy(shell), testing::Eq(true))
      << "A cancelled same-identity capture must repost from the completion-woken shell frame.";
  ASSERT_TRUE(shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(3)));
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
  EXPECT_NE(EditorShellTestAccess::PixelCapture(shell), nullptr);
}

TEST(EditorShellTest, ToolPaletteButtonsReachTheCenteredBottomEdge) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_THAT(shell.valid(), testing::Eq(true));

  constexpr ImVec2 kPaneOrigin(0.0f, 0.0f);
  constexpr ImVec2 kContentRegion(640.0f, 480.0f);
  const Box2d palette =
      EditorShellTestAccess::ToolPaletteScreenRect(shell, kPaneOrigin, kContentRegion);
  const float buttonSize = EditorShellTestAccess::AdaptiveUiLayout(shell).toolButtonSize;
  ASSERT_THAT(buttonSize, testing::Lt(44.0f));
  const float paletteCenterY =
      (static_cast<float>(palette.topLeft.y) + static_cast<float>(palette.bottomRight.y)) * 0.5f;
  const float penButtonCenterX =
      static_cast<float>(palette.topLeft.x) + 8.0f + buttonSize + 4.0f + buttonSize * 0.5f;
  const ImVec2 abovePenButton(penButtonCenterX, paletteCenterY - buttonSize * 0.5f - 2.0f);
  const ImVec2 penButtonNearBottom(penButtonCenterX, paletteCenterY + buttonSize * 0.5f - 2.0f);

  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, abovePenButton, false);
  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, abovePenButton, true);
  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, abovePenButton, false);
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsSelect(shell), testing::Eq(true));

  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, penButtonNearBottom, false);
  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, penButtonNearBottom, true);
  RenderToolPaletteFrame(window, shell, kPaneOrigin, kContentRegion, penButtonNearBottom, false);
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsPen(shell), testing::Eq(true));
}

TEST(EditorShellTest, ToolbarEyedropperButtonArmsWithoutSamplingItsActivationClick) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  const ImVec2 paneOrigin(0.0f, 0.0f);
  const ImVec2 contentRegion(640.0f, 480.0f);
  const Box2d palette =
      EditorShellTestAccess::ToolPaletteScreenRect(shell, paneOrigin, contentRegion);
  const float buttonSize = EditorShellTestAccess::AdaptiveUiLayout(shell).toolButtonSize;
  const ImVec2 eyedropperCenter(
      static_cast<float>(palette.topLeft.x) + 8.0f + 3.0f * (buttonSize + 4.0f) + buttonSize * 0.5f,
      static_cast<float>(palette.topLeft.y) + 8.0f + buttonSize * 0.5f);
  RenderToolPaletteFrame(window, shell, paneOrigin, contentRegion, eyedropperCenter,
                         /*mouseDown=*/false);
  RenderToolPaletteFrame(window, shell, paneOrigin, contentRegion, eyedropperCenter,
                         /*mouseDown=*/true);
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsEyedropper(shell), testing::Eq(false));
  RenderToolPaletteFrame(window, shell, paneOrigin, contentRegion, eyedropperCenter,
                         /*mouseDown=*/false);
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsEyedropper(shell), testing::Eq(true));
}

TEST(EditorShellTest, StrokeColorPopupEyedropperButtonTargetsStroke) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  constexpr ImVec2 kWidgetCursor(20.0f, 40.0f);
  constexpr ImVec2 kStrokeSwatch(57.0f, 76.0f);
  ClickToolbar(window, shell, kWidgetCursor, kStrokeSwatch);
  EXPECT_THAT(EditorShellTestAccess::ActivePaintTargetIsStroke(shell), testing::Eq(true));
  ClickToolbar(window, shell, kWidgetCursor, kStrokeSwatch);
  const std::optional<ImVec2> popupButton = CurrentPopupFirstButtonCenter();
  ASSERT_THAT(popupButton, testing::Optional(testing::_));
  RenderToolbarFrame(window, shell, kWidgetCursor, *popupButton, /*mouseDown=*/false);
  RenderToolbarFrame(window, shell, kWidgetCursor, *popupButton, /*mouseDown=*/true);
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsEyedropper(shell), testing::Eq(false));
  RenderToolbarFrame(window, shell, kWidgetCursor, *popupButton, /*mouseDown=*/false);
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsEyedropper(shell), testing::Eq(true));
  EXPECT_THAT(EditorShellTestAccess::EyedropperTargetsStroke(shell), testing::Eq(true));
}

TEST(EditorShellTest, ActivePaintSwatchRoutesToolbarAndShortcutWithoutChangingSource) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
<rect id="target" x="4" y="4" width="40" height="40" fill="red" stroke="blue"/>
</svg>)";
  EditorShell shell(window, OptionsWithSource(kSource));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  EditorApp& app = EditorShellTestAccess::App(shell);
  const auto target = app.document().document().querySelector("#target");
  ASSERT_THAT(target, testing::Optional(testing::_));
  app.setSelection(*target);
  const std::string sourceBefore(app.document().document().source());
  constexpr ImVec2 kWidgetCursor(20.0f, 40.0f);
  constexpr ImVec2 kStrokeOnly(57.0f, 76.0f);
  constexpr ImVec2 kFillOnly(28.0f, 48.0f);

  EXPECT_THAT(EditorShellTestAccess::ActivePaintTargetIsStroke(shell), testing::Eq(false));
  ClickToolbar(window, shell, kWidgetCursor, kStrokeOnly);
  EXPECT_THAT(EditorShellTestAccess::ActivePaintTargetIsStroke(shell), testing::Eq(true));
  EXPECT_THAT(CurrentPopupFirstButtonCenter(), testing::Eq(std::nullopt));
  EXPECT_THAT(std::string(app.document().document().source()), testing::Eq(sourceBefore));
  EXPECT_THAT(app.canUndo(), testing::Eq(false));

  const ImVec2 paneOrigin(0.0f, 0.0f);
  const ImVec2 contentRegion(640.0f, 480.0f);
  const Box2d palette =
      EditorShellTestAccess::ToolPaletteScreenRect(shell, paneOrigin, contentRegion);
  const float buttonSize = EditorShellTestAccess::AdaptiveUiLayout(shell).toolButtonSize;
  const ImVec2 eyedropperCenter(
      static_cast<float>(palette.topLeft.x) + 8.0f + 3.0f * (buttonSize + 4.0f) + buttonSize * 0.5f,
      static_cast<float>(palette.topLeft.y) + 8.0f + buttonSize * 0.5f);
  RenderToolPaletteFrame(window, shell, paneOrigin, contentRegion, eyedropperCenter, false);
  RenderToolPaletteFrame(window, shell, paneOrigin, contentRegion, eyedropperCenter, true);
  RenderToolPaletteFrame(window, shell, paneOrigin, contentRegion, eyedropperCenter, false);
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsEyedropper(shell), testing::Eq(true));
  EXPECT_THAT(EditorShellTestAccess::EyedropperTargetsStroke(shell), testing::Eq(true));

  ClickToolbar(window, shell, kWidgetCursor, kFillOnly);
  EXPECT_THAT(EditorShellTestAccess::ActivePaintTargetIsStroke(shell), testing::Eq(false));
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsEyedropper(shell), testing::Eq(false));
  EXPECT_THAT(EditorShellTestAccess::EyedropperCaptureEnabled(shell), testing::Eq(false));
  EXPECT_THAT(std::string(app.document().document().source()), testing::Eq(sourceBefore));
  EXPECT_THAT(app.canUndo(), testing::Eq(false));

  DriveGlobalShortcut(shell, {ImGuiKey_I});
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsEyedropper(shell), testing::Eq(true));
  EXPECT_THAT(EditorShellTestAccess::EyedropperTargetsStroke(shell), testing::Eq(false));
  DriveGlobalShortcut(shell, {ImGuiKey_Escape});
  EXPECT_THAT(EditorShellTestAccess::ActivePaintTargetIsStroke(shell), testing::Eq(false));
}

TEST(EditorShellTest, SingleNoneControlClearsActiveSelectedStrokeWithOneUndo) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
<rect id="first" x="4" y="4" width="20" height="20" fill="red" stroke="blue"/>
<rect id="second" x="30" y="4" width="20" height="20" fill="green" stroke="black"/>
</svg>)";
  EditorShell shell(window, OptionsWithSource(kSource));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  EditorApp& app = EditorShellTestAccess::App(shell);
  svg::SVGDocument& document = app.document().document();
  const auto first = document.querySelector("#first");
  const auto second = document.querySelector("#second");
  ASSERT_THAT(first, testing::Optional(testing::_));
  ASSERT_THAT(second, testing::Optional(testing::_));
  app.setSelection(std::vector<svg::SVGElement>{*first, *second});
  const std::string before(document.source());
  const std::string fillBefore = app.activePaintStyle().fill;
  constexpr ImVec2 kWidgetCursor(20.0f, 40.0f);
  ClickToolbar(window, shell, kWidgetCursor, ImVec2(57.0f, 76.0f));
  ASSERT_THAT(EditorShellTestAccess::ActivePaintTargetIsStroke(shell), testing::Eq(true));
  ClickToolbar(window, shell, kWidgetCursor, ImVec2(76.0f, 73.0f));
  EXPECT_THAT(app.activePaintStyle().stroke, testing::Eq("none"));
  EXPECT_THAT(app.activePaintStyle().fill, testing::Eq(fillBefore));
  EXPECT_THAT(app.selectedElements(), testing::SizeIs(2));
  EXPECT_THAT(std::string(document.source()), testing::Ne(before));
  EXPECT_THAT(std::string(*first->getAttribute("style")), testing::HasSubstr("stroke: none"));
  EXPECT_THAT(std::string(*second->getAttribute("style")), testing::HasSubstr("stroke: none"));
  ASSERT_THAT(app.undoTimeline().nextUndoLabel(),
              testing::Optional(testing::Eq("Set stroke to none")));
  app.undo();
  app.flushFrame();
  EXPECT_THAT(std::string(document.source()), testing::Eq(before));
  ASSERT_THAT(app.selectedElements(), testing::SizeIs(2));
  EXPECT_THAT(std::string(app.selectedElements()[0].id()), testing::Eq("first"));
  EXPECT_THAT(std::string(app.selectedElements()[1].id()), testing::Eq("second"));
  app.redo();
  app.flushFrame();
  EXPECT_THAT(std::string(document.source()), testing::Ne(before));
  EXPECT_THAT(EditorShellTestAccess::ActivePaintTargetIsStroke(shell), testing::Eq(true));
}

TEST(EditorShellTest, FillAndStrokeForegroundWidgetScreenshots) {
  gui::EditorWindow window(TestWindowOptions({
      .title = "Fill and Stroke foreground screenshots",
      .initialWidth = 640,
      .initialHeight = 480,
      .forceOffscreenRenderTarget = true,
      .enableFramebufferReadback = true,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  constexpr ImVec2 kWidgetCursor(20.0f, 40.0f);
  WriteEyedropperScreenshot(CapturePaintWidgetFrame(window, shell, kWidgetCursor),
                            "fill_stroke_fill_active.png");
  ClickToolbar(window, shell, kWidgetCursor, ImVec2(57.0f, 76.0f));
  ASSERT_THAT(EditorShellTestAccess::ActivePaintTargetIsStroke(shell), testing::Eq(true));
  WriteEyedropperScreenshot(CapturePaintWidgetFrame(window, shell, kWidgetCursor),
                            "fill_stroke_stroke_active.png");
}

TEST(EditorShellTest, SampledFillChangesSelectedStylesInOneUndoAndDefaultsNewText) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  constexpr std::string_view kSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
<rect id="first" x="2" y="2" width="20" height="20" fill="red"/>
<rect id="second" x="30" y="2" width="20" height="20" fill="blue"/>
</svg>)";
  EditorShell shell(window, OptionsWithSource(kSource));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  EditorApp& app = EditorShellTestAccess::App(shell);
  svg::SVGDocument& document = app.document().document();
  const auto first = document.querySelector("#first");
  const auto second = document.querySelector("#second");
  ASSERT_THAT(first, testing::Optional(testing::_));
  ASSERT_THAT(second, testing::Optional(testing::_));
  app.setSelection(std::vector<svg::SVGElement>{*first, *second});
  const std::string before(document.source());

  EditorShellTestAccess::ApplySampledColor(shell, /*stroke=*/false, css::RGBA(51, 102, 153, 128));
  EXPECT_THAT(app.activePaintStyle().fill, testing::Eq("#33669980"));
  EXPECT_THAT(app.selectedElements(), testing::SizeIs(2));
  EXPECT_THAT(std::string(*first->getAttribute("style")), testing::HasSubstr("fill: #33669980"));
  EXPECT_THAT(std::string(*second->getAttribute("style")), testing::HasSubstr("fill: #33669980"));
  ASSERT_THAT(app.undoTimeline().nextUndoLabel(), testing::Optional(testing::_));
  EXPECT_THAT(*app.undoTimeline().nextUndoLabel(), testing::Eq("Sample document color"));
  app.undo();
  app.flushFrame();
  EXPECT_THAT(std::string(document.source()), testing::Eq(before));
  ASSERT_THAT(app.selectedElements(), testing::SizeIs(2));
  EXPECT_THAT(std::string(app.selectedElements()[0].id()), testing::Eq("first"));
  EXPECT_THAT(std::string(app.selectedElements()[1].id()), testing::Eq("second"));
  app.redo();
  app.flushFrame();
  ASSERT_THAT(app.selectedElements(), testing::SizeIs(2));
  EXPECT_THAT(std::string(app.selectedElements()[0].id()), testing::Eq("first"));
  EXPECT_THAT(std::string(app.selectedElements()[1].id()), testing::Eq("second"));
}

TEST(EditorShellTest, StrokeTargetUpdatesAuthoringDefaultWithoutDocumentUndo) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  EditorShell shell(window, OptionsWithSource(kInitialSvg));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  const std::string before(shell.documentSourceForReadback().value_or(""));
  ASSERT_THAT(EditorShellTestAccess::ArmEyedropper(shell, /*stroke=*/true), testing::Eq(true));
  EXPECT_THAT(EditorShellTestAccess::EyedropperTargetsStroke(shell), testing::Eq(true));
  EditorShellTestAccess::ApplySampledColor(shell, /*stroke=*/true, css::RGBA(17, 34, 51, 255));
  EditorShellTestAccess::CancelEyedropper(shell, /*restorePreviousTool=*/true);
  EXPECT_THAT(EditorShellTestAccess::App(shell).activePaintStyle().stroke, testing::Eq("#112233"));
  EXPECT_THAT(shell.documentSourceForReadback(), testing::Optional(testing::Eq(before)));
  EXPECT_THAT(EditorShellTestAccess::App(shell).canUndo(), testing::Eq(false));
}

TEST(EditorShellTest, EyedropperSamplesDonnerTextAndShowsEdgeLoupe) {
  gui::EditorWindow window(TestWindowOptions({
      .title = "Eyedropper integration test",
      .initialWidth = 1600,
      .initialHeight = 900,
      .forceOffscreenRenderTarget = true,
      .enableFramebufferReadback = true,
  }));
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }
  constexpr std::string_view kTextSource =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="240" height="120">
<text id="donner" x="10" y="75" font-family="sans-serif" font-size="48"
 style="fill:#53c4f1">Donner</text></svg>)";
  EditorShell shell(window, OptionsWithSource(kTextSource));
  ASSERT_THAT(shell.valid(), testing::Eq(true));
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
  RunFrameWithMouse(window, shell, ImVec2(-1.0f, -1.0f), false);
  ASSERT_THAT(EditorShellTestAccess::ArmEyedropper(shell, /*stroke=*/false), testing::Eq(true));

  const DocumentPixelCapture* capture = WaitForDocumentPixelCapture(window, shell);
  ASSERT_NE(capture, nullptr) << "The composed worker readback never reached the live canvas.";

  const ViewportState& viewport = shell.viewportForReadback();
  const Vector2d textScreen = viewport.documentToScreen(Vector2d(18.0, 55.0));
  const std::optional<Vector2i> pixel =
      DocumentPixelIndexAtScreenPoint(viewport, capture->identity.rasterViewport, textScreen);
  ASSERT_THAT(pixel, testing::Optional(testing::_));
  const std::optional<css::RGBA> color = ReadDocumentPixel(capture->bitmap, *pixel);
  ASSERT_THAT(color, testing::Optional(testing::_));
  EXPECT_EQ(color->r, 0x53);
  EXPECT_EQ(color->g, 0xc4);
  EXPECT_EQ(color->b, 0xf1);
  EXPECT_GE(color->a, 240)
      << "The sampled glyph point should remain nearly opaque across GPU antialiasing variants";

  const Vector2d edgeScreen = viewport.documentToScreen(Vector2d(2.0, 2.0));
  const std::optional<Vector2i> edgePixel =
      DocumentPixelIndexAtScreenPoint(viewport, capture->identity.rasterViewport, edgeScreen);
  ASSERT_THAT(edgePixel, testing::Optional(testing::_));
  EXPECT_THAT(edgePixel->x, testing::Lt(5));
  EXPECT_THAT(edgePixel->y, testing::Lt(5));
  const svg::RendererBitmap screenshot = CaptureFrameWithMouse(
      window, shell, ImVec2(static_cast<float>(edgeScreen.x), static_cast<float>(edgeScreen.y)));
  WriteEyedropperScreenshot(screenshot, "eyedropper_loupe_document_edge.png");
  const ImVec2 textMouse(static_cast<float>(textScreen.x), static_cast<float>(textScreen.y));
  RunFrameWithMouse(window, shell, textMouse, false);
  RunFrameWithMouse(window, shell, textMouse, true);
  RunFrameWithMouse(window, shell, textMouse, false);
  EXPECT_THAT(EditorShellTestAccess::ActiveToolIsSelect(shell), testing::Eq(true));
  EXPECT_THAT(EditorShellTestAccess::App(shell).activePaintStyle().fill, testing::Eq("#53c4f1"));

  DriveGlobalShortcut(shell, {ImGuiKey_T});
  MouseModifiers modifiers;
  modifiers.doubleClick = true;
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(20.0, 108.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
      .modifiers = modifiers,
  });
  RunShellFrame(window, shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(20.0, 108.0),
      .leftMouseReleased = true,
  });
  RunShellFrame(window, shell);
  ImGui::GetIO().AddInputCharactersUTF8("SVG");
  RunShellFrame(window, shell);
  ASSERT_TRUE(shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(
      std::chrono::steady_clock::now() + std::chrono::seconds(3)));
  RunShellFrame(window, shell);
  ASSERT_THAT(EditorShellTestAccess::App(shell).selectedElements(), testing::SizeIs(1));
  const svg::SVGElement& newText = EditorShellTestAccess::App(shell).selectedElements().front();
  EXPECT_THAT(newText.type(), testing::Eq(svg::ElementType::Text));
  EXPECT_THAT(std::string(newText.id()), testing::Ne("donner"));
  const std::string newTextContent =
      newText.withReadAccess([&newText](svg::DocumentReadAccess&, EntityHandle) {
        return std::string(newText.cast<svg::SVGTextElement>().textContent());
      });
  EXPECT_THAT(newTextContent, testing::Eq("SVG"));
  ASSERT_THAT(newText.getAttribute("style"), testing::Optional(testing::_));
  EXPECT_THAT(std::string(*newText.getAttribute("style")), testing::HasSubstr("fill: #53c4f1"));
  const std::optional<svg::SVGElement> originalDonner =
      EditorShellTestAccess::App(shell).document().document().querySelector("#donner");
  ASSERT_THAT(originalDonner, testing::Optional(testing::_));
  const std::string originalContent =
      originalDonner->withReadAccess([&originalDonner](svg::DocumentReadAccess&, EntityHandle) {
        return std::string(originalDonner->cast<svg::SVGTextElement>().textContent());
      });
  EXPECT_THAT(originalContent, testing::Eq("Donner"));
}

namespace {

/// Source with enough lines before and after a resource that a focus partition computed against
/// stale line numbers lands on unrelated markup.
std::string SourceFocusInsertionSvg() {
  std::string source =
      "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"240\" height=\"160\">\n"
      "<defs>\n";
  for (int i = 0; i < 6; ++i) {
    source += "<linearGradient id=\"gradient-" + std::to_string(i) + "\">\n";
    source += "<stop offset=\"0\" stop-color=\"red\"/>\n";
    source += "<stop offset=\"1\" stop-color=\"blue\"/>\n";
    source += "</linearGradient>\n";
  }
  source += "</defs>\n";
  for (int i = 0; i < 24; ++i) {
    source += "<rect id=\"filler-" + std::to_string(i) + "\" x=\"" + std::to_string(200 + i) +
              "\" y=\"150\" width=\"1\" height=\"1\"/>\n";
  }
  source += "</svg>\n";
  return source;
}

int LineAtOffset(std::string_view text, std::size_t offset) {
  return static_cast<int>(std::count(text.begin(), text.begin() + offset, '\n'));
}

/// Describe which source lines the active focus partition keeps visible.
std::string DescribeFocusLines(const TextEditor& source) {
  const std::string text = source.getText();
  std::string description;
  const int lineCount = LineAtOffset(text, text.size()) + 1;
  std::size_t lineStart = 0;
  for (int line = 0; line < lineCount; ++line) {
    const std::size_t lineEnd = std::min(text.find('\n', lineStart), text.size());
    description +=
        source.isPositionInsideFocusRange(Coordinates(line, 0)) ? "  shown " : " folded ";
    description += std::to_string(line) + ": " + text.substr(lineStart, lineEnd - lineStart);
    description += "\n";
    lineStart = lineEnd + 1;
  }
  return description;
}

/// Run frames until the async renderer is idle and every queued writeback reached the source pane.
void SettleShellFrames(gui::EditorWindow& window, EditorShell& shell) {
  const tests::HangGuard guard;
  for (int attempt = 0; attempt < 6; ++attempt) {
    RunShellFrame(window, shell);
    ASSERT_TRUE(
        shell.asyncRendererForReplay().waitUntilNoRenderInFlightForTesting(guard.deadline()));
  }
  RunShellFrame(window, shell);
}

/// Expect the focused source view to show the whole selected element, and to fold unrelated
/// filler markup.
void ExpectFocusShowsSelectedElement(EditorShell& shell, std::string_view contentMarker,
                                     std::string_view closeTag) {
  EditorApp& app = EditorShellTestAccess::App(shell);
  const TextEditor& source = EditorShellTestAccess::Source(shell);
  const std::string text = source.getText();
  ASSERT_EQ(text, internal::CanonicalizeForTextEditor(app.document().document().source()))
      << "the source pane must mirror the document before focus is checked";
  ASSERT_TRUE(EditorShellTestAccess::SourceFocusMode(shell));
  ASSERT_TRUE(source.hasFocusPartition()) << DescribeFocusLines(source);

  const std::size_t markerOffset = text.find(contentMarker);
  ASSERT_NE(markerOffset, std::string::npos) << text;
  const std::size_t openOffset = text.rfind('<', markerOffset);
  ASSERT_NE(openOffset, std::string::npos);
  // An empty element may still be self-closing; its end is whichever close comes first.
  const std::size_t closeOffset =
      std::min(text.find(closeTag, markerOffset), text.find("/>", markerOffset));
  ASSERT_NE(closeOffset, std::string::npos) << text;
  const int openLine = LineAtOffset(text, openOffset);
  const int closeLine = LineAtOffset(text, closeOffset);
  for (int line = openLine; line <= closeLine; ++line) {
    EXPECT_TRUE(source.isPositionInsideFocusRange(Coordinates(line, 0)))
        << "selected element line " << line << " is folded away\n"
        << DescribeFocusLines(source);
  }

  const std::size_t fillerOffset = text.find("id=\"filler-12\"");
  ASSERT_NE(fillerOffset, std::string::npos);
  EXPECT_FALSE(source.isPositionInsideFocusRange(Coordinates(LineAtOffset(text, fillerOffset), 0)))
      << "unrelated markup should stay folded\n"
      << DescribeFocusLines(source);
}

}  // namespace

TEST(EditorShellTest, SourceFocusRevealsNewTextWhileCreatingAndTyping) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(SourceFocusInsertionSvg(), "focus-new-text.svg"));
  ASSERT_TRUE(shell.valid());
  ASSERT_TRUE(EditorShellTestAccess::SourceFocusMode(shell));
  SettleShellFrames(window, shell);

  DriveGlobalShortcut(shell, {ImGuiKey_T});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsText(shell));
  MouseModifiers doubleClick;
  doubleClick.doubleClick = true;
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(20.0, 60.0),
      .leftMouseDown = true,
      .leftMousePressed = true,
      .modifiers = doubleClick,
  });
  RunShellFrame(window, shell);
  shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
      .documentPoint = Vector2d(20.0, 60.0),
      .leftMouseReleased = true,
  });
  SettleShellFrames(window, shell);

  ASSERT_TRUE(EditorShellTestAccess::TextToolIsEditing(shell));
  ASSERT_THAT(EditorShellTestAccess::App(shell).selectedElements(), testing::SizeIs(1));
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElements().front().type(),
            svg::ElementType::Text);
  {
    SCOPED_TRACE("after creating the text element");
    ExpectFocusShowsSelectedElement(shell, "x=\"20\" y=\"60\"", "</text>");
  }

  for (const char* typed : {"S", "V", "G"}) {
    ImGui::GetIO().AddInputCharactersUTF8(typed);
    SettleShellFrames(window, shell);
    SCOPED_TRACE(std::string("after typing ") + typed);
    ExpectFocusShowsSelectedElement(shell, "x=\"20\" y=\"60\"", "</text>");
  }
  EXPECT_NE(EditorShellTestAccess::Source(shell).getText().find("SVG</text>"), std::string::npos);
}

TEST(EditorShellTest, DisablingSourceFocusClearsPendingEditPartitionImmediately) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "Hidden editor window is unavailable on this host";
  }

  for (const bool flushBeforeDisable : {false, true}) {
    SCOPED_TRACE(flushBeforeDisable ? "writeback awaiting source pane" : "queued insertion");
    EditorShell shell(window, OptionsWithSource(kInitialSvg, "focus-disable.svg"));
    ASSERT_THAT(shell.valid(), testing::Eq(true));
    EditorApp& app = EditorShellTestAccess::App(shell);
    auto target = app.document().document().querySelector("#target");
    ASSERT_THAT(target, testing::Optional(testing::_));
    app.setSelection(*target);
    EditorShellTestAccess::SetSourceFocusMode(shell, true);
    TextEditor& source = EditorShellTestAccess::Source(shell);
    ASSERT_THAT(source.hasFocusPartition(), testing::Eq(true));

    svg::SVGDocument& document = app.document().document();
    svg::SVGRectElement inserted = svg::SVGRectElement::Create(document);
    inserted.setAttribute("id", "inserted");
    app.applyMutation(EditorCommand::InsertElementCommand(document.svgElement(), inserted));
    if (flushBeforeDisable) {
      ASSERT_THAT(app.flushFrame(), testing::Eq(true));
      ASSERT_THAT(app.document().hasUnmirroredSourceDeltas(), testing::Eq(true));
    } else {
      ASSERT_THAT(app.document().hasPendingMutations(), testing::Eq(true));
    }
    const std::string beforeText = source.getText();
    const auto beforeVersion = app.document().currentFrameVersion();
    const bool beforePending = app.document().hasPendingMutations();
    const bool beforeUnmirrored = app.document().hasUnmirroredSourceDeltas();

    EditorShellTestAccess::SetSourceFocusMode(shell, false);
    EXPECT_THAT(EditorShellTestAccess::SourceFocusMode(shell), testing::Eq(false));
    EXPECT_THAT(source.hasFocusPartition(), testing::Eq(false));
    EXPECT_THAT(source.getText(), testing::Eq(beforeText));
    EXPECT_THAT(app.document().currentFrameVersion(), testing::Eq(beforeVersion));
    EXPECT_THAT(app.document().hasPendingMutations(), testing::Eq(beforePending));
    EXPECT_THAT(app.document().hasUnmirroredSourceDeltas(), testing::Eq(beforeUnmirrored));
  }
}

TEST(EditorShellTest, SourceFocusWaitsForInsertedElementToReachSourcePane) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  // A tool inserts and selects an element, then the selection is highlighted before the source
  // pane has the element: either the insertion is still queued (a busy renderer defers the flush),
  // or it is flushed and its source writeback waits for an idle frame.
  for (const bool flushBeforeHighlight : {false, true}) {
    SCOPED_TRACE(flushBeforeHighlight ? "flushed, writeback pending" : "insertion still queued");
    EditorShell shell(window, OptionsWithSource(SourceFocusInsertionSvg(), "focus-insert.svg"));
    ASSERT_TRUE(shell.valid());
    SettleShellFrames(window, shell);

    EditorApp& app = EditorShellTestAccess::App(shell);
    svg::SVGDocument& document = app.document().document();
    svg::SVGRectElement inserted = svg::SVGRectElement::Create(document);
    inserted.setAttribute("id", "inserted");
    inserted.setAttribute("width", "5");
    inserted.setAttribute("height", "5");
    app.applyMutation(EditorCommand::InsertElementCommand(document.svgElement(), inserted));
    app.setSelection(inserted);
    if (flushBeforeHighlight) {
      ASSERT_TRUE(app.flushFrame());
    }
    EditorShellTestAccess::HighlightSelectionSourceIfNeeded(shell);

    SettleShellFrames(window, shell);
    ExpectFocusShowsSelectedElement(shell, "id=\"inserted\"", "/>");
  }
}

TEST(EditorShellTest, SourceFocusRevealsNewPenPath) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(SourceFocusInsertionSvg(), "focus-pen.svg"));
  ASSERT_TRUE(shell.valid());
  SettleShellFrames(window, shell);

  DriveGlobalShortcut(shell, {ImGuiKey_P});
  ASSERT_TRUE(EditorShellTestAccess::ActiveToolIsPen(shell));
  for (const Vector2d& point : {Vector2d(20.0, 60.0), Vector2d(100.0, 60.0)}) {
    shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
        .documentPoint = point,
        .leftMouseDown = true,
        .leftMousePressed = true,
    });
    RunShellFrame(window, shell);
    shell.queueDocumentSpaceReplayInputForTesting(EditorShellDocumentReplayInput{
        .documentPoint = point,
        .leftMouseReleased = true,
    });
    SettleShellFrames(window, shell);
  }

  ASSERT_THAT(EditorShellTestAccess::App(shell).selectedElements(), testing::SizeIs(1));
  EXPECT_EQ(EditorShellTestAccess::App(shell).selectedElements().front().type(),
            svg::ElementType::Path);
  ExpectFocusShowsSelectedElement(shell, "<path", "/>");
}

TEST(EditorShellTest, SourceFocusRevealsPastedShape) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(SourceFocusInsertionSvg(), "focus-paste.svg"));
  ASSERT_TRUE(shell.valid());
  EditorShellTestAccess::UseInMemoryShapeClipboard(shell);
  SettleShellFrames(window, shell);

  EditorApp& app = EditorShellTestAccess::App(shell);
  std::optional<svg::SVGElement> source = app.document().document().querySelector("#filler-3");
  ASSERT_TRUE(source.has_value());
  app.setSelection(*source);
  SettleShellFrames(window, shell);
  EditorShellTestAccess::CopySelectedShapesToClipboard(shell);
  ASSERT_TRUE(EditorShellTestAccess::ShapeClipboardHasText(shell));

  EditorShellTestAccess::PasteShapesFromClipboard(shell, /*inFront=*/false);
  SettleShellFrames(window, shell);

  ASSERT_THAT(app.selectedElements(), testing::SizeIs(1));
  EXPECT_EQ(std::string(app.selectedElements().front().id()), "filler-3_pasted");
  ExpectFocusShowsSelectedElement(shell, "id=\"filler-3_pasted\"", "/>");
}

TEST(EditorShellTest, SourceFocusFollowsSelectedElementMovedByDocumentEdit) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  // Each edit moves the selected element's source lines without changing the selection.
  struct SourceMovingEdit {
    const char* name;
    std::function<void(EditorShell&)> apply;
  };
  const std::vector<SourceMovingEdit> edits = {
      {"Bring to Front shortcut",
       [](EditorShell& shell) {
         DriveGlobalShortcut(shell, {ImGuiKey_RightBracket}, /*ctrl=*/true, /*shift=*/true);
       }},
      {"Send Backward shortcut",
       [](EditorShell& shell) {
         DriveGlobalShortcut(shell, {ImGuiKey_LeftBracket}, /*ctrl=*/true);
       }},
      {"sibling above deleted",
       [](EditorShell& shell) {
         EditorApp& app = EditorShellTestAccess::App(shell);
         std::optional<svg::SVGElement> sibling =
             app.document().document().querySelector("#filler-1");
         ASSERT_THAT(sibling, testing::Optional(testing::_));
         app.applyMutation(EditorCommand::DeleteElementCommand(*sibling));
       }},
  };

  for (const SourceMovingEdit& edit : edits) {
    SCOPED_TRACE(edit.name);
    EditorShell shell(window, OptionsWithSource(SourceFocusInsertionSvg(), "focus-move.svg"));
    ASSERT_TRUE(shell.valid());
    SettleShellFrames(window, shell);

    EditorApp& app = EditorShellTestAccess::App(shell);
    std::optional<svg::SVGElement> target = app.document().document().querySelector("#filler-3");
    ASSERT_THAT(target, testing::Optional(testing::_));
    app.setSelection(*target);
    SettleShellFrames(window, shell);
    {
      SCOPED_TRACE("before the edit");
      ExpectFocusShowsSelectedElement(shell, "id=\"filler-3\"", "/>");
    }

    const std::string sourceBefore(app.document().document().source());
    edit.apply(shell);
    SettleShellFrames(window, shell);

    ASSERT_NE(std::string(app.document().document().source()), sourceBefore);
    ASSERT_THAT(app.selectedElements(), testing::ElementsAre(*target));
    ExpectFocusShowsSelectedElement(shell, "id=\"filler-3\"", "/>");
    EXPECT_THAT(
        EditorShellTestAccess::Source(shell).getSelectedText(),
        testing::AllOf(testing::StartsWith("<rect id=\"filler-3\""), testing::EndsWith("/>")))
        << "the source selection highlight should cover the moved element";
  }
}

TEST(EditorShellTest, SourceFocusFollowsZOrderAfterTypingThatChangedNothing) {
  gui::EditorWindow window = MakeHiddenWindow();
  if (!window.valid()) {
    GTEST_SKIP() << "GL-backed hidden editor window is unavailable on this host";
  }

  EditorShell shell(window, OptionsWithSource(SourceFocusInsertionSvg(), "focus-retype.svg"));
  ASSERT_TRUE(shell.valid());
  SettleShellFrames(window, shell);

  // Retype a character in the source pane: the pane reports typed text, but the document source
  // does not change, so the typed edit must not suppress a later document edit's focus scroll.
  TextEditor& source = EditorShellTestAccess::Source(shell);
  const std::size_t retypedOffset = source.getText().find("<rect id=\"filler-1\"");
  ASSERT_NE(retypedOffset, std::string::npos);
  source.setSelection(source.getCoordinatesAtByteOffset(retypedOffset),
                      source.getCoordinatesAtByteOffset(retypedOffset + 1u));
  source.resetTextChanged();
  source.insertText("<");
  ASSERT_TRUE(source.isTextChanged());
  window.beginFrame();
  ImGuiIO& io = ImGui::GetIO();
  if (!io.Fonts->IsBuilt()) {
    io.Fonts->Build();
  }
  EditorShellTestAccess::RenderSourcePane(shell, /*paneOriginY=*/0.0f, /*paneHeight=*/180.0f,
                                          /*paneWidth=*/260.0f, io.Fonts->Fonts[0]);
  window.endFrame();
  SettleShellFrames(window, shell);

  EditorApp& app = EditorShellTestAccess::App(shell);
  std::optional<svg::SVGElement> target = app.document().document().querySelector("#filler-3");
  ASSERT_THAT(target, testing::Optional(testing::_));
  app.setSelection(*target);
  SettleShellFrames(window, shell);
  DriveGlobalShortcut(shell, {ImGuiKey_RightBracket}, /*ctrl=*/true, /*shift=*/true);
  SettleShellFrames(window, shell);

  ExpectFocusShowsSelectedElement(shell, "id=\"filler-3\"", "/>");
  EXPECT_THAT(source.getSelectedText(),
              testing::AllOf(testing::StartsWith("<rect id=\"filler-3\""), testing::EndsWith("/>")))
      << "the source selection highlight should follow the moved element";
}
}  // namespace donner::editor
