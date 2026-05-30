#include "donner/editor/ViewportSvgExport.h"

#include <array>
#include <cmath>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace donner::editor {

namespace {

/// Identifier for the clip path injected into exported documents.
constexpr std::string_view kClipPathId = "donner-viewport-clip";
/// Identifier for the (M6: empty) editor overlay placeholder group.
constexpr std::string_view kOverlayGroupId = "donner-editor-overlay";

/// Format a double for SVG attribute output, trimming trailing zeros so
/// `100.0` becomes `100` and `12.50` becomes `12.5`. Determinism matters: the
/// exported `viewBox` is asserted against `screenToDocument(renderPaneRect)`.
std::string FormatNumber(double value) {
  if (!std::isfinite(value)) {
    return "0";
  }
  std::ostringstream stream;
  stream.precision(6);
  stream << std::fixed << value;
  std::string text = stream.str();
  if (text.find('.') != std::string::npos) {
    while (!text.empty() && text.back() == '0') {
      text.pop_back();
    }
    if (!text.empty() && text.back() == '.') {
      text.pop_back();
    }
  }
  if (text == "-0") {
    return "0";
  }
  return text;
}

/// Escape a string for inclusion in an XML attribute value or comment body.
std::string EscapeXml(std::string_view input) {
  std::string output;
  output.reserve(input.size());
  for (const char ch : input) {
    switch (ch) {
      case '&': output += "&amp;"; break;
      case '<': output += "&lt;"; break;
      case '>': output += "&gt;"; break;
      case '"': output += "&quot;"; break;
      case '\'': output += "&apos;"; break;
      default: output += ch; break;
    }
  }
  return output;
}

/// Case-insensitive ASCII comparison of \p haystack starting at \p pos against
/// \p needle.
bool MatchesAtCaseInsensitive(std::string_view haystack, std::size_t pos, std::string_view needle) {
  if (pos + needle.size() > haystack.size()) {
    return false;
  }
  for (std::size_t i = 0; i < needle.size(); ++i) {
    char a = haystack[pos + i];
    char b = needle[i];
    if (a >= 'A' && a <= 'Z') {
      a = static_cast<char>(a - 'A' + 'a');
    }
    if (b >= 'A' && b <= 'Z') {
      b = static_cast<char>(b - 'A' + 'a');
    }
    if (a != b) {
      return false;
    }
  }
  return true;
}

/// A single `name="value"` attribute parsed from an element open tag.
struct Attribute {
  std::string name;
  std::string value;  ///< Raw value bytes as they appeared in the source.
};

/// Parsed root `<svg ...>` open tag.
struct RootTag {
  std::vector<Attribute> attributes;
  std::size_t bodyStart = 0;  ///< Byte offset just past the root open tag.
  std::size_t bodyEnd = 0;    ///< Byte offset of the root `</svg>` close tag.
  bool found = false;
};

/// Find the byte offset of the root `<svg` open tag, skipping the XML
/// declaration, comments, doctype, and processing instructions.
std::size_t FindRootSvgStart(std::string_view source) {
  std::size_t pos = 0;
  while (pos < source.size()) {
    if (source[pos] != '<') {
      ++pos;
      continue;
    }
    if (MatchesAtCaseInsensitive(source, pos, "<!--")) {
      const std::size_t end = source.find("-->", pos + 4);
      pos = (end == std::string_view::npos) ? source.size() : end + 3;
      continue;
    }
    if (pos + 1 < source.size() && (source[pos + 1] == '?' || source[pos + 1] == '!')) {
      const std::size_t end = source.find('>', pos);
      pos = (end == std::string_view::npos) ? source.size() : end + 1;
      continue;
    }
    if (MatchesAtCaseInsensitive(source, pos, "<svg") && pos + 4 < source.size()) {
      const char after = source[pos + 4];
      if (after == ' ' || after == '\t' || after == '\n' || after == '\r' || after == '>' ||
          after == '/') {
        return pos;
      }
    }
    ++pos;
  }
  return std::string_view::npos;
}

/// Parse the root `<svg>` open tag attributes and locate the body bounds.
RootTag ParseRootTag(std::string_view source) {
  RootTag result;
  const std::size_t start = FindRootSvgStart(source);
  if (start == std::string_view::npos) {
    return result;
  }

  // Advance past "<svg".
  std::size_t pos = start + 4;
  // Parse attributes until the end of the open tag ('>' possibly preceded by '/').
  while (pos < source.size()) {
    // Skip whitespace.
    while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' ||
                                   source[pos] == '\n' || source[pos] == '\r')) {
      ++pos;
    }
    if (pos >= source.size()) {
      return result;  // Malformed: no '>'.
    }
    if (source[pos] == '>' || source[pos] == '/') {
      break;
    }

    // Parse attribute name.
    const std::size_t nameStart = pos;
    while (pos < source.size() && source[pos] != '=' && source[pos] != ' ' && source[pos] != '\t' &&
           source[pos] != '\n' && source[pos] != '\r' && source[pos] != '>' && source[pos] != '/') {
      ++pos;
    }
    Attribute attribute;
    attribute.name.assign(source.substr(nameStart, pos - nameStart));

