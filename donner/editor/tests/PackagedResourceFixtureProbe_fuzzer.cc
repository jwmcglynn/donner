/// @file
/// Exercises authenticated font and icon fixtures through each fuzzer entry point.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

#include "donner/editor/EmbeddedSvgIcon.h"
#include "donner/svg/SVGTextElement.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/RendererTinySkia.h"
#include "embed_resources/EditorIcons.h"

namespace donner::editor {
namespace {

void Require(bool condition, const char* message) {
  if (!condition) {
    std::fprintf(stderr, "Packaged fixture probe: %s\n", message);
    std::abort();
  }
}

bool HasInk(const svg::RendererBitmap& bitmap) {
  for (std::size_t i = 3; i < bitmap.pixels.size(); i += 4) {
    if (bitmap.pixels[i] != 0) {
      return true;
    }
  }
  return false;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* /*data*/, std::size_t /*size*/) {
  constexpr std::string_view kText =
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="32" height="32">)"
      R"(<text x="2" y="24" font-size="20">A</text></svg>)";
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = svg::parser::SVGParser::ParseSVG(kText, warnings);
  Require(parsed.hasResult(), "text document did not parse");
  svg::SVGDocument document = std::move(parsed.result());
  const auto text = document.querySelector("text");
  Require(text.has_value(), "text element is missing");
  Require(text->cast<svg::SVGTextElement>().getComputedTextLength() > 0,
          "fallback font glyphs are unavailable");
  svg::RendererTinySkia renderer;
  renderer.draw(document);
  Require(HasInk(renderer.takeSnapshot()), "fallback glyph rasterization is empty");
  ConfigureEmbeddedSvgIconRenderer(renderer);
  const auto icon = RenderEmbeddedSvgIcon(embedded::kEditorChevronSvg, 20);
  ResetEmbeddedSvgIconRenderer(renderer);
  Require(icon.has_value() && HasInk(*icon), "packaged icon rasterization is empty");
  return 0;
}

}  // namespace donner::editor
