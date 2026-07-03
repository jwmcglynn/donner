#include "donner/editor/EmbeddedSvgIcon.h"

#include <string_view>
#include <utility>

#include "donner/base/Length.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGSVGElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/Renderer.h"

namespace donner::editor {
namespace {

std::string_view StringViewFromSpan(std::span<const unsigned char> bytes) {
  return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

void NormalizeIconBitmapToTintableAlphaMask(svg::RendererBitmap* bitmap) {
  if (bitmap == nullptr || bitmap->empty()) {
    return;
  }

  for (int y = 0; y < bitmap->dimensions.y; ++y) {
    unsigned char* row = bitmap->pixels.data() + static_cast<std::size_t>(y) * bitmap->rowBytes;
    for (int x = 0; x < bitmap->dimensions.x; ++x) {
      unsigned char* pixel = row + static_cast<std::size_t>(x) * 4u;
      const unsigned char alpha = pixel[3];
      pixel[0] = alpha;
      pixel[1] = alpha;
      pixel[2] = alpha;
    }
  }
  bitmap->alphaType = svg::AlphaType::Premultiplied;
}

/// One shared renderer for all embedded-icon rasterization, created on first
/// use. Icons render lazily from UI-thread panel code only. Constructing a
/// fresh renderer per icon is disproportionately expensive on GPU backends —
/// each construction stands up a full WebGPU instance/adapter/device — and
/// showed up as a stream of duplicate "[Geode/wgpu-native] Adapter:" logs at
/// editor startup.
svg::Renderer& SharedIconRenderer() {
  static svg::Renderer* renderer = new svg::Renderer();
  return *renderer;
}

}  // namespace

std::optional<svg::RendererBitmap> RenderEmbeddedSvgIcon(std::span<const unsigned char> svgBytes,
                                                         int outputSizePx) {
  if (outputSizePx <= 0) {
    return std::nullopt;
  }

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parseResult = svg::parser::SVGParser::ParseSVG(StringViewFromSpan(svgBytes), warnings);
  if (parseResult.hasError()) {
    return std::nullopt;
  }

  svg::SVGDocument document = std::move(parseResult.result());
  svg::SVGSVGElement root = document.svgElement();
  root.setWidth(Lengthd(outputSizePx, Lengthd::Unit::Px));
  root.setHeight(Lengthd(outputSizePx, Lengthd::Unit::Px));

  svg::Renderer& renderer = SharedIconRenderer();
  renderer.draw(document);
  svg::RendererBitmap bitmap = renderer.takeSnapshot();
  if (bitmap.empty()) {
    return std::nullopt;
  }

  NormalizeIconBitmapToTintableAlphaMask(&bitmap);
  return bitmap;
}

}  // namespace donner::editor
