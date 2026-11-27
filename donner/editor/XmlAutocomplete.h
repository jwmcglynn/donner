#pragma once
/// @file

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace donner::editor {

/// XML/SVG source context at an editor cursor offset.
enum class XmlAutocompleteContextKind {
  ElementName,                      ///< Cursor is completing an element name after `<`.
  AttributeName,                    ///< Cursor is completing an attribute name inside an open tag.
  CssSelector,                      ///< Cursor is in a stylesheet selector prelude.
  CssDeclarationName,               ///< Cursor is completing a declaration name.
  CssDeclarationValue,              ///< Cursor is completing a value for the active property.
  CssVariableName,                  ///< Cursor is completing the first argument of `var()`.
  CssAtRuleName,                    ///< Cursor is completing an at-rule name.
  TextContent,                      ///< Cursor is in ordinary text content.
  Unknown,                          ///< Cursor context is not useful for autocomplete.
  StyleValue = CssDeclarationName,  ///< Compatibility name for the former declaration context.
};

/// Source replacement range and prefix for XML autocomplete.
struct XmlAutocompleteContext {
  XmlAutocompleteContextKind kind =
      XmlAutocompleteContextKind::Unknown;  //!< XML syntactic context at the completion cursor.
  std::size_t replaceStartOffset = 0;       ///< Inclusive byte offset to replace.
  std::size_t replaceEndOffset = 0;         ///< Exclusive byte offset to replace.
  std::string prefix;                       ///< Already-typed completion prefix.
  std::string propertyName;                 ///< Active property for declaration-value completion.
  bool hasFollowingColon = false;  ///< The authored declaration already supplies its colon.
};

/// Live document and grammar-derived values used by CSS autocomplete.
struct XmlAutocompleteData {
  std::span<const std::string> classNames;             //!< Names without the leading dot.
  std::span<const std::string> ids;                    //!< IDs without the leading hash.
  std::span<const std::string> customPropertyNames;    //!< Authored case-sensitive `--*` names.
  std::span<const std::string_view> propertyKeywords;  //!< Keywords valid for the active property.
};

/// Bounded, deduplicated live selector names collected without splitting whole class attributes.
class XmlAutocompleteSelectorCatalog {
public:
  /// Independent count and retained-text limits shared by classes and IDs.
  struct Limits {
    std::size_t values = 4096;           //!< Maximum retained class and ID values.
    std::size_t bytes = 64 * 1024;       //!< Maximum retained spelling bytes.
    std::size_t scanBytes = 256 * 1024;  //!< Maximum inspected class and ID source bytes.
    std::size_t scanTokens = 32768;  //!< Maximum inspected nonempty names, including duplicates.
  };

  XmlAutocompleteSelectorCatalog() = default;
  explicit XmlAutocompleteSelectorCatalog(Limits limits) : limits_(limits) {}
  /// Stream whitespace-separated class names into the bounded catalog.
  void addClassList(std::string_view classList);
  /// Add one non-empty ID to the bounded catalog.
  void addId(std::string_view id);
  /// Sort retained names for deterministic suggestions.
  void finish();
  /// True once either shared limit prevents collecting another distinct value.
  bool full() const { return full_; }
  /// Sorted class names without a leading dot.
  std::span<const std::string> classNames() const { return classNames_; }
  /// Sorted IDs without a leading hash.
  std::span<const std::string> ids() const { return ids_; }

private:
  // Index the owning vector by integer position so vector reallocation never invalidates a key.
  using NameIndex = std::unordered_multimap<std::size_t, std::size_t>;
  bool addUnique(std::vector<std::string>& destination, NameIndex& index, std::string_view value);
  std::size_t admitBytes(std::size_t requested);
  static void sortAndIndex(std::vector<std::string>& names, NameIndex& index);

  Limits limits_;
  std::size_t retainedBytes_ = 0;
  std::size_t inspectedBytes_ = 0;
  std::size_t inspectedTokens_ = 0;
  NameIndex classIndex_, idIndex_;
  bool full_ = false;
  std::vector<std::string> classNames_;
  std::vector<std::string> ids_;
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
 * attributes plus structural XML/SVG attributes; CSS names come from PropertyRegistry.
 *
 * @param context Context returned by \ref donner::editor::DetectXmlAutocompleteContext
 * "DetectXmlAutocompleteContext".
 * @param data Live document and property-grammar values for CSS contexts.
 * @return Matching suggestions, filtered, deduplicated, stable, and capped at 256.
 */
std::vector<XmlAutocompleteSuggestion> BuildXmlAutocompleteSuggestions(
    const XmlAutocompleteContext& context, const XmlAutocompleteData& data = {});

}  // namespace donner::editor