    // Skip whitespace before '='.
    while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' ||
                                   source[pos] == '\n' || source[pos] == '\r')) {
      ++pos;
    }
    if (pos < source.size() && source[pos] == '=') {
      ++pos;  // Past '='.
      while (pos < source.size() && (source[pos] == ' ' || source[pos] == '\t' ||
                                     source[pos] == '\n' || source[pos] == '\r')) {
        ++pos;
      }
      if (pos < source.size() && (source[pos] == '"' || source[pos] == '\'')) {
        const char quote = source[pos];
        ++pos;
        const std::size_t valueStart = pos;
        while (pos < source.size() && source[pos] != quote) {
          ++pos;
        }
        attribute.value.assign(source.substr(valueStart, pos - valueStart));
        if (pos < source.size()) {
          ++pos;  // Past closing quote.
        }
      }
    }
    if (!attribute.name.empty()) {
      result.attributes.push_back(std::move(attribute));
    }
  }

  // `pos` is at '>' or '/'. Find the end of the open tag.
  const std::size_t openTagEnd = source.find('>', pos);
  if (openTagEnd == std::string_view::npos) {
    return result;
  }
  const bool selfClosing = openTagEnd > 0 && source[openTagEnd - 1] == '/';
  result.bodyStart = openTagEnd + 1;
  if (selfClosing) {
    result.bodyEnd = openTagEnd - 1;  // No children.
  } else {
    const std::size_t closeTag = source.rfind("</svg");
    result.bodyEnd = (closeTag == std::string_view::npos || closeTag < result.bodyStart)
                         ? source.size()
                         : closeTag;
  }
  result.found = true;
  return result;
}

/// Scan the source for external `href` / `xlink:href` references over
/// `http://`, `https://`, or `file://`. Returns the offending value, or empty.
std::string FindExternalReference(std::string_view source) {
  static constexpr std::array<std::string_view, 3> kExternalSchemes = {
      "http://",
      "https://",
      "file://",
  };
  std::size_t pos = 0;
  while (pos < source.size()) {
    const std::size_t hrefPos = source.find("href", pos);
    if (hrefPos == std::string_view::npos) {
      break;
    }
    // Find the value following `href[whitespace]=[whitespace]quote...quote`.
    std::size_t cursor = hrefPos + 4;
    while (cursor < source.size() && (source[cursor] == ' ' || source[cursor] == '\t' ||
                                      source[cursor] == '\n' || source[cursor] == '\r')) {
      ++cursor;
    }
    if (cursor >= source.size() || source[cursor] != '=') {
      pos = hrefPos + 4;
      continue;
    }
    ++cursor;
    while (cursor < source.size() && (source[cursor] == ' ' || source[cursor] == '\t' ||
                                      source[cursor] == '\n' || source[cursor] == '\r')) {
      ++cursor;
    }
    if (cursor >= source.size() || (source[cursor] != '"' && source[cursor] != '\'')) {
      pos = hrefPos + 4;
      continue;
    }
    const char quote = source[cursor];
    ++cursor;
    const std::size_t valueStart = cursor;
    while (cursor < source.size() && source[cursor] != quote) {
      ++cursor;
    }
    const std::string_view value = source.substr(valueStart, cursor - valueStart);
    // Skip leading whitespace inside the value when scheme-matching.
    std::size_t valueOffset = 0;
    while (valueOffset < value.size() &&
           (value[valueOffset] == ' ' || value[valueOffset] == '\t')) {
      ++valueOffset;
    }
    for (const std::string_view scheme : kExternalSchemes) {
      if (MatchesAtCaseInsensitive(value, valueOffset, scheme)) {
        return std::string(value);
      }
    }
    pos = (cursor < source.size()) ? cursor + 1 : source.size();
  }
  return std::string();
}

}  // namespace

