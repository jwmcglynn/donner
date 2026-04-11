#include "donner/editor/OverlayRenderer.h"

#include "donner/editor/EditorApp.h"
#include "donner/svg/renderer/Renderer.h"
#include "gtest/gtest.h"

namespace donner::editor {
namespace {

constexpr std::string_view kTrivialSvg =
    R"(<svg xmlns="http://www.w3.org/2000/svg" width="200" height="200">
         <rect id="r1" x="20" y="30" width="40" height="50" fill="red"/>
       </svg>)";

// OverlayRenderer is hard to unit-test in isolation because the canvas
// primitives end up in a Skia/TinySkia frame buffer that we don't read
// back at the unit-test layer. The test plan in `editor.md` calls these
// out as belonging to the framebuffer-golden tier (M4).
//
// What we *can* unit-test cheaply: that calling `drawChrome` does not
// crash on any combination of (no document / document but no selection /
// document with valid selection / document with stale selection
// referencing a since-deleted entity). The full visual verification
// happens in the example viewer and in the future M4 framebuffer
// golden tests.

TEST(OverlayRendererTest, NoOpWithoutDocument) {
  EditorApp app;
  svg::Renderer renderer;
  // No document loaded → drawChrome must early-out before touching the
  // renderer. The test passes if this doesn't crash.
  OverlayRenderer::drawChrome(renderer, app);
  SUCCEED();
}

TEST(OverlayRendererTest, NoOpWithoutSelection) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  svg::Renderer renderer;
  renderer.draw(app.document().document());
  // Selection is empty by default → drawChrome should be a no-op.
  OverlayRenderer::drawChrome(renderer, app);
  SUCCEED();
}

TEST(OverlayRendererTest, EmitsChromeForSelectedElement) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  svg::Renderer renderer;
  renderer.draw(app.document().document());
  OverlayRenderer::drawChrome(renderer, app);

  // The renderer should have produced a non-empty bitmap that includes
  // both the document fill and the chrome stroke. We can't easily diff
  // pixels here without going to the framebuffer-golden tier, but we
  // can at least confirm the renderer produced *something*.
  const auto bitmap = renderer.takeSnapshot();
  EXPECT_FALSE(bitmap.empty());
  EXPECT_GT(bitmap.dimensions.x, 0);
  EXPECT_GT(bitmap.dimensions.y, 0);
}

TEST(OverlayRendererTest, ToleratesStaleSelectionAfterReload) {
  EditorApp app;
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));

  auto rect = app.document().document().querySelector("#r1");
  ASSERT_TRUE(rect.has_value());
  app.setSelection(*rect);

  // Reload the document — the previously-selected entity is now stale.
  // EditorApp::loadFromString clears the selection, so this case is
  // handled by the public API; test it explicitly to lock in the
  // contract.
  ASSERT_TRUE(app.loadFromString(kTrivialSvg));
  EXPECT_FALSE(app.hasSelection());

  svg::Renderer renderer;
  renderer.draw(app.document().document());
  OverlayRenderer::drawChrome(renderer, app);
  SUCCEED();
}

}  // namespace
}  // namespace donner::editor
