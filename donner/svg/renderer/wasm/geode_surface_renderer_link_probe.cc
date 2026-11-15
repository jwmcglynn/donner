/// @file
/// Compiles and links GeodeSurfaceRenderer's browser canvas path into a WebAssembly module.
///
/// The canvas factory, the browser root header it selects and the Metal-layer stub exist only in
/// browser-backend builds, so ordinary native builds never compile them. Linking this module
/// against the browser bridge's JavaScript library proves that path builds, and that every bridge
/// entry point it reaches is implemented. Running it in a browser presents one frame.

#include <cstdio>
#include <memory>
#include <string>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/GeodeSurfaceRenderer.h"

int main() {
  using donner::svg::GeodeSurfaceRenderer;

  donner::ParseWarningSink warnings;
  donner::ParseResult<donner::svg::SVGDocument> parsed = donner::svg::parser::SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg" width="64" height="64">
           <rect width="64" height="64" fill="#d33"/>
         </svg>)",
      warnings);
  if (parsed.hasError()) {
    return 1;
  }
  donner::svg::SVGDocument document = std::move(parsed.result());

  std::string error;
  std::unique_ptr<GeodeSurfaceRenderer> renderer =
      GeodeSurfaceRenderer::CreateForCanvas("#canvas", donner::Vector2i(64, 64), &error);
  if (renderer == nullptr) {
    std::fprintf(stderr, "CreateForCanvas failed: %s\n", error.c_str());
    return 1;
  }
  if (!renderer->resize(donner::Vector2i(96, 64), &error)) {
    std::fprintf(stderr, "resize failed: %s\n", error.c_str());
    return 1;
  }
  document.setCanvasSize(96, 64);
  const GeodeSurfaceRenderer::FrameStatus status = renderer->draw(document);
  std::fprintf(stderr, "frame status: %d\n", static_cast<int>(status));
  return status == GeodeSurfaceRenderer::FrameStatus::Failed ? 1 : 0;
}
