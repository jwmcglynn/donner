#include "donner/editor/DisclosureChevron.h"

#include <cmath>

#include "donner/editor/EmbeddedSvgIcon.h"
#include "embed_resources/EditorIcons.h"

namespace donner::editor {

const std::optional<svg::RendererBitmap>& CachedDisclosureChevronBitmap() {
  static const std::optional<svg::RendererBitmap> bitmap =
      RenderEmbeddedSvgIcon(embedded::kEditorChevronSvg, kDisclosureChevronRasterSizePx);
  return bitmap;
}

int DisclosureChevronQuarterTurns(bool expanded) { return expanded ? 1 : 0; }

void DrawDisclosureChevron(ImDrawList* drawList, ImTextureID texture, const Vector2d& uvBottomRight,
                           const ImVec2& center, float sizePx, bool expanded, ImU32 tint) {
  if (texture == 0) {
    return;
  }

  const float half = sizePx * 0.5f;
  // Square corners, clockwise from top-left, centered on the origin.
  const ImVec2 base[4] = {
      ImVec2(-half, -half),
      ImVec2(half, -half),
      ImVec2(half, half),
      ImVec2(-half, half),
  };

  // Screen space is y-down, so a positive angle rotates clockwise; one quarter
  // turn takes the right-pointing chevron to point down for the expanded state.
  const float angle = static_cast<float>(DisclosureChevronQuarterTurns(expanded)) * 1.57079632679f;
  const float ca = std::cos(angle);
  const float sa = std::sin(angle);
  ImVec2 p[4];
  for (int i = 0; i < 4; ++i) {
    p[i] = ImVec2(center.x + base[i].x * ca - base[i].y * sa,
                  center.y + base[i].x * sa + base[i].y * ca);
  }

  const float u = static_cast<float>(uvBottomRight.x);
  const float v = static_cast<float>(uvBottomRight.y);
  drawList->AddImageQuad(texture, p[0], p[1], p[2], p[3], ImVec2(0.0f, 0.0f), ImVec2(u, 0.0f),
                         ImVec2(u, v), ImVec2(0.0f, v), tint);
}

}  // namespace donner::editor
