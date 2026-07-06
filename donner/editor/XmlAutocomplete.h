#pragma once
/// @file

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace donner::editor {

/// XML/SVG source context at an editor cursor offset.
enum class XmlAutocompleteContextKind {
  ElementName,    ///< Cursor is completing an element name after `<`.
  AttributeName,  ///< Cursor is completing an attribute name inside an open tag.
  StyleValue,     ///< Cursor is completing a CSS property name in SVG style text.
  TextContent,    ///< Cursor is in ordinary text content.
  Unknown,        ///< Cursor context is not useful for XML autocomplete.
};

/// Source replacement range and prefix for XML autocomplete.
struct XmlAutocompleteContext {
  XmlAutocompleteContextKind kind = XmlAutocompleteContextKind::Unknown;
  std::size_t replaceStartOffset = 0;  ///< Inclusive byte offset to replace.
  std::size_t replaceEndOffset = 0;    ///< Exclusive byte offset to replace.
  std::string prefix;                  ///< Already-typed completion prefix.

  /// Tag name of the element whose open tag the cursor is inside, when \ref kind is
  /// \ref XmlAutocompleteContextKind::AttributeName. Empty when the enclosing element is unknown
  /// (e.g. a context built directly rather than through \ref DetectXmlAutocompleteContext), in
  /// which case \ref BuildXmlAutocompleteSuggestions falls back to its permissive,
  /// element-insensitive attribute set.
  std::string currentTagName;
};

/// A single XML autocomplete suggestion.
struct XmlAutocompleteSuggestion {
  std::string displayText;                 ///< Text shown in the popup.
  std::string insertText;                  ///< Text inserted when selected.
  bool alsoPresentationAttribute = false;  ///< True for CSS properties usable as attributes.
};

/**
 * Detect an XML/SVG autocomplete context at a byte offset.
 *
 * This uses \ref donner::xml::Tokenize instead of regular expressions so incomplete source is
 * interpreted the same way as editor highlighting and source-aware editing.
 *
 * @param source XML/SVG source text.
 * @param cursorOffset Byte offset of the editor cursor in \p source.
 * @return Detected autocomplete context.
 */
XmlAutocompleteContext DetectXmlAutocompleteContext(std::string_view source,
                                                    std::size_t cursorOffset);

/**
 * Build SVG-aware suggestions for a detected XML context.
 *
 * Element names come from the SVG element registry; attribute names come from SVG presentation
 * attributes plus structural XML/SVG attributes; CSS names come from PropertyRegistry. Attribute
 * suggestions are element-aware for the primitive shape elements (`path`, `rect`, `circle`,
 * `ellipse`, `line`, `polygon`, `polyline`): a shape only gets its own geometry attributes (e.g. a
 * `<circle>` is offered `cx`/`cy`/`r`, not `d` or `points`), and `viewBox`/`points` are only
 * offered on elements that actually accept them, regardless of tag. Every other element keeps the
 * permissive, element-insensitive attribute set.
 *
 * @param context Context returned by \ref DetectXmlAutocompleteContext.
 * @return Matching suggestions, filtered by the context prefix.
 */
std::vector<XmlAutocompleteSuggestion> BuildXmlAutocompleteSuggestions(
    const XmlAutocompleteContext& context);

}  // namespace donner::editor
