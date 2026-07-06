#include "donner/editor/TextChipIconSet.h"

#include "donner/editor/EmbeddedSvgIcon.h"
#include "embed_resources/EditorIcons.h"

namespace donner::editor {
namespace {

/// Raster size for chip icon masks. Chips draw at roughly the text line height
/// (~14-20px); a 64px raster stays crisp through downscale at 1x and 2x DPR.
constexpr int kTextChipIconRasterSizePx = 64;

}  // namespace

std::span<const unsigned char> TextChipIconSvg(TextChipIcon icon) {
  switch (icon) {
    case TextChipIcon::StyleSource: return embedded::kEditorChipStyleSourceSvg;
    case TextChipIcon::Overflow: return embedded::kEditorChipOverflowSvg;
  }
  return embedded::kEditorChipStyleSourceSvg;
}

std::uint64_t TextChipIconTextureKey(TextChipIcon icon) {
  constexpr std::uint64_t kChipTextureKeyBase = 0xf800000000000000ull;
  switch (icon) {
    case TextChipIcon::StyleSource: return kChipTextureKeyBase + 1u;
    case TextChipIcon::Overflow: return kChipTextureKeyBase + 2u;
  }
  return kChipTextureKeyBase;
}

const std::optional<svg::RendererBitmap>& CachedTextChipIconBitmap(TextChipIcon icon) {
  switch (icon) {
    case TextChipIcon::StyleSource: {
      static const std::optional<svg::RendererBitmap> bitmap = RenderEmbeddedSvgIcon(
          TextChipIconSvg(TextChipIcon::StyleSource), kTextChipIconRasterSizePx);
      return bitmap;
    }
    case TextChipIcon::Overflow: {
      static const std::optional<svg::RendererBitmap> bitmap =
          RenderEmbeddedSvgIcon(TextChipIconSvg(TextChipIcon::Overflow), kTextChipIconRasterSizePx);
      return bitmap;
    }
  }

  static const std::optional<svg::RendererBitmap> empty;
  return empty;
}

bool DrawTextChipIcon(ImDrawList* drawList, TextChipIcon icon, const ImVec2& min, const ImVec2& max,
                      ImU32 tintColor, const ToolbarIconTextureProvider& provider) {
  if (drawList == nullptr || !provider) {
    return false;
  }

  const std::optional<svg::RendererBitmap>& bitmap = CachedTextChipIconBitmap(icon);
  if (!bitmap.has_value()) {
    return false;
  }

  const ToolbarIconTexture iconTexture = provider(TextChipIconTextureKey(icon), *bitmap);
  if (iconTexture.texture == 0) {
    return false;
  }

  const ImVec2 uvTopLeft(0.0f, 0.0f);
  const ImVec2 uvBottomRight(static_cast<float>(iconTexture.uvBottomRight.x),
                             static_cast<float>(iconTexture.uvBottomRight.y));
  drawList->AddImage(iconTexture.texture, min, max, uvTopLeft, uvBottomRight, tintColor);
  return true;
}

}  // namespace donner::editor
