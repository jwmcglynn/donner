#pragma once
/// @file

#include <memory>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::geode {
class GeodeDevice;
}

namespace donner::svg {

/// @cond INTERNAL
// Link-time backend selection: exactly one of RendererTinySkiaBackend.cc /
// RendererGeodeBackend.cc is linked (BUILD config), and each defines these
// factories returning its concrete backend. The backends implement
// RendererInterface directly; there is deliberately no intermediate
// "implementation" interface — an earlier RendererImplementation layer added
// only hand-written forwarding, where a missed forward silently fell back to
// a base-class default (a real perf bug for drawBitmap).
std::unique_ptr<RendererInterface> CreateRendererImplementation(bool verbose);
std::unique_ptr<RendererInterface> CreateRendererImplementation(
    std::shared_ptr<geode::GeodeDevice> device, bool verbose);
/// @endcond

}  // namespace donner::svg
