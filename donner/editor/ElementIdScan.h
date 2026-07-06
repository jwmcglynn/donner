#pragma once
/// @file
///
/// Shared DOM-based id scanning and id-reference rewrite helpers.
///
/// Several editor surfaces need to know which ids a document (or subtree)
/// already declares, or which attribute/style values reference a given id.
/// Before this utility existed, three call sites each grew their own
/// hand-rolled textual scanner over serialized source text: a plain substring
/// search for `id="..."` (or a `<g>`/`</g>` counter) can false-positive-match
/// text that merely *looks* like an id or a tag inside a comment, a CDATA
/// section, or an unrelated attribute's string value, and a textual depth
/// counter can miscount for the same reason.
///
/// This header centralizes the DOM-based version of that logic - walking the
/// live element tree instead of the serialized text - for every caller that
/// has a DOM available:
///  - `ViewportSvgExport.cc` (uniquify the injected viewport-clip id against
///    ids the source document already declares).
///  - `ShapeClipboardCommands.cc` (collect ids already present in the paste
///    destination document).
///  - `EditorApp.cc` (`renameSelectedElement`: walk every element/attribute to
///    repoint `url(#oldId)` / `href="#oldId"` references, and CSS id
///    selectors inside `<style>` text, to a renamed id).
///
/// A target with no DOM at all (e.g. a raw clipboard payload fragment that
/// has not been parsed into this document) is out of scope here; that case
/// stays a textual scan, made comment/CDATA/string-aware instead of naive
/// (see `ShapeClipboardCommands.cc`'s `scanDefinedIds` / `applyRenames`).

#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "donner/svg/SVGElement.h"

namespace donner::editor {

/// Depth-first (pre-order) list of every element in the subtree rooted at \p root, including
/// \p root itself.
void CollectSubtreeElements(const svg::SVGElement& root, std::vector<svg::SVGElement>& out);

/// Every non-empty `id` attribute value in the subtree rooted at \p root, including \p root
/// itself.
void CollectSubtreeIds(const svg::SVGElement& root, std::unordered_set<std::string>& out);

/// If \p value references \p oldId via `url(#oldId)` or a whole-value `#oldId` (an `href`
/// target), return the value with every such reference repointed to \p newId; otherwise return
/// `std::nullopt` (no reference, no change).
std::optional<std::string> RewriteIdReferenceInValue(std::string_view value,
                                                     std::string_view oldId,
                                                     std::string_view newId);

/// Rewrite `#oldId` CSS id tokens to `#newId` inside a `<style>` element's text content, in the
/// positions where a `#token` can actually reference the element: id selectors (selector
/// preludes, including inside conditional group rules like `@media`) and `url(#id)` references
/// inside declaration values. A `#token` elsewhere in a declaration value is a hex color literal
/// (an id like `abc` is also a valid color), so it is left untouched, as are comments and quoted
/// strings. A match requires the exact token: `#` + \p oldId + a non-id-character boundary, so
/// `#oldIdSuffix` never matches. Returns the rewritten text if anything changed, otherwise
/// `std::nullopt`.
std::optional<std::string> RewriteIdSelectorInStyle(std::string_view value,
                                                     std::string_view oldId,
                                                     std::string_view newId);

}  // namespace donner::editor
