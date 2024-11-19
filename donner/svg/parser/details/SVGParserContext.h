#pragma once

#include <vector>

#include "donner/base/FileOffset.h"
#include "donner/base/ParseError.h"
#include "donner/base/parser/LineOffsets.h"
#include "donner/base/xml/XMLNode.h"
#include "donner/base/xml/XMLQualifiedName.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg::parser {

/**
 * Contains the start location within a string where a subparser was invoked, used for remapping
 * errors back to their original text.
 */
struct ParserOrigin {
  /// 0-based offset into the string where the subparser started.
  size_t startOffset;

  /**
   * Create a ParserOrigin with the given start offset.
   *
   * @param offset 0-based offset into the string where the subparser started.
   */
  static ParserOrigin StartOffset(size_t offset) { return ParserOrigin{offset}; }
};

/**
 * Stores the current state of \ref SVGParser during parsing. Used to add parse warnings and
 * store global state like the parsing options.
 */
class SVGParserContext {
public:
  /**
   * Construct a new context for the given input string.
   *
   * @param input Input string.
   * @param warningsStorage Storage for warnings, may be \c nullptr to disable warnings.
   * @param options Options for parsing.
   */
  SVGParserContext(std::string_view input, std::vector<ParseError>* warningsStorage,
                   const SVGParser::Options& options)
      : input_(input), lineOffsets_(input), warnings_(warningsStorage), options_(options) {}

  /// Get the parser options.
  const SVGParser::Options& options() const { return options_; }

  /**
   * Set the XML document's default namespace prefix, such as "http://www.w3.org/2000/svg".
   *
   * @param namespacePrefix The default namespace prefix, such as "http://www.w3.org/2000/svg".
   */
  void setNamespacePrefix(const RcString& namespacePrefix) { namespacePrefix_ = namespacePrefix; }

  /// Get the XML document's default namespace prefix, such as "http://www.w3.org/2000/svg".
  std::string_view namespacePrefix() const { return namespacePrefix_; }

  /**
   * Remap a parse error from a subparser back to the original input string, translating the line
   * numbers.
   *
   * @param error Error to remap.
   * @param origin Origin of the subparser.
   * @return ParseError Remapped error.
   */
  ParseError fromSubparser(ParseError&& error, ParserOrigin origin) {
    const size_t line = lineOffsets_.offsetToLine(origin.startOffset);
    const base::parser::FileOffset parentOffset = base::parser::FileOffset::OffsetWithLineInfo(
        origin.startOffset,
        base::parser::FileOffset::LineInfo{line, lineOffsets_.lineOffset(line)});

    ParseError newError = std::move(error);
    newError.location = newError.location.addParentOffset(parentOffset);
    return newError;
  }

  /**
   * Add a warning to the list of warnings.
   *
   * @param warning Warning to add.
   */
  void addWarning(ParseError&& warning) {
    if (warnings_) {
      warnings_->emplace_back(std::move(warning));
    }
  }

  /**
   * Add a warning from a subparser to the list of warnings, remapping the error back to the
   * original input string.
   *
   * @param warning Warning to add.
   * @param origin Origin of the subparser.
   */
  void addSubparserWarning(ParseError&& warning, ParserOrigin origin) {
    addWarning(fromSubparser(std::move(warning), origin));
  }

  /**
   * Get the location of an element's attribute in the input string.
   *
   * @param element Element containing the attribute.
   * @param attributeName Name of the attribute.
   * @return std::optional<size_t> Offset of the element's attribute in the input string.
   */
  std::optional<base::parser::FileOffsetRange> getAttributeLocation(
      const SVGElement& element, const xml::XMLQualifiedNameRef& attributeName) const {
    // Convert the SVGElement into an XMLNode.
    if (auto maybeNode = xml::XMLNode::TryCast(EntityHandle(element.entityHandle()))) {
      xml::XMLNode node = maybeNode.value();
      return node.getAttributeLocation(input_, attributeName);
    }
    return std::nullopt;
  }

  /**
   * Create a \ref ParserOrigin for the given substring, where \p substring is within the XML
   * parser's original string, \ref input_.
   *
   * @param substring Substring within the XML parser's original string.
   */
  ParserOrigin parserOriginFrom(std::string_view substring) const {
    if (substring.begin() > input_.begin() && substring.end() < input_.end()) {
      return ParserOrigin::StartOffset(substring.begin() - input_.begin());
    } else {
      return ParserOrigin::StartOffset(0);
    }
  }

  /**
   * Return line numbers for the given offset.
   *
   * For example, given a string: "abc\n123", offsets 0-3 would be considered line 1, and offsets
   * after 4 (corresponding to the index of '1'), would be line 2. Values beyond the length of the
   * string return the last line number.
   *
   * @param offset Character index.
   * @return size_t Line number, 1-indexed.
   */
  size_t offsetToLine(size_t offset) const { return lineOffsets_.offsetToLine(offset); }

  /**
   * Returns the offset of a given 1-indexed line number.
   *
   * @param line Line number, 1-indexed.
   */
  size_t lineOffset(size_t line) const { return lineOffsets_.lineOffset(line); }

private:
  /// Original string containing the XML text, used for remapping errors.
  std::string_view input_;

  /// Offsets of the start of each line in the input string.
  base::parser::LineOffsets lineOffsets_;

  /// Storage for warnings, may be \c nullptr to disable warnings.
  std::vector<ParseError>* warnings_;

  /// Options for parsing.
  SVGParser::Options options_;

  /// The XML document's default namespace prefix, such as "http://www.w3.org/2000/svg".
  RcString namespacePrefix_;
};

}  // namespace donner::svg::parser
