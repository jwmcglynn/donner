/// @file
#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {

/**
 * @brief Rendering backend selected for the current Bazel configuration.
 */
enum class RendererBackend {
  TinySkia,
  Geode,
};

/**
 * @brief Backend feature flags exposed to renderer tests.
 */
enum class RendererBackendFeature : uint32_t {
  Text = 0,
  TextFull = 1,
  FilterEffects = 2,
  AsciiSnapshot = 3,
};

/**
 * @brief Returns a bitmask for a renderer feature.
 *
 * @param feature The feature to convert.
 * @return Bitmask for the feature.
 */
constexpr uint32_t RendererBackendFeatureMask(RendererBackendFeature feature) {
  return 1u << static_cast<uint32_t>(feature);
}

/**
 * @brief Returns a human-readable backend name.
 *
 * @param backend The backend to stringify.
 * @return Backend name.
 */
inline std::string_view RendererBackendName(RendererBackend backend) {
  switch (backend) {
    case RendererBackend::TinySkia: return "TinySkia";
    case RendererBackend::Geode: return "Geode";
  }

  return "Unknown";
}

/**
 * @brief Returns a human-readable feature name.
 *
 * @param feature The feature to stringify.
 * @return Feature name.
 */
inline std::string_view RendererBackendFeatureName(RendererBackendFeature feature) {
  switch (feature) {
    case RendererBackendFeature::Text: return "text rendering";
    case RendererBackendFeature::TextFull: return "full text rendering";
    case RendererBackendFeature::FilterEffects: return "filter effects";
    case RendererBackendFeature::AsciiSnapshot: return "ASCII snapshot";
  }

  return "unknown feature";
}

// ---------------------------------------------------------------------------
// Runtime backend dispatch
//
// The geode-enabled build links *both* the tiny-skia and geode test backends
// into one binary (so geode-vs-tiny-skia parity can run in-process); the
// pure-CPU build links tiny-skia only. Each backend registers its operations
// into a process-wide table at startup (see `RegisterTinySkiaBackend` /
// `RegisterGeodeBackend`), and the functions below dispatch to a backend
// chosen *at runtime*. `RenderDocumentWithBackend(doc, Geode)` only works in a
// build that linked the geode backend; asking for an unregistered backend is a
// fatal error.
// ---------------------------------------------------------------------------

/**
 * @brief Returns whether the given backend is linked into this binary.
 *
 * @param backend The backend to query.
 * @return True if the backend registered its operations.
 */
bool IsRendererBackendAvailable(RendererBackend backend);

/**
 * @brief Returns whether the given backend supports a renderer test feature.
 *
 * @param backend The backend to query.
 * @param feature The feature to query.
 * @return True if the backend supports the feature.
 */
bool RendererBackendSupportsFeature(RendererBackend backend, RendererBackendFeature feature);

/**
 * @brief Renders a document with the given backend and returns a snapshot.
 *
 * @param document The document to render.
 * @param backend The backend to render with (must be linked into this binary).
 * @param verbose If true, enable backend-specific verbose logging.
 * @return Snapshot of the rendered document.
 */
RendererBitmap RenderDocumentWithBackend(SVGDocument& document, RendererBackend backend,
                                         bool verbose = false);

/**
 * @brief Renders a document with the given backend for ASCII snapshots.
 *
 * Implementations disable anti-aliasing so legacy ASCII goldens remain stable.
 *
 * @param document The document to render.
 * @param backend The backend to render with (must be linked into this binary).
 * @return Snapshot of the rendered document.
 */
RendererBitmap RenderDocumentWithBackendForAscii(SVGDocument& document, RendererBackend backend);

/**
 * @brief Creates a new renderer instance for the given backend.
 *
 * @param backend The backend to construct (must be linked into this binary).
 * @param verbose If true, enable backend-specific verbose logging.
 * @return A new renderer instance.
 */
std::unique_ptr<RendererInterface> CreateRendererInstance(RendererBackend backend,
                                                          bool verbose = false);

/**
 * @brief Operation table a backend registers with the dispatcher.
 *
 * Each backend translation unit fills one of these and hands it to
 * \ref RegisterBackendOps from its `Register<Backend>Backend()` entry point.
 */
struct BackendOps {
  /// Renders a document and returns a snapshot.
  RendererBitmap (*render)(SVGDocument& document, bool verbose);
  /// Renders a document for ASCII snapshots (anti-aliasing disabled).
  RendererBitmap (*renderForAscii)(SVGDocument& document);
  /// Reports whether the backend supports a given test feature.
  bool (*supportsFeature)(RendererBackendFeature feature);
  /// Creates a fresh renderer instance for the backend.
  std::unique_ptr<RendererInterface> (*createInstance)(bool verbose);
};

/**
 * @brief Registers a backend's operations into the process-wide dispatch table.
 *
 * Called from each backend's `Register<Backend>Backend()` entry point.
 *
 * @param backend The backend being registered.
 * @param ops The backend's operation table.
 */
void RegisterBackendOps(RendererBackend backend, const BackendOps& ops);

/// Registers the tiny-skia test backend (always linked).
void RegisterTinySkiaBackend();

#ifdef DONNER_GEODE_BACKEND_AVAILABLE
/// Registers the geode test backend (linked only in the geode-enabled build).
void RegisterGeodeBackend();
#endif

// ---------------------------------------------------------------------------
// Active-backend convenience API (the build's *primary* backend)
//
// `ActiveRendererBackend()` returns the backend the build is configured around
// (Geode in the geode build, TinySkia otherwise); the `*Active*` helpers below
// simply forward to the per-backend dispatch above. Existing callers keep
// compiling unchanged while migration to explicit per-call backends proceeds.
// ---------------------------------------------------------------------------

/**
 * @brief Returns the primary renderer backend selected for the current build.
 *
 * @return Active renderer backend.
 */
RendererBackend ActiveRendererBackend();

/**
 * @brief Returns a human-readable name for the active renderer backend.
 *
 * @return Active backend name.
 */
std::string_view ActiveRendererBackendName();

/**
 * @brief Returns whether the active backend supports a renderer test feature.
 *
 * @param feature The feature to query.
 * @return True if the active backend supports the feature.
 */
bool ActiveRendererSupportsFeature(RendererBackendFeature feature);

/**
 * @brief Renders a document with the active backend and returns a snapshot.
 *
 * @param document The document to render.
 * @param verbose If true, enable backend-specific verbose logging.
 * @return Snapshot of the rendered document.
 */
RendererBitmap RenderDocumentWithActiveBackend(SVGDocument& document, bool verbose = false);

/**
 * @brief Renders a document with the active backend for ASCII snapshots.
 *
 * Implementations disable anti-aliasing so legacy ASCII goldens remain stable.
 *
 * @param document The document to render.
 * @return Snapshot of the rendered document.
 */
RendererBitmap RenderDocumentWithActiveBackendForAscii(SVGDocument& document);

/**
 * @brief Creates a new renderer instance for the active backend.
 *
 * @param verbose If true, enable backend-specific verbose logging.
 * @return A new renderer instance.
 */
std::unique_ptr<RendererInterface> CreateActiveRendererInstance(bool verbose = false);

}  // namespace donner::svg