Result<std::string, std::string> ExportViewportAsSvg(const svg::SVGDocument& doc,
                                                     const ViewportState& viewport,
                                                     const Recti& renderPaneRect,
                                                     const ViewportExportOptions& options) {
  using ResultType = Result<std::string, std::string>;

  if (!doc.hasSourceStore()) {
    return ResultType::Err("Viewport export requires a document with an XML source store.");
  }

  const std::string_view source = doc.source();

  // Refuse documents that reference external resources we cannot embed safely.
  const std::string externalReference = FindExternalReference(source);
  if (!externalReference.empty()) {
    return ResultType::Err(
        "Viewport export cannot embed external resource reference: " + externalReference +
        ". Inline or remove external href/xlink:href references and try again.");
  }

  const RootTag rootTag = ParseRootTag(source);
  if (!rootTag.found) {
    return ResultType::Err("Viewport export could not find the root <svg> element in the source.");
  }

  // Compute the document-space viewport rect from the screen-space pane rect.
  // `ViewportState` is the single source of truth for crop and scale.
  const Box2d paneScreenBox(Vector2d(static_cast<double>(renderPaneRect.topLeft.x),
                                     static_cast<double>(renderPaneRect.topLeft.y)),
                            Vector2d(static_cast<double>(renderPaneRect.bottomRight.x),
                                     static_cast<double>(renderPaneRect.bottomRight.y)));
  const Box2d documentViewportBox = viewport.screenToDocument(paneScreenBox);

  const double viewBoxMinX = documentViewportBox.topLeft.x;
  const double viewBoxMinY = documentViewportBox.topLeft.y;
  const double viewBoxWidth = documentViewportBox.width();
  const double viewBoxHeight = documentViewportBox.height();

  // Output dimensions match the render pane size in CSS pixels.
  const int outputWidth = renderPaneRect.bottomRight.x - renderPaneRect.topLeft.x;
  const int outputHeight = renderPaneRect.bottomRight.y - renderPaneRect.topLeft.y;

  const std::string viewBoxValue = FormatNumber(viewBoxMinX) + " " + FormatNumber(viewBoxMinY) +
                                   " " + FormatNumber(viewBoxWidth) + " " +
                                   FormatNumber(viewBoxHeight);

  // Source document name for provenance metadata (no absolute local paths).
  std::string sourceName = "untitled";
  {
    // Prefer an explicit id/name-ish attribute? The design doc only requires a
    // source document name. We do not have a filename here (export takes the
    // document, not a path), so use the root element id when present, else
    // "untitled". This keeps metadata free of absolute local paths.
    for (const Attribute& attribute : rootTag.attributes) {
      if (attribute.name == "id" && !attribute.value.empty()) {
        sourceName = attribute.value;
        break;
      }
    }
  }

  std::string output;
  output.reserve(source.size() + 1024);

  output += "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
  output += "<!-- Generated by Donner SVG Editor & Toolkit; source: ";
  output += EscapeXml(sourceName);
  output += "; viewBox: ";
  output += EscapeXml(viewBoxValue);
  output += " -->\n";

  // Root open tag: carry source root attributes, replacing viewBox/width/height.
  output += "<svg";
  bool wroteViewBox = false;
  bool wroteWidth = false;
  bool wroteHeight = false;
  for (const Attribute& attribute : rootTag.attributes) {
    if (attribute.name == "viewBox") {
      output += " viewBox=\"" + EscapeXml(viewBoxValue) + "\"";
      wroteViewBox = true;
    } else if (attribute.name == "width") {
      output += " width=\"" + std::to_string(outputWidth) + "\"";
      wroteWidth = true;
    } else if (attribute.name == "height") {
      output += " height=\"" + std::to_string(outputHeight) + "\"";
      wroteHeight = true;
    } else {
      output += " " + attribute.name + "=\"" + EscapeXml(attribute.value) + "\"";
    }
  }
  if (!wroteWidth) {
    output += " width=\"" + std::to_string(outputWidth) + "\"";
  }
  if (!wroteHeight) {
    output += " height=\"" + std::to_string(outputHeight) + "\"";
  }
  if (!wroteViewBox) {
    output += " viewBox=\"" + EscapeXml(viewBoxValue) + "\"";
  }
  output += ">\n";

  // Defs: clip path covering the document-space viewport rect.
  output += "  <defs><clipPath id=\"";
  output += kClipPathId;
  output += "\"><rect x=\"" + FormatNumber(viewBoxMinX) + "\" y=\"" + FormatNumber(viewBoxMinY) +
            "\" width=\"" + FormatNumber(viewBoxWidth) + "\" height=\"" +
            FormatNumber(viewBoxHeight) + "\"/></clipPath></defs>\n";

  // Optional covering background rect (non-transparent export).
  if (!options.transparentBackground) {
    output += "  <rect x=\"" + FormatNumber(viewBoxMinX) + "\" y=\"" + FormatNumber(viewBoxMinY) +
              "\" width=\"" + FormatNumber(viewBoxWidth) + "\" height=\"" +
              FormatNumber(viewBoxHeight) + "\" fill=\"#ffffff\"/>\n";
  }

  // Document content: source children verbatim, wrapped in a clipped group.
  output += "  <g clip-path=\"url(#";
  output += kClipPathId;
  output += ")\">";
  if (rootTag.bodyEnd > rootTag.bodyStart) {
    output += source.substr(rootTag.bodyStart, rootTag.bodyEnd - rootTag.bodyStart);
  }
  output += "</g>\n";

  // Optional editor overlay placeholder. In Milestone 6 this group is always
  // empty; Milestone 7's OverlayRenderer SVG emit populates it.
  if (options.includeSelectionOverlay) {
    output += "  <g id=\"";
    output += kOverlayGroupId;
    output += "\" data-donner-export-role=\"editor-overlay\" pointer-events=\"none\">";
    // TODO(M7): overlay primitives populated by OverlayRenderer SVG emit
    output += "</g>\n";
  }

  output += "</svg>\n";

  return ResultType::Ok(std::move(output));
}

}  // namespace donner::editor
