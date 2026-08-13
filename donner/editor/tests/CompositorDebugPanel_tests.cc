/// @file
/// Headless-ImGui tests for the Compositor Debug panel's layout.

#include "donner/editor/CompositorDebugPanel.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "donner/editor/ImGuiIncludes.h"
#include "donner/editor/ImGuiInternalIncludes.h"

namespace donner::editor {
namespace {

using CompositeTileSnapshot = svg::compositor::CompositorController::CompositeTileSnapshot;

constexpr const char* kHostWindowName = "##compositor_debug_host";

/// Metadata-only tiles: no thumbnail pixels and no texture snapshot, so the
/// panel's rows never touch a GL/WebGPU upload and the test stays headless.
std::vector<CompositeTileSnapshot> MakeTiles(int count) {
  std::vector<CompositeTileSnapshot> tiles;
  tiles.reserve(static_cast<std::size_t>(count));
  for (int i = 0; i < count; ++i) {
    CompositeTileSnapshot tile;
    tile.kind = CompositeTileSnapshot::Kind::Segment;
    tile.id = "seg:" + std::to_string(i);
    tile.label = "segment " + std::to_string(i);
    tile.generation = static_cast<std::uint64_t>(i + 1);
    tile.lastRasterizeMs = 1.5;
    tiles.push_back(std::move(tile));
  }
  return tiles;
}

class CompositorDebugPanelImGuiTest : public ::testing::Test {
protected:
  /// Panel host smaller than the panel's diagnostic header, which is the
  /// configuration the user hit: the header alone overflows the window.
  static constexpr float kHostWidth = 520.0f;
  static constexpr float kHostHeight = 200.0f;

  void SetUp() override {
    IMGUI_CHECKVERSION();
    context_ = ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(kHostWidth, kHostHeight);
    io.DeltaTime = 1.0f / 60.0f;
    io.Fonts->Build();
  }

  void TearDown() override {
    ImGui::DestroyContext(context_);
    context_ = nullptr;
  }

  /// Render one panel frame with `tileCount` tiles and return the host
  /// window's laid-out content height - the extent the window's own scrollbar
  /// can reach. Content the window cannot account for is content the user
  /// cannot scroll to.
  float contentHeightForTiles(int tileCount) {
    const std::vector<CompositeTileSnapshot> tiles = MakeTiles(tileCount);
    // ImGui publishes `ContentSize` at the end of the frame that laid it out,
    // and a table needs one settling frame before its columns are final, so
    // measure a repeated frame rather than the first one.
    float contentHeight = 0.0f;
    for (int frame = 0; frame < 3; ++frame) {
      ImGuiIO& io = ImGui::GetIO();
      io.DisplaySize = ImVec2(kHostWidth, kHostHeight);
      ImGui::NewFrame();
      ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f), ImGuiCond_Always);
      ImGui::SetNextWindowSize(ImVec2(kHostWidth, kHostHeight), ImGuiCond_Always);
      ImGui::Begin(kHostWindowName, nullptr,
                   ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                       ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
      panel_.render(tiles, svg::compositor::CompositorController::StateSnapshot{}, entt::null,
                    /*viewportZoom=*/1.0, /*viewportDpr=*/1.0, Vector2i(800, 600),
                    Vector2i(800, 600), PresentationCoverageDiagnostics{},
                    svg::compositor::CompositorController::FastPathCounters{},
                    svg::compositor::CompositorController::RenderFrameStats{});
      ImGui::End();
      ImGui::Render();

      const ImGuiWindow* window = ImGui::FindWindowByName(kHostWindowName);
      contentHeight = window != nullptr ? window->ContentSize.y : 0.0f;
    }
    return contentHeight;
  }

  CompositorDebugPanel panel_;

private:
  ImGuiContext* context_ = nullptr;
};

// Every composite-tile row has to be reachable by scrolling the panel, at any
// panel size. The regression: the tile table opened its own `_ScrollY` child
// sized to whatever vertical space was left after the diagnostics header. In a
// short panel that remainder is zero, so the table collapsed to a sliver whose
// height no longer depended on the row count - the user scrolled to the
// bottom of the panel and the tile table simply was not there. Growing the row
// count must grow what the panel's own scrollbar can reach.
TEST_F(CompositorDebugPanelImGuiTest, TileRowsExtendTheScrollableContentOfAShortPanel) {
  const float twoTiles = contentHeightForTiles(2);
  const float twelveTiles = contentHeightForTiles(12);

  EXPECT_GT(twelveTiles - twoTiles, 100.0f)
      << "ten extra tile rows only grew the panel's scrollable content from " << twoTiles << " to "
      << twelveTiles << " px; the tile table is not reachable in a " << kHostHeight
      << " px tall panel";
}

}  // namespace
}  // namespace donner::editor
