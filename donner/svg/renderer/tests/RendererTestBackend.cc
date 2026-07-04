#include "donner/svg/renderer/tests/RendererTestBackend.h"

#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>

namespace donner::svg {

// ---------------------------------------------------------------------------
// Backend registration table
//
// Each backend's translation unit (RendererTestBackendTinySkia.cc /
// RendererTestBackendGeode.cc) defines a `Register<Backend>Backend()` function
// that calls `RegisterBackendOps()` to populate this table with its function
// pointers. The geode build links both TUs (so both register); the CPU build
// links tiny-skia only. Registration is driven explicitly from
// `EnsureBackendsRegistered()` below - no static-init ordering dependency, and
// the table entry for an unlinked backend stays empty.
// ---------------------------------------------------------------------------

namespace {

constexpr size_t kBackendCount = 2;  // TinySkia, Geode

std::array<std::optional<BackendOps>, kBackendCount>& BackendTable() {
  static std::array<std::optional<BackendOps>, kBackendCount> table;
  return table;
}

/// Registers every backend linked into this binary exactly once. Tiny-skia is
/// always linked; the geode backend is only linked (and only declared) in the
/// geode-enabled build.
void EnsureBackendsRegistered() {
  static const bool registered = [] {
    RegisterTinySkiaBackend();
#ifdef DONNER_GEODE_BACKEND_AVAILABLE
    RegisterGeodeBackend();
#endif
    return true;
  }();
  (void)registered;
}

const BackendOps& OpsFor(RendererBackend backend) {
  EnsureBackendsRegistered();
  const auto& slot = BackendTable()[static_cast<size_t>(backend)];
  if (!slot.has_value()) {
    std::cerr << "RendererTestBackend: backend '" << RendererBackendName(backend)
              << "' is not linked into this binary\n";
    std::abort();
  }
  return *slot;
}

}  // namespace

void RegisterBackendOps(RendererBackend backend, const BackendOps& ops) {
  BackendTable()[static_cast<size_t>(backend)] = ops;
}

bool IsRendererBackendAvailable(RendererBackend backend) {
  EnsureBackendsRegistered();
  return BackendTable()[static_cast<size_t>(backend)].has_value();
}

bool RendererBackendSupportsFeature(RendererBackend backend, RendererBackendFeature feature) {
  return OpsFor(backend).supportsFeature(feature);
}

RendererBitmap RenderDocumentWithBackend(SVGDocument& document, RendererBackend backend,
                                         bool verbose) {
  return OpsFor(backend).render(document, verbose);
}

RendererBitmap RenderDocumentWithBackendForAscii(SVGDocument& document, RendererBackend backend) {
  return OpsFor(backend).renderForAscii(document);
}

std::unique_ptr<RendererInterface> CreateRendererInstance(RendererBackend backend, bool verbose) {
  return OpsFor(backend).createInstance(verbose);
}

// ---------------------------------------------------------------------------
// Active-backend convenience API
//
// The build's primary backend is selected at compile time: Geode in the
// geode-enabled build (which defines DONNER_GEODE_BACKEND_AVAILABLE), TinySkia
// otherwise. All `*Active*` helpers forward to the per-backend dispatch above.
// ---------------------------------------------------------------------------

RendererBackend ActiveRendererBackend() {
#ifdef DONNER_GEODE_BACKEND_AVAILABLE
  return RendererBackend::Geode;
#else
  return RendererBackend::TinySkia;
#endif
}

std::string_view ActiveRendererBackendName() {
  return RendererBackendName(ActiveRendererBackend());
}

bool ActiveRendererSupportsFeature(RendererBackendFeature feature) {
  return RendererBackendSupportsFeature(ActiveRendererBackend(), feature);
}

RendererBitmap RenderDocumentWithActiveBackend(SVGDocument& document, bool verbose) {
  return RenderDocumentWithBackend(document, ActiveRendererBackend(), verbose);
}

RendererBitmap RenderDocumentWithActiveBackendForAscii(SVGDocument& document) {
  return RenderDocumentWithBackendForAscii(document, ActiveRendererBackend());
}

std::unique_ptr<RendererInterface> CreateActiveRendererInstance(bool verbose) {
  return CreateRendererInstance(ActiveRendererBackend(), verbose);
}

}  // namespace donner::svg
