/// @file
/// Regression coverage for the offscreen (surface-less) arm of the editor's
/// WebGPU frame path.
///
/// `EditorWindow::endFrameImpl` renders into one of two targets: the
/// presentable window surface, or - when there is no surface - an offscreen
/// texture. Only the second arm is reachable headlessly, and until now only on
/// Linux, where `useNullPlatform` engages because GLFW is on its windowless
/// "null" platform. macOS always builds a real (if hidden) Cocoa surface, and
/// the default CI renderer backend is tiny_skia, which compiles the WebGPU path
/// out entirely. That combination let a broken offscreen guard (for example
/// checking only `!surface`, which bails out of every offscreen frame before
/// anything is drawn) survive on every lane but one.
///
/// `EditorWindowOptions::forceOffscreenRenderTarget` makes the arm reachable on
/// any platform, and this file lives in its own test target with a `geode`
/// variant so `bazel test //...` executes it on both the macOS and Linux lanes
/// without a `--config=` flag.

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>

#include "donner/editor/gui/EditorWindow.h"

#if defined(DONNER_EDITOR_WGPU)
#include "donner/svg/renderer/tests/RgbaTestMatchers.h"
#endif

namespace donner::editor::gui {
namespace {

#if defined(DONNER_EDITOR_WGPU)
using svg::test::Near;
using svg::test::Rgba;

std::array<std::uint8_t, 4> PixelAt(const svg::RendererBitmap& bitmap, int x, int y) {
  const std::size_t offset =
      static_cast<std::size_t>(y) * bitmap.rowBytes + static_cast<std::size_t>(x) * 4u;
  return {bitmap.pixels[offset], bitmap.pixels[offset + 1], bitmap.pixels[offset + 2],
          bitmap.pixels[offset + 3]};
}
#endif

}  // namespace

// Guards the default in non-WebGPU builds too, so the tiny_skia configuration
// of this target still runs a case and the option cannot silently start
// forcing offscreen rendering in the shipped editor.
TEST(EditorWindowOffscreenTargetTest, ForceOffscreenRenderTargetDefaultsOff) {
  EXPECT_FALSE(EditorWindowOptions{}.forceOffscreenRenderTarget);
}

#if defined(DONNER_EDITOR_WGPU)

TEST(EditorWindowOffscreenTargetTest, ForcedOffscreenTargetRendersAndReadsBackOnEveryPlatform) {
  EditorWindow window(EditorWindowOptions{
      .title = "Forced Offscreen WGPU Readback Test",
      .initialWidth = 64,
      .initialHeight = 48,
      .visible = false,
      .forceOffscreenRenderTarget = true,
      .clearColor = {0.0f, 0.0f, 1.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
#if defined(__linux__)
  // Linux always has a software Vulkan adapter (lavapipe) available, so an
  // unusable window here is a real regression, not a host capability gap.
  ASSERT_TRUE(window.valid());
#else
  if (!window.valid()) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }
#endif
  ASSERT_NE(window.geodeFramebufferDevice(), nullptr);

  // The window is backed by an offscreen texture rather than a presentable
  // surface, which is exactly the configuration the offscreen arm handles.
  ASSERT_TRUE(window.usingOffscreenRenderTarget());

  const Vector2i framebufferSize = window.framebufferSize();
  ASSERT_GT(framebufferSize.x, 0);
  ASSERT_GT(framebufferSize.y, 0);

  window.beginFrame();
  const svg::RendererBitmap frame = window.endFrameAndReadPixels();

  // A frame that bailed out before the render pass reads back empty.
  ASSERT_FALSE(frame.empty())
      << "endFrameImpl produced no offscreen frame; the offscreen render target was most likely "
         "rejected before the render pass";
  EXPECT_EQ(frame.dimensions, framebufferSize);
  EXPECT_EQ(frame.rowBytes, static_cast<std::size_t>(framebufferSize.x) * 4u);

  // Opaque blue, the configured clear color, proves the frame was cleared and
  // copied out of the offscreen texture rather than left zero-initialized.
  EXPECT_THAT(PixelAt(frame, 8, 8),
              Rgba(testing::Le(3), testing::Le(3), Near(255, 3), testing::Eq(255)));
  EXPECT_THAT(PixelAt(frame, framebufferSize.x - 1, framebufferSize.y - 1),
              Rgba(testing::Le(3), testing::Le(3), Near(255, 3), testing::Eq(255)));
}

// Two consecutive frames from the same window: the offscreen arm must keep
// rendering into the texture it already created rather than dropping the
// target after the first present-less frame.
TEST(EditorWindowOffscreenTargetTest, ForcedOffscreenTargetKeepsRenderingAcrossFrames) {
  EditorWindow window(EditorWindowOptions{
      .title = "Forced Offscreen WGPU Repeat Frame Test",
      .initialWidth = 64,
      .initialHeight = 48,
      .visible = false,
      .forceOffscreenRenderTarget = true,
      .clearColor = {0.0f, 1.0f, 0.0f, 1.0f},
      .enableFramebufferReadback = true,
  });
#if defined(__linux__)
  ASSERT_TRUE(window.valid());
#else
  if (!window.valid()) {
    GTEST_SKIP() << "WebGPU editor window is unavailable on this host";
  }
#endif
  ASSERT_TRUE(window.usingOffscreenRenderTarget());

  window.beginFrame();
  const svg::RendererBitmap first = window.endFrameAndReadPixels();
  ASSERT_FALSE(first.empty());

  window.beginFrame();
  const svg::RendererBitmap second = window.endFrameAndReadPixels();
  ASSERT_FALSE(second.empty()) << "the offscreen target did not survive the first frame";
  EXPECT_EQ(second.dimensions, first.dimensions);
  EXPECT_TRUE(window.usingOffscreenRenderTarget());
  EXPECT_THAT(PixelAt(second, second.dimensions.x - 1, second.dimensions.y - 1),
              Rgba(testing::Le(3), Near(255, 3), testing::Le(3), testing::Eq(255)));
}

#endif  // defined(DONNER_EDITOR_WGPU)

}  // namespace donner::editor::gui
