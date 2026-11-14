#include "donner/svg/components/resources/StylesheetImportLoader.h"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

#include "donner/base/ParseDiagnostic.h"
#include "donner/base/StringUtils.h"
#include "donner/base/Utf8.h"
#include "donner/css/parser/StylesheetParser.h"
#include "donner/svg/resources/UrlReference.h"

namespace donner::svg::components {

namespace {

constexpr std::string_view kUtf8Bom = "\xEF\xBB\xBF";

/**
 * Resolve the relative `url()` sources of an imported sheet's font faces against the sheet's own
 * URL, as CSS specifies. Sources that cannot be resolved are dropped rather than being resolved
 * against the document by mistake.
 */
void RebaseFontFaceSources(css::Stylesheet& sheet, std::string_view sheetUrl) {
  for (css::FontFace& fontFace : sheet.mutableFontFaces()) {
    std::erase_if(fontFace.sources, [&](css::FontFaceSource& source) {
      if (source.kind != css::FontFaceSource::Kind::Url) {
        return false;
      }
      std::optional<std::string> resolved =
          ResolveUrlReference(sheetUrl, std::get<RcString>(source.payload));
      if (!resolved.has_value() || resolved->empty()) {
        return true;
      }
      source.payload = RcString(*resolved);
      return false;
    });
  }
}

/// Depth-first loader for one root stylesheet; see \ref LoadStylesheetImports.
class ImportLoader {
public:
  ImportLoader(const StylesheetImportFetch& fetch, StylesheetImportBudget& budget,
               ParseWarningSink& warnings)
      : fetch_(fetch), budget_(budget), warnings_(warnings) {}

  void loadRoot(const css::Stylesheet& stylesheet) {
    for (const css::ImportRule& import : stylesheet.imports()) {
      loadImport(import.url, "", import.sourceOffset, 1);
    }
  }

  std::vector<css::Stylesheet> takeResult() { return std::move(result_); }

private:
  void warn(const char* reason, FileOffset offset) {
    warnings_.add([&] { return ParseDiagnostic::Warning(reason, offset); });
  }

  /// Check every limit that applies before a fetch; return the refusal reason, if any.
  const char* admissionFailure(const std::string& resolvedUrl, std::size_t depth) const {
    if (depth > StylesheetImportBudget::kMaximumDepth) {
      return "CSS @import nesting exceeds the depth limit; the nested import is ignored";
    }
    if (std::find(path_.begin(), path_.end(), resolvedUrl) != path_.end()) {
      return "CSS @import cycle detected; the repeated stylesheet is ignored";
    }
    if (budget_.remainingImports == 0) {
      return "CSS @import count exceeds the document limit; the import is ignored";
    }
    if (budget_.remainingBytes == 0) {
      return "CSS @import bytes exceed the document limit; the import is ignored";
    }
    return nullptr;
  }

  /// Validate fetched bytes and charge them to the byte budget; return the CSS text on success.
  std::optional<std::string_view> admitText(const UrlLoader::Result& loaded, FileOffset offset) {
    if (!loaded.mimeType.empty() && !StringUtils::EqualsLowercase(std::string_view(loaded.mimeType),
                                                                  std::string_view("text/css"))) {
      warn("CSS @import target is not a text/css resource; the import is ignored", offset);
      return std::nullopt;
    }
    if (loaded.data.size() > budget_.remainingBytes) {
      budget_.remainingBytes = 0;
      warn("CSS @import bytes exceed the document limit; the import is ignored", offset);
      return std::nullopt;
    }
    budget_.remainingBytes -= loaded.data.size();
    std::string_view text(reinterpret_cast<const char*>(loaded.data.data()),  // NOLINT
                          loaded.data.size());
    if (!Utf8::IsValidString(text)) {
      warn("CSS @import target is not valid UTF-8; the import is ignored", offset);
      return std::nullopt;
    }
    if (text.starts_with(kUtf8Bom)) {
      text.remove_prefix(kUtf8Bom.size());
    }
    return text;
  }

  /// Charge the parsed size of an imported sheet; return false if it exceeds the budget.
  bool admitParsed(const css::Stylesheet& sheet, FileOffset offset) {
    const css::Stylesheet::SecurityStats& stats = sheet.securityStats();
    if (stats.rules > budget_.remainingRules ||
        stats.componentValues > budget_.remainingComponentValues) {
      budget_.remainingRules = 0;
      budget_.remainingComponentValues = 0;
      warn("CSS @import parsed size exceeds the document limit; the import is ignored", offset);
      return false;
    }
    budget_.remainingRules -= stats.rules;
    budget_.remainingComponentValues -= stats.componentValues;
    return true;
  }

  void loadImport(std::string_view url, std::string_view baseUrl, FileOffset offset,
                  std::size_t depth) {
    std::optional<std::string> resolved = ResolveUrlReference(baseUrl, url);
    if (!resolved.has_value()) {
      warn("CSS @import relative URL cannot be resolved against a non-hierarchical stylesheet URL",
           offset);
      return;
    }
    if (resolved->empty()) {
      // A reference such as "." names the document or base directory, never a stylesheet.
      warn("CSS @import target resolves to the document itself; the import is ignored", offset);
      return;
    }
    if (const char* failure = admissionFailure(*resolved, depth)) {
      warn(failure, offset);
      return;
    }
    --budget_.remainingImports;

    auto fetched = fetch_(*resolved, budget_.remainingBytes);
    if (const auto* reason = std::get_if<RcString>(&fetched)) {
      warnings_.add([&] { return ParseDiagnostic::Warning(*reason, offset); });
      return;
    }
    const std::optional<std::string_view> text =
        admitText(std::get<UrlLoader::Result>(fetched), offset);
    if (!text.has_value()) {
      return;
    }

    ParseWarningSink importedSheetWarnings = ParseWarningSink::Disabled();
    css::Stylesheet sheet = css::parser::StylesheetParser::Parse(*text, importedSheetWarnings);
    if (!admitParsed(sheet, offset)) {
      return;
    }

    RebaseFontFaceSources(sheet, *resolved);
    path_.push_back(*resolved);
    for (const css::ImportRule& nested : sheet.imports()) {
      loadImport(nested.url, path_.back(), offset, depth + 1);
    }
    path_.pop_back();
    result_.push_back(std::move(sheet));
  }

  const StylesheetImportFetch& fetch_;
  StylesheetImportBudget& budget_;
  ParseWarningSink& warnings_;
  /// Resolved URLs of the stylesheets currently being loaded, outermost first.
  std::vector<std::string> path_;
  std::vector<css::Stylesheet> result_;
};

}  // namespace

std::vector<css::Stylesheet> LoadStylesheetImports(const css::Stylesheet& stylesheet,
                                                   const StylesheetImportFetch& fetch,
                                                   StylesheetImportBudget& budget,
                                                   ParseWarningSink& warnings) {
  ImportLoader loader(fetch, budget, warnings);
  loader.loadRoot(stylesheet);
  return loader.takeResult();
}

}  // namespace donner::svg::components
