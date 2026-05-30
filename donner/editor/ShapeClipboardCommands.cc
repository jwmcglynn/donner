#include "donner/editor/ShapeClipboardCommands.h"

#include <cctype>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/svg/SVGGeometryElement.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::editor {

namespace {

/// Resolve a FileOffset against \p source to an absolute byte offset.
std::optional<std::size_t> ResolveFileOffset(std::string_view source, const FileOffset& offset) {
  const FileOffset resolved = offset.resolveOffset(source);
  if (!resolved.offset.has_value()) {
    return std::nullopt;
  }
  return std::min(*resolved.offset, source.size());
}

/// Half-open `[start, end)` byte range of \p element's serialized node in
/// \p source, or std::nullopt if the element has no source mapping.
std::optional<std::pair<std::size_t, std::size_t>> elementSourceRange(
    const svg::SVGElement& element, std::string_view source) {
  return element.withReadAccess(
      [source](svg::DocumentReadAccess&,
               EntityHandle handle) -> std::optional<std::pair<std::size_t, std::size_t>> {
        auto xmlNode = xml::XMLNode::TryCast(handle);
        if (!xmlNode.has_value()) {
          return std::nullopt;
        }
        auto range = xmlNode->getNodeLocation();
        if (!range.has_value()) {
          return std::nullopt;
        }
        const std::optional<std::size_t> start = ResolveFileOffset(source, range->start);
        const std::optional<std::size_t> end = ResolveFileOffset(source, range->end);
        if (!start.has_value() || !end.has_value() || *end <= *start) {
          return std::nullopt;
        }
        return std::make_pair(*start, *end);
      });
}

/// Recursively collect every `id` attribute in the subtree rooted at \p element.
void collectIds(const svg::SVGElement& element, std::unordered_set<std::string>& out) {
  const RcString id = element.id();
  if (!id.empty()) {
    out.insert(id.str());
  }
  for (auto child = element.firstChild(); child; child = child->nextSibling()) {
    collectIds(*child, out);
  }
}

/// Whether \p c can appear in an SVG id / fragment identifier token.
bool isIdChar(char c) {
  return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '-' || c == '_' || c == '.' ||
         c == ':';
}

/// Whether the byte before \p pos in \p text is a token boundary (start of
/// string or ASCII whitespace) so `id=` is a standalone attribute rather than a
/// suffix of e.g. `gradientid=`.
bool boundaryBefore(std::string_view text, std::size_t pos) {
  return pos == 0 || text[pos - 1] == ' ' || text[pos - 1] == '\t' || text[pos - 1] == '\n' ||
         text[pos - 1] == '\r';
}

/// Scan \p text for every `id="..."` attribute value and append them to \p out.
void scanDefinedIds(std::string_view text, std::vector<std::string>& out) {
  std::size_t pos = 0;
  while ((pos = text.find("id=", pos)) != std::string_view::npos) {
    const std::size_t valueStart = pos + 3;
    if (!boundaryBefore(text, pos) || valueStart >= text.size() ||
        (text[valueStart] != '"' && text[valueStart] != '\'')) {
      pos = valueStart;
      continue;
    }
    const char quote = text[valueStart];
    const std::size_t end = text.find(quote, valueStart + 1);
    if (end == std::string_view::npos) {
      break;
    }
    out.emplace_back(text.substr(valueStart + 1, end - (valueStart + 1)));
    pos = end + 1;
  }
}

/// Find every internal reference target (`#id`) used in \p text, via
/// `href="#id"`, `xlink:href="#id"`, and `url(#id)`.
void scanReferencedIds(std::string_view text, std::unordered_set<std::string>& out) {
  // url(#id)
  std::size_t pos = 0;
  while ((pos = text.find("url(#", pos)) != std::string_view::npos) {
    const std::size_t start = pos + 5;
    std::size_t end = start;
    while (end < text.size() && isIdChar(text[end])) {
      ++end;
    }
    if (end > start) {
      out.emplace(text.substr(start, end - start));
    }
    pos = end;
  }
  // href="#id" and xlink:href="#id" (both end in `href=`).
  pos = 0;
  while ((pos = text.find("href=", pos)) != std::string_view::npos) {
    const std::size_t valueStart = pos + 5;
    if (valueStart < text.size() && (text[valueStart] == '"' || text[valueStart] == '\'') &&
        valueStart + 1 < text.size() && text[valueStart + 1] == '#') {
      const char quote = text[valueStart];
      const std::size_t start = valueStart + 2;
      const std::size_t end = text.find(quote, start);
      if (end != std::string_view::npos) {
        out.emplace(text.substr(start, end - start));
      }
    }
    pos = valueStart;
  }
}

/// Rewrite every `id="oldId"`, `url(#oldId)`, and `href="#oldId"` occurrence to
/// the renamed id from \p renames. Tokens with no rename are left untouched.
std::string applyRenames(std::string_view text,
                         const std::unordered_map<std::string, std::string>& renames) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    bool rewrote = false;

