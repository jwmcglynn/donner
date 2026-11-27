#pragma once
/// @file

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseWarningSink.h"
#include "donner/base/Utils.h"
#include "donner/css/Stylesheet.h"
#include "donner/svg/components/style/RuleIndex.h"
#include "donner/svg/components/style/SelectorDependencySummary.h"

namespace donner::svg::components {

/// Maps a contiguous span of parsed CSS text back to the SVG document source.
struct StylesheetSourceMapSegment {
  std::size_t cssStartOffset = 0;  ///< First local CSS byte covered by this segment.
  std::size_t cssEndOffset = 0;    ///< First local CSS byte after this segment.
  FileOffset documentStartOffset{
      FileOffset::Offset(0)};  ///< Document source offset for \ref cssStartOffset.

  /// Equality operator.
  bool operator==(const StylesheetSourceMapSegment& other) const = default;
};

/// Source map for stylesheet text produced by concatenating `<style>` text/CDATA children.
class StylesheetSourceMap {
public:
  /**
   * Add one local-CSS to document-source mapping segment.
   *
   * @param cssStartOffset First local CSS byte covered by this segment.
   * @param cssEndOffset First local CSS byte after this segment.
   * @param documentStartOffset Document source offset for \p cssStartOffset.
   */
  void addSegment(std::size_t cssStartOffset, std::size_t cssEndOffset,
                  FileOffset documentStartOffset);

  /**
   * Record the length of the concatenated CSS text this map describes.
   *
   * Point diagnostics at the end of the last mapped segment map to that segment's end only when
   * it is also the end of the stylesheet; a later chunk without source provenance makes that point
   * unmappable instead.
   *
   * @param cssLength Length in bytes of the concatenated CSS text.
   */
  void setCssLength(std::size_t cssLength) { cssLength_ = cssLength; }

  /**
   * Return true when no local CSS offsets can be mapped to document source.
   */
  bool empty() const { return segments_.empty(); }

  /**
   * Map a local CSS source range back to the SVG document source.
   *
   * @param localRange Range in the parsed CSS string.
   * @return Document source range, or \c std::nullopt when either endpoint is unmappable.
   */
  std::optional<SourceRange> mapToDocumentSource(const SourceRange& localRange) const;

  /**
   * Map a point diagnostic, preferring the next chunk at a shared CSS boundary. The stylesheet
   * end maps to the end of the last segment only when \ref setCssLength recorded that the last
   * segment reaches it.
   *
   * @param localOffset Point in the parsed CSS string.
   * @return Document offset, or nullopt when no source segment covers the point.
   */
  std::optional<FileOffset> mapPointToDocumentSource(FileOffset localOffset) const;

  /**
   * Map a document source offset back to this stylesheet's local CSS offset.
   *
   * @param documentOffset Offset in the SVG document source.
   * @return Local CSS offset, or \c std::nullopt if \p documentOffset is outside this stylesheet.
   */
  std::optional<std::size_t> mapToLocalCssOffset(std::size_t documentOffset) const;

  /**
   * Get the stored mapping segments.
   */
  std::span<const StylesheetSourceMapSegment> segments() const UTILS_LIFETIME_BOUND {
    return segments_;
  }

private:
  std::optional<FileOffset> mapOffset(const FileOffset& localOffset) const;

  std::vector<StylesheetSourceMapSegment> segments_;
  std::optional<std::size_t> cssLength_;  ///< Concatenated CSS length, when recorded.
};

/// Identifies a successful import occurrence in an imports-first flattened list.
struct StylesheetImportOrigin {
  std::size_t importIndex;  ///< Index in the importing sheet's retained import stream.
  std::optional<std::size_t> parentEntry;  ///< Flattened parent, or the root stylesheet.
};

/**
 * Data for a \ref xml_style element.
 *
 * See https://www.w3.org/TR/SVG2/styling.html#StyleElement
 */
struct StylesheetComponent {
  css::Stylesheet stylesheet;     ///< The parsed stylesheet from the \ref xml_style element.
  RcString text;                  ///< The raw CSS text the \ref stylesheet was parsed from.
  RcString type;                  ///< The type attribute of the \ref xml_style element.
  StylesheetSourceMap sourceMap;  ///< Optional mapping from CSS offsets to SVG document source.

  /// True if this is a user agent stylesheet, which is defined by the SVG specification and applied
  /// by default by the document. If set, this component is attached to an \ref xml_svg element
  /// instead. The stylesheet within is applied at lower priority, and should be considered a
  /// constant for Donner (it can be overridden, but the base style cannot be changed by the user).
  bool isUserAgentStylesheet = false;

  /**
   * Stylesheets loaded through `@import`, flattened in cascade order: each imported sheet follows
   * the sheets it imports, and all of them precede \ref stylesheet's own rules. Populated by
   * \ref ResourceManagerContext::loadStylesheetImports when the document is prepared for
   * rendering, and cleared whenever \ref stylesheet is reparsed. Imported rules have no document
   * source provenance, so source-mapped editor queries consider only \ref stylesheet.
   */
  std::vector<css::Stylesheet> importedStylesheets;

  /// Positions of successful loads in their original import streams.
  std::vector<StylesheetImportOrigin> importOrigins;

