#pragma once
/// @file

#include <vector>

#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/TextBackend.h"
#include "donner/svg/renderer/TextTypes.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::svg {

/**
 * Shared SVG text layout engine.
 *
 * Implements the SVG text layout algorithm: per-character positioning, baseline shifts,
 * text-anchor, textLength, and text-on-path. Delegates font-specific operations
 * (glyph lookup, advance widths, kerning, outline extraction) to a TextBackend.
 *
 * Usage:
 * @code
 *   TextBackendSimple backend(fontManager);
 *   TextEngine engine(backend, fontManager);
 *   std::vector<TextRun> runs = engine.layout(text, params);
 *   // Use engine.backend() for outline/metrics queries during rendering.
 * @endcode
 */
class TextEngine {
public:
  TextEngine(TextBackend& backend, FontManager& fontManager);

  /// Lay out all spans, returning positioned glyph runs.
  std::vector<TextRun> layout(const components::ComputedTextComponent& text,
                              const TextParams& params);

  /// Access the backend for rendering queries (glyphOutline, fontVMetrics, etc.).
  TextBackend& backend() { return backend_; }
  const TextBackend& backend() const { return backend_; }

private:
  TextBackend& backend_;
  FontManager& fontManager_;
};

}  // namespace donner::svg