    // id="oldId" / id='oldId'
    if (text.compare(i, 3, "id=") == 0 && boundaryBefore(text, i)) {
      const std::size_t valueStart = i + 3;
      if (valueStart < text.size() && (text[valueStart] == '"' || text[valueStart] == '\'')) {
        const char quote = text[valueStart];
        const std::size_t end = text.find(quote, valueStart + 1);
        if (end != std::string_view::npos) {
          const std::string value(text.substr(valueStart + 1, end - (valueStart + 1)));
          auto it = renames.find(value);
          out.append("id=");
          out.push_back(quote);
          out.append(it != renames.end() ? it->second : value);
          out.push_back(quote);
          i = end + 1;
          rewrote = true;
        }
      }
    }

    // url(#oldId)
    if (!rewrote && text.compare(i, 5, "url(#") == 0) {
      const std::size_t start = i + 5;
      std::size_t end = start;
      while (end < text.size() && isIdChar(text[end])) {
        ++end;
      }
      const std::string value(text.substr(start, end - start));
      auto it = renames.find(value);
      out.append("url(#");
      out.append(it != renames.end() ? it->second : value);
      i = end;
      rewrote = true;
    }

    // href="#oldId" / xlink:href="#oldId"
    if (!rewrote && text.compare(i, 5, "href=") == 0) {
      const std::size_t valueStart = i + 5;
      if (valueStart + 1 < text.size() && (text[valueStart] == '"' || text[valueStart] == '\'') &&
          text[valueStart + 1] == '#') {
        const char quote = text[valueStart];
        const std::size_t start = valueStart + 2;
        const std::size_t end = text.find(quote, start);
        if (end != std::string_view::npos) {
          const std::string value(text.substr(start, end - start));
          auto it = renames.find(value);
          out.append("href=");
          out.push_back(quote);
          out.push_back('#');
          out.append(it != renames.end() ? it->second : value);
          out.push_back(quote);
          i = end + 1;
          rewrote = true;
        }
      }
    }

    if (!rewrote) {
      out.push_back(text[i]);
      ++i;
    }
  }
  return out;
}

/// Generate a unique id from \p base by appending `_pasted`, `_pasted2`, … until
/// it does not appear in \p taken. The chosen id is inserted into \p taken.
std::string makePastedId(const std::string& base, std::unordered_set<std::string>& taken) {
  const std::string root = base.empty() ? "shape" : base;
  std::string candidate = root + "_pasted";
  int counter = 2;
  while (taken.count(candidate) != 0) {
    candidate = root + "_pasted" + std::to_string(counter);
    ++counter;
  }
  taken.insert(candidate);
  return candidate;
}

/// Combined world-space bounds of any geometry elements in \p selection.
std::optional<Box2d> combinedSelectionBounds(const std::vector<svg::SVGElement>& selection) {
  std::optional<Box2d> combined;
  for (const auto& element : selection) {
    auto geometry = element;
    auto geo = geometry.tryCast<svg::SVGGeometryElement>();
    if (!geo.has_value()) {
      continue;
    }
    auto bounds = geo->worldBounds();
    if (!bounds.has_value()) {
      continue;
    }
    if (!combined.has_value()) {
      combined = *bounds;
    } else {
      combined->addPoint(bounds->topLeft);
      combined->addPoint(bounds->bottomRight);
    }
  }
  return combined;
}

}  // namespace

std::optional<ShapeClipboardPayload> copySelectionToPayload(
    const svg::SVGDocument& document, const std::vector<svg::SVGElement>& selection) {
  if (selection.empty()) {
    return std::nullopt;
  }

  const std::string_view source = document.source();

  ShapeClipboardPayload payload;
  std::string fragment;
  bool allGroups = true;
  bool serializedAny = false;

  for (const auto& element : selection) {
    auto range = elementSourceRange(element, source);
    if (!range.has_value()) {
      // No source mapping for this element. Skip it; the payload still carries
      // any siblings that did serialize. (A temporary-doc round-trip fallback
      // can be wired in later for programmatic-only elements.)
      continue;
    }
    fragment.append(source.substr(range->first, range->second - range->first));
    fragment.push_back('\n');
    payload.sourceElementIds.emplace_back(element.id().str());
    serializedAny = true;

    const xml::XMLQualifiedNameRef tag = element.tagName();
    if (tag.name != "g" && tag.name != "svg") {
      allGroups = false;
    }
  }

  if (!serializedAny) {
    return std::nullopt;
  }

  payload.svgFragment = std::move(fragment);
  payload.wasGroupSelection = allGroups;
  payload.documentBounds = combinedSelectionBounds(selection);
  return payload;
}