  /// Candidate-rule index of \ref stylesheet, rebuilt whenever it or its imports change.
  RuleIndex ruleIndex;

  /// Identity of this revision of the document and imported rules.
  std::uint64_t contentRevision() const { return contentRevision_; }

  /// Candidate-rule index of each sheet in \ref importedStylesheets, in the same order.
  std::vector<RuleIndex> importedRuleIndexes;

  /// Which elements a change to each id, class and attribute name named by \ref stylesheet or
  /// \ref importedStylesheets can restyle, rebuilt whenever they change.
  SelectorDependencySummary dependencySummary;

  /// Bytes retained by \ref ruleIndex, \ref importedRuleIndexes and \ref dependencySummary,
  /// charged with the stylesheet.
  std::size_t selectorCacheBytes() const;

  /// Font faces of \ref importedStylesheets followed by \ref stylesheet, populated only when
  /// imports contributed any; see \ref cascadeFontFaces().
  std::vector<css::FontFace> combinedFontFaces;

  /// Incremented whenever \ref cascadeFontFaces may have changed, so font registration notices
  /// new contents even if their storage address and size are reused.
  std::uint64_t fontFaceRevision = 0;

  /// Resource-manager generation for which \ref importedStylesheets was resolved, or nullopt when
  /// the imports of the current \ref stylesheet have not been resolved yet.
  std::optional<std::uint64_t> importGeneration;

  /// Font faces contributed by this active CSS sheet, including imported ones.
  std::span<const css::FontFace> cascadeFontFaces() const UTILS_LIFETIME_BOUND {
    if (!isCssType()) {
      return {};
    }
    return combinedFontFaces.empty() ? stylesheet.fontFaces()
                                     : std::span<const css::FontFace>(combinedFontFaces);
  }

  /**
   * Install the resolved imports of \ref stylesheet, updating the selector-dependency caches so
   * that imported selectors also invalidate styles.
   *
   * @param imported Imported stylesheets in cascade order.
   * @param generation Resource-manager generation the imports were resolved for.
   * @param origins Original positions of the successful imports, parallel to \p imported.
   */
  void setImportedStylesheets(std::vector<css::Stylesheet>&& imported, std::uint64_t generation,
                              std::vector<StylesheetImportOrigin> origins = {});

  /**
   * Returns true if the \ref xml_style element has either no `type` attribute, or if it has been
   * manually set to "text/css".
   */
  bool isCssType() const { return type.empty() || type.equalsIgnoreCase("text/css"); }

  /**
   * Returns true if this stylesheet contains an attribute selector that could match an attribute
   * with the given local name, meaning writing that attribute can change which elements the
   * stylesheet matches.
   *
   * Deliberately per-name rather than a single "uses any attribute selector" flag: the user agent
   * stylesheet always contains one (`*[xml|space=preserve]`), so a single flag would be set for
   * every document.
   *
   * @param localName Attribute local name, compared case-insensitively to over-approximate.
   */
  bool usesAttributeInSelector(std::string_view localName) const {
    if (!isCssType()) {
      return false;
    }
    return dependencySummary.usesAttribute(localName);
  }

  /**
   * Parse the contents of the \ref xml_style element.
   *
   * @param str The contents of the \ref xml_style element.
   */
  void parseStylesheet(const RcStringOrRef& str);

  /**
   * Parse the contents of the \ref xml_style element with source provenance.
   *
   * @param str The contents of the \ref xml_style element.
   * @param sourceMap Source map for local CSS offsets in \p str.
   */
  void parseStylesheet(const RcStringOrRef& str, StylesheetSourceMap sourceMap);
  /**
   * Parse stylesheet contents while collecting CSS-local diagnostics.
   *
   * @param str Stylesheet contents.
   * @param sourceMap Source provenance for the retained stylesheet.
   * @param warnings Bounded sink receiving recoverable CSS diagnostics.
   */
  void parseStylesheet(const RcStringOrRef& str, StylesheetSourceMap sourceMap,
                       ParseWarningSink& warnings);

  /**
   * Install a prepared rule-window splice from the previous projection, when supported.
   * @param previous Previous projection.
   * @param updatedText New CSS text.
   * @param updatedSourceMap New source mapping.
   * @param offset Local CSS byte offset of the edit.
   * @param removedLength Old byte count.
   * @param insertedLength New byte count.
   * @param changed Optional verified rule ranges, set only when the splice succeeds.
   * @return True when the splice was installed, false when a full parse is needed.
   */
  bool applyTextEdit(const StylesheetComponent& previous, std::string_view updatedText,
                     const StylesheetSourceMap& updatedSourceMap, std::size_t offset,
                     std::size_t removedLength, std::size_t insertedLength,
                     css::EditedRuleWindow* changed = nullptr);

private:
  void installParsedStylesheet(const RcStringOrRef& str, StylesheetSourceMap newSourceMap,
                               css::Stylesheet parsed);

  /// Recompute the dependency summary and the rule indexes from \ref stylesheet and
  /// \ref importedStylesheets.
  void rebuildSelectorCaches();

  std::uint64_t contentRevision_ = 0;
};

}  // namespace donner::svg::components
