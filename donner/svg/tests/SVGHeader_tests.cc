#include "donner/svg/SVG.h"

namespace donner::svg {

template <typename Document>
concept HasPublicRenderingDiagnosticsType = requires { typename Document::RenderingDiagnostics; };

template <typename Document>
concept HasPublicRenderingDiagnosticsMethod =
    requires(const Document& document) { document.renderingDiagnostics(); };

static_assert(!HasPublicRenderingDiagnosticsType<SVGDocument>,
              "Raw storage diagnostics must remain outside the public SVG document API");
static_assert(!HasPublicRenderingDiagnosticsMethod<SVGDocument>,
              "Tool-only inspection must remain outside the public SVG document API");

static_assert(requires(const SVGElement& element, std::ostream& output) {
  element.getComputedStyle().opacity.get();
  element.specifiedStyle()->fill.get();
  element.computedStyleIfPresent()->stroke.get();
  output << element.getComputedStyle();
});

}  // namespace donner::svg
