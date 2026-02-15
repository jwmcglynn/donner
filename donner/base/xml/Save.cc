#include "donner/base/xml/Save.h"

namespace donner::xml {

ParseResult<SaveResult> SaveDocument(
    const SourceDocument& source, std::vector<ReplaceSpanPlanner::ReplaceSpan> replacements,
    const SaveOptions& options) {
  ReplaceSpanPlanner planner;
  auto planResult = planner.plan(std::move(replacements));
  if (!planResult.hasResult()) {
    return ParseResult<SaveResult>(planResult.error());
  }

  if (!options.allowFallbackExpansion && planResult.result().usedFallback) {
    return ParseResult<SaveResult>(
        ParseError{RcString("Fallback replacements are disallowed by SaveOptions")});
  }

  auto applied = source.applyReplacements(planResult.result().ordered);
  if (!applied.hasResult()) {
    return ParseResult<SaveResult>(applied.error());
  }

  SaveDiagnostics diagnostics;
  diagnostics.usedFallback = planResult.result().usedFallback;
  diagnostics.appliedReplacements = planResult.result().ordered;

  SaveResult result{applied.result().text, std::move(applied.result().offsetMap),
                    std::move(diagnostics)};
  return ParseResult<SaveResult>(std::move(result));
}

}  // namespace donner::xml

