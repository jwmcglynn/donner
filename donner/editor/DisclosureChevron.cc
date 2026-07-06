#include "donner/editor/DisclosureChevron.h"

#include "donner/editor/EmbeddedSvgIcon.h"
#include "embed_resources/EditorIcons.h"

namespace donner::editor {

const std::optional<svg::RendererBitmap>& CachedDisclosureChevronBitmap(bool expanded) {
  static const std::optional<svg::RendererBitmap> collapsed =
      RenderEmbeddedSvgIcon(embedded::kEditorChevronSvg, kDisclosureChevronRasterSizePx);
  static const std::optional<svg::RendererBitmap> expandedBitmap =
      RenderEmbeddedSvgIcon(embedded::kEditorChevronDownSvg, kDisclosureChevronRasterSizePx);
  return expanded ? expandedBitmap : collapsed;
}

int DisclosureChevronTextureVariant(bool expanded) { return expanded ? 1 : 0; }

void DrawDisclosureChevron(ImDrawList* drawList, ImTextureID texture, const Vector2d& uvBottomRight,
                           const ImVec2& center, float sizePx, ImU32 tint) {
  if (texture == 0) {
    return;
  }

  const float half = sizePx * 0.5f;
  const ImVec2 topLeft(center.x - half, center.y - half);
  const ImVec2 bottomRight(center.x + half, center.y + half);
  const ImVec2 uvTopLeft(0.0f, 0.0f);
  const ImVec2 uvBottomRightVec(static_cast<float>(uvBottomRight.x),
                                static_cast<float>(uvBottomRight.y));
  drawList->AddImage(texture, topLeft, bottomRight, uvTopLeft, uvBottomRightVec, tint);
}

}  // namespace donner::editor
