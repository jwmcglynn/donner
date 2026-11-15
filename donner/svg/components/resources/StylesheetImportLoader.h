#pragma once
/// @file

#include <cstddef>
#include <functional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/RcString.h"
#include "donner/css/Stylesheet.h"
#include "donner/css/parser/RuleParser.h"
#include "donner/svg/resources/UrlLoader.h"

namespace donner::svg::components {

/**
 * Bounds applied to one document-wide `@import` resolution pass, across every stylesheet of the
 * document. Fetches are additionally charged against the document's existing external-fetch and
 * aggregate resource budgets by the caller.
 */
struct StylesheetImportBudget {
  /// Maximum nesting depth; a top-level import has depth 1.
  static constexpr std::size_t kMaximumDepth = 16;
  /// Maximum number of import loads, counting each occurrence (including repeats of one URL).
  static constexpr std::size_t kMaximumImports = 64;
  /// Maximum total bytes of imported CSS text.
  static constexpr std::size_t kMaximumBytes = 2 * 1024 * 1024;

  std::size_t remainingImports = kMaximumImports;  ///< Import loads still permitted.
  std::size_t remainingBytes = kMaximumBytes;      ///< Imported CSS bytes still permitted.
  /// Parsed rules still permitted across all imported sheets: one stylesheet's worth in total.
  std::size_t remainingRules = css::parser::RuleParser::kMaximumRules;
  /// Parsed component values still permitted across all imported sheets.
  std::size_t remainingComponentValues = css::parser::RuleParser::kMaximumComponentValues;
};

/**
 * Fetch callback for one resolved import URL: returns the loaded bytes, or a diagnostic reason
 * (which must not contain the URL) when the resource is refused or unavailable.
 *
 * The callback receives the maximum number of bytes this import may still use, so oversized
 * responses can be rejected before they are retained.
 */
using StylesheetImportFetch = std::function<std::variant<UrlLoader::Result, RcString>(
    std::string_view url, std::size_t maximumBytes)>;

/**
 * Load the `@import` rules of \p stylesheet and, recursively, of the stylesheets they import.
 *
 * Top-level import URLs are document-relative and passed to \p fetch after dot-segment
 * normalization; nested imports resolve against the URL of the stylesheet that contains them, as
 * CSS specifies. Imports that would recurse into a stylesheet already being loaded on the current
 * path (a cycle), exceed \ref StylesheetImportBudget, cannot be resolved, fail to load, are not
 * UTF-8, or have a non-CSS data URL type are skipped with a warning. Relative `@font-face` source
 * URLs in an imported sheet are resolved against that sheet's URL. Warnings are reported at the
 * offset of the top-level import that led to them, in \p stylesheet's local CSS coordinates, and
 * never include URLs. Diagnostics from parsing imported text are not reported, since their offsets
 * refer to another resource.
 *
 * @param stylesheet Stylesheet whose imports should be loaded.
 * @param fetch Callback that loads one resolved URL.
 * @param budget Document-wide budget, decremented by this call.
 * @param warnings Sink receiving load diagnostics.
 * @return Imported stylesheets in cascade order: each sheet follows the sheets it imports, in
 *   import order, and all precede \p stylesheet.
 */
std::vector<css::Stylesheet> LoadStylesheetImports(const css::Stylesheet& stylesheet,
                                                   const StylesheetImportFetch& fetch,
                                                   StylesheetImportBudget& budget,
                                                   ParseWarningSink& warnings);

}  // namespace donner::svg::components
