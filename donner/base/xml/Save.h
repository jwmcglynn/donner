#pragma once
/// @file

#include <vector>

#include "donner/base/ParseResult.h"
#include "donner/base/RcString.h"
#include "donner/base/xml/ReplaceSpanPlanner.h"
#include "donner/base/xml/SourceDocument.h"

namespace donner::xml {

/**
 * Options controlling the save pipeline behavior.
 */
struct SaveOptions {
  /// Allow falling back to expanded replacements when precise spans are missing or conflicting.
  bool allowFallbackExpansion = true;
};

/**
 * Diagnostics describing how a save operation was executed.
 */
struct SaveDiagnostics {
  bool usedFallback = false;  ///< True if any fallback replacement was applied.
  std::vector<SourceDocument::Replacement> appliedReplacements;  ///< Final replacements used.
};

/**
 * Result of saving an XML document with span-preserving replacements.
 */
struct SaveResult {
  RcString updatedText;               ///< Updated source after applying replacements.
  SourceDocument::OffsetMap offsetMap;  ///< Mapping from original to updated offsets.
  SaveDiagnostics diagnostics;        ///< Execution diagnostics for the save.
};

/**
 * Plan and apply replacements onto \p source, returning updated text and diagnostics.
 */
ParseResult<SaveResult> SaveDocument(
    const SourceDocument& source, std::vector<ReplaceSpanPlanner::ReplaceSpan> replacements,
    const SaveOptions& options = SaveOptions());

}  // namespace donner::xml