PreparePasteResult preparePaste(const svg::SVGDocument& document,
                                const ShapeClipboardPayload& payload, PastePlacement placement,
                                const std::optional<svg::SVGElement>& selectedGroup) {
  PreparePasteResult result;

  // Validate the fragment by parsing it inside a synthetic root first, so a
  // malformed clipboard never mutates the destination document.
  {
    std::string wrapped =
        "<svg xmlns=\"http://www.w3.org/2000/svg\" "
        "xmlns:xlink=\"http://www.w3.org/1999/xlink\">";
    wrapped += payload.svgFragment;
    wrapped += "</svg>";

    ParseWarningSink sink;
    auto parsed = svg::parser::SVGParser::ParseSVG(wrapped, sink);
    if (parsed.hasError()) {
      result.error = "Paste failed: clipboard does not contain valid SVG.";
      return result;
    }
  }

  // Enumerate ids defined in the fragment and ids referenced by the fragment.
  std::vector<std::string> fragmentIds;
  scanDefinedIds(payload.svgFragment, fragmentIds);
  std::unordered_set<std::string> fragmentIdSet(fragmentIds.begin(), fragmentIds.end());

  std::unordered_set<std::string> referencedIds;
  scanReferencedIds(payload.svgFragment, referencedIds);

  // Collect ids already present in the destination document.
  std::unordered_set<std::string> existingIds;
  collectIds(document.svgElement(), existingIds);

  // A reference to an id that is neither defined in the fragment nor present in
  // the destination document cannot be repaired safely → fail without mutating.
  for (const auto& ref : referencedIds) {
    if (fragmentIdSet.count(ref) == 0 && existingIds.count(ref) == 0) {
      result.error = "Paste failed: clipboard references missing element '#" + ref + "'.";
      return result;
    }
  }

  // Rename colliding fragment ids deterministically. `taken` tracks ids that
  // must stay unique across the destination and the freshly-assigned set.
  std::unordered_set<std::string> taken = existingIds;
  for (const std::string& id : fragmentIds) {
    taken.insert(id);
  }
  std::unordered_map<std::string, std::string> renames;
  for (const std::string& id : fragmentIds) {
    if (existingIds.count(id) != 0) {
      taken.erase(id);
      renames[id] = makePastedId(id, taken);
    }
  }

  const std::string repairedFragment =
      renames.empty() ? payload.svgFragment : applyRenames(payload.svgFragment, renames);

  // Wrapper group id, unique against everything else.
  const std::string wrapperId = makePastedId("donner-paste", taken);

  // Ids the caller should select after the reparse: the (possibly renamed) ids
  // of the top-level fragment elements. If none have ids, select the wrapper.
  for (const std::string& id : payload.sourceElementIds) {
    if (id.empty()) {
      continue;
    }
    auto it = renames.find(id);
    result.pastedElementIds.push_back(it != renames.end() ? it->second : id);
  }
  if (result.pastedElementIds.empty()) {
    result.pastedElementIds.push_back(wrapperId);
  }

  // Build the wrapper group carrying the repaired fragment.
  std::string wrapperOpen = "<g id=\"" + wrapperId + "\"";
  if (placement == PastePlacement::EndOfRootOffset) {
    wrapperOpen += " transform=\"translate(20,20)\"";
  }
  wrapperOpen += ">";
  const std::string wrapperBlock = wrapperOpen + "\n" + repairedFragment + "\n</g>\n";

  // Splice the wrapper into the destination document source.
  std::string source(document.source());
  if (source.empty()) {
    result.error = "Paste failed: destination document has no source text.";
    return result;
  }

  const std::size_t rootClose = source.rfind("</svg>");
  if (rootClose == std::string::npos) {
    result.error = "Paste failed: destination document has no root <svg> element.";
    return result;
  }
  std::size_t insertAt = rootClose;

  // Default paste into a single selected group: insert just before that group's
  // matching `</g>` so the paste lands inside the group's subtree.
  if (placement == PastePlacement::EndOfRootOffset && selectedGroup.has_value() &&
      !selectedGroup->id().empty()) {
    const std::string anchor = "id=\"" + selectedGroup->id().str() + "\"";
    const std::size_t idPos = source.find(anchor);
    if (idPos != std::string::npos) {
      const std::size_t tagOpen = source.rfind('<', idPos);
      if (tagOpen != std::string::npos && source.compare(tagOpen, 2, "<g") == 0) {
        std::size_t depth = 0;
        std::size_t scan = tagOpen;
        std::size_t matchedClose = std::string::npos;
        while (scan < source.size()) {
          if (source.compare(scan, 2, "<g") == 0 &&
              (scan + 2 >= source.size() || source[scan + 2] == ' ' || source[scan + 2] == '>' ||
               source[scan + 2] == '\t' || source[scan + 2] == '\n')) {
            ++depth;
            scan += 2;
          } else if (source.compare(scan, 4, "</g>") == 0) {
            --depth;
            if (depth == 0) {
              matchedClose = scan;
              break;
            }
            scan += 4;
          } else {
            ++scan;
          }
        }
        if (matchedClose != std::string::npos) {
          insertAt = matchedClose;
        }
      }
    }
  }

  // Paste-in-Front: insert above the source elements in paint order. The
  // wrapper is appended at the end of the root (last sibling = painted last =
  // in front), which already satisfies "in front" for the common case where the
  // sources live in the root. We keep the no-offset wrapper at `insertAt`.

  source.insert(insertAt, wrapperBlock);

  result.ok = true;
  result.mergedSource = std::move(source);
  return result;
}

}  // namespace donner::editor
