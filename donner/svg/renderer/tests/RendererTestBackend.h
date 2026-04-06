/// @file
#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

#include "donner/svg/SVGDocument.h"
#include "donner/svg/renderer/RendererInterface.h"

namespace donner::svg {

/**
 * @brief Rendering backend selected for the current Bazel configuration.
 */
enum class RendererBackend {
  Skia,
  TinySkia,
};

/**
 * @brief Backend feature flags exposed to renderer tests.
 */
enum class RendererBackendFeature : uint32_t {
  Text = 0,
  TextFull = 1,
  FilterEffects = 2,
  AsciiSnapshot = 3,
  SkpDebug = 4,
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
    case RendererBackend::Skia: return "Skia";
    case RendererBackend::TinySkia: return "TinySkia";
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
    case RendererBackendFeature::SkpDebug: return "SkPicture debug capture";
  }

  return "unknown feature";
}

/**
 * @brief Returns the renderer backend selected for the current build.
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
 * @brief Writes a `.skp` debug file for the active backend when supported.
 *
 * @param document The document to render.
 * @param outputPath The output file to write.
 * @return True if the file was written.
 */
bool WriteActiveRendererDebugSkp(SVGDocument& document, const std::filesystem::path& outputPath);

}  // namespace donner::svg
