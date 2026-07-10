#include "donner/editor/ToolbarIconSet.h"

#include "donner/editor/EmbeddedSvgIcon.h"
#include "donner/editor/ToolIconsSvg.h"

namespace donner::editor {
namespace {

/// Raster size for toolbar artwork. Icons display at 20 logical px, so 80 px
/// gives exact 4:1 and 2:1 downsampling at the common 1x and 2x scales.
constexpr int kToolbarIconRasterSizePx = 80;

/// Icon display size inside the button, in logical px.
constexpr float kToolbarIconDisplaySize = 20.0f;

}  // namespace

std::span<const unsigned char> ToolbarIconSvg(ToolbarIcon icon) {
  switch (icon) {
    case ToolbarIcon::Select: return embedded::kToolSelectIconSvg;
    case ToolbarIcon::Pen: return embedded::kToolPenIconSvg;
    case ToolbarIcon::Text: return embedded::kToolTextIconSvg;
  }
  return embedded::kToolSelectIconSvg;
}

std::uint64_t ToolbarIconTextureKey(ToolbarIcon icon) {
  constexpr std::uint64_t kIconTextureKeyBase = 0xf700000000000000ull;
  switch (icon) {
    case ToolbarIcon::Select: return kIconTextureKeyBase + 1u;
    case ToolbarIcon::Pen: return kIconTextureKeyBase + 2u;
    case ToolbarIcon::Text: return kIconTextureKeyBase + 3u;
  }
  return kIconTextureKeyBase;
}

const std::optional<svg::RendererBitmap>& CachedToolbarIconBitmap(ToolbarIcon icon) {
  switch (icon) {
    case ToolbarIcon::Select: {
      static const std::optional<svg::RendererBitmap> bitmap =
          RenderEmbeddedSvgArtwork(ToolbarIconSvg(ToolbarIcon::Select), kToolbarIconRasterSizePx);
      return bitmap;
    }
    case ToolbarIcon::Pen: {
      static const std::optional<svg::RendererBitmap> bitmap =
          RenderEmbeddedSvgArtwork(ToolbarIconSvg(ToolbarIcon::Pen), kToolbarIconRasterSizePx);
      return bitmap;
    }
    case ToolbarIcon::Text: {
      static const std::optional<svg::RendererBitmap> bitmap =
          RenderEmbeddedSvgArtwork(ToolbarIconSvg(ToolbarIcon::Text), kToolbarIconRasterSizePx);
      return bitmap;
    }
  }

  static const std::optional<svg::RendererBitmap> empty;
  return empty;
}

void DrawToolbarIcon(ToolbarIcon icon, const ImVec2& min, const ImVec2& max, ImU32 tintColor,
                     const ToolbarIconTextureProvider& provider) {
  if (!provider) {
    return;
  }

  const std::optional<svg::RendererBitmap>& bitmap = CachedToolbarIconBitmap(icon);
  if (!bitmap.has_value()) {
    return;
  }

  const ToolbarIconTexture iconTexture = provider(ToolbarIconTextureKey(icon), *bitmap);
  if (iconTexture.texture == 0) {
    return;
  }

  const float centerX = (min.x + max.x) * 0.5f;
  const float centerY = (min.y + max.y) * 0.5f;
  const float half = kToolbarIconDisplaySize * 0.5f;
  const ImVec2 iconMin(centerX - half, centerY - half);
  const ImVec2 iconMax(centerX + half, centerY + half);
  const ImVec2 uvTopLeft(0.0f, 0.0f);
  const ImVec2 uvBottomRight(static_cast<float>(iconTexture.uvBottomRight.x),
                             static_cast<float>(iconTexture.uvBottomRight.y));
  ImGui::GetWindowDrawList()->AddImage(iconTexture.texture, iconMin, iconMax, uvTopLeft,
                                       uvBottomRight, tintColor);
}

}  // namespace donner::editor
