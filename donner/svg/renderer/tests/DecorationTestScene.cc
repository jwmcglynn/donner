#include "donner/svg/renderer/tests/DecorationTestScene.h"

#include <algorithm>
#include <memory>

#include "donner/base/Utf8.h"
#include "donner/svg/renderer/tests/RendererTestBackend.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/tests/ParserTestUtils.h"
#include "donner/svg/text/TextEngine.h"

namespace donner::svg::tests {

DecorationBackend::DecorationBackend(bool underlineMetrics, double glyphHeight)
    : hasUnderlineMetrics_(underlineMetrics), glyphHeight_(glyphHeight) {}

FontVMetrics DecorationBackend::fontVMetrics(FontHandle) const {
  return {.ascent = 800, .descent = -200, .lineGap = 0, .xHeight = 500, .unitsPerEm = 1000};
}

float DecorationBackend::scaleForPixelHeight(FontHandle, float size) const {
  return size / 1000;
}

float DecorationBackend::scaleForEmToPixels(FontHandle, float size) const {
  return size / 1000;
}

std::optional<UnderlineMetrics> DecorationBackend::underlineMetrics(FontHandle) const {
  return hasUnderlineMetrics_ ? std::optional(UnderlineMetrics{.position = -100, .thickness = 50})
                              : std::nullopt;
}

std::optional<UnderlineMetrics> DecorationBackend::strikeoutMetrics(FontHandle) const {
  return UnderlineMetrics{.position = 300, .thickness = 40};
}

std::optional<SubSuperMetrics> DecorationBackend::subSuperMetrics(FontHandle) const {
  return std::nullopt;
}

Path DecorationBackend::glyphOutline(FontHandle, int, float) const {
  return PathBuilder().addRect(Box2d::FromXYWH(0, -glyphHeight_, 1, glyphHeight_)).build();
}

bool DecorationBackend::isBitmapOnly(FontHandle) const {
  return false;
}

bool DecorationBackend::isCursive(uint32_t) const {
  return false;
}

bool DecorationBackend::hasSmallCapsFeature(FontHandle) const {
  return false;
}

std::optional<DecorationBackend::BitmapGlyph> DecorationBackend::bitmapGlyph(FontHandle, int,
                                                                             float) const {
  return std::nullopt;
}

DecorationBackend::ShapedRun DecorationBackend::shapeRun(FontHandle, float, std::string_view text,
                                                         size_t begin, size_t length, bool,
                                                         FontVariant, bool) const {
  ShapedRun run;
  const size_t end = std::min(text.size(), begin + length);
  for (size_t index = begin; index < end;) {
    const size_t start = index;
    const auto [codepoint, bytes] = Utf8::NextCodepoint(text.substr(index, end - index));
    (void)codepoint;
    index += bytes;
    run.glyphs.push_back(
        {.glyphIndex = 3, .xAdvance = 10, .yAdvance = 10, .cluster = static_cast<uint32_t>(start)});
  }

  return run;
}

double DecorationBackend::crossSpanKern(FontHandle, float, FontHandle, float, uint32_t, uint32_t,
                                        bool) const {
  return 0;
}

SVGDocument DecorationDocument(std::string_view body, bool metrics, double glyphHeight) {
  SVGDocument document = instantiateSubtree(body, {}, Vector2i(100, 100));
  auto& registry = document.registry();
  auto& fonts = registry.ctx().emplace<FontManager>(registry);
  registry.ctx().emplace<TextEngine>(fonts, registry,
                                     std::make_unique<DecorationBackend>(metrics, glyphHeight));
  return document;
}

RendererBitmap RenderDecorationScene(std::string_view body, bool metrics, double glyphHeight) {
  auto document = DecorationDocument(body, metrics, glyphHeight);
  return RenderDocumentWithBackend(document, ActiveRendererBackend());
}

}  // namespace donner::svg::tests
