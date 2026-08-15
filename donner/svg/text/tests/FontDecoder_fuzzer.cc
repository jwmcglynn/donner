#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "donner/base/EcsRegistry.h"
#include "donner/base/fonts/SfntUtils.h"
#include "donner/base/fonts/WoffParser.h"
#ifdef DONNER_TEXT_FULL
#include "donner/base/fonts/Woff2Parser.h"
#include "donner/svg/text/TextBackendFull.h"
#else
#include "donner/svg/text/TextBackendSimple.h"
#endif
#include "donner/svg/core/FontVariant.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::svg {
namespace {

uint32_t ReadMagic(std::span<const uint8_t> data) {
  return data.size() >= 4 ? fonts::ReadBe32(data.data()) : 0;
}

#ifdef DONNER_TEXT_FULL
using FuzzTextBackend = TextBackendFull;
#else
using FuzzTextBackend = TextBackendSimple;
#endif

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  const std::span<const uint8_t> fontBytes(data, size);
  const uint32_t magic = ReadMagic(fontBytes);
  if (magic == 0x774F4646) {  // wOFF
    if (fonts::WoffParser::Parse(fontBytes).hasError()) {
      return 0;
    }
#ifdef DONNER_TEXT_FULL
  } else if (magic == 0x774F4632) {  // wOF2
    if (fonts::Woff2Parser::Decompress(fontBytes).hasError()) {
      return 0;
    }
#endif
  } else if (!fonts::ValidateSfnt(fontBytes)) {
    return 0;
  }

  Registry registry;
  FontManager fontManager(registry);
  const FontHandle font = fontManager.loadFontData(fontBytes);
  if (!font) {
    return 0;
  }

  FuzzTextBackend backend(fontManager, registry);
  (void)backend.fontVMetrics(font);
  (void)backend.scaleForPixelHeight(font, 16.0f);
  (void)backend.scaleForEmToPixels(font, 16.0f);
  (void)backend.underlineMetrics(font);
  (void)backend.strikeoutMetrics(font);
  (void)backend.subSuperMetrics(font);
  (void)backend.isBitmapOnly(font);
  (void)backend.hasSmallCapsFeature(font);

  constexpr std::string_view kText = "Az";
  const TextBackend::ShapedRun shaped =
      backend.shapeRun(font, 16.0f, kText, 0, kText.size(), false, FontVariant::Normal, false);
  for (const TextBackend::ShapedGlyph& glyph : shaped.glyphs) {
    (void)backend.glyphOutline(font, glyph.glyphIndex, 1.0f);
  }
  (void)backend.crossSpanKern(font, 16.0f, font, 16.0f, 'A', 'z', false);
  return 0;
}

}  // namespace donner::svg
