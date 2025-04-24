#pragma once
/// @file

#include <string_view>

#include "donner/base/ParseResult.h"
#include "donner/base/xml/XMLDocument.h"
#include "donner/base/xml/XMLNode.h"

namespace donner::xml {

/**
 * Parses an XML document from a string.
 *
 * The document tree will remain valid as long as the returned \ref XMLDocument is alive.
 */
class XMLParser {
public:
  /**
   * Options to modify the parsing behavior.
   */
  struct Options {
    /// Default options.
    constexpr Options() {}

    /**
     * Parse all nodes in the XML document, including comments, the doctype node, and processing
     * instructions.
     */
    static Options ParseAll() {
      Options options;
      options.parseComments = true;
      options.parseProcessingInstructions = true;
      return options;
    }

    /**
     * Parse comments in the XML document, e.g. `<!-- ... -->`. If this flag is set to true,
     * comments will be parsed as \ref XMLNode::Type::Comment nodes added to the document tree.
     */
    bool parseComments = false;

    /**
     * Parse the doctype node in the XML document, e.g. `<!DOCTYPE ...>`. If this flag is set to
     * true, the doctype node will be parsed as a \ref XMLNode::Type::Doctype node added to the
     * document tree.
     */
    bool parseDoctype = true;

    /**
     * Parse processing instructions in the XML document, e.g. `<?php ...>`. If this flag is set to
     * true, processing instructions will be parsed as \ref XMLNode::Type::ProcessingInstruction
     * nodes added to the document tree.
     */
    bool parseProcessingInstructions = false;

    /**
     * Disable entity translation during parsing. If this flag is set to true, built-in entities
     * such as `&amp;` and `&lt;` will not be translated to their respective characters.
     */
    bool disableEntityTranslation = false;
  };

  /**
   * Parse an XML string with the given options.
   *
   * By default, the parser will ignore comments, the doctype node, and processing instructions. To
   * enable parsing these nodes, configure the flags within \ref Options.
   *
   * The document tree will remain valid as long as the returned \ref XMLDocument is alive.
   *
   * @param str XML data to parse. Will not be modified.
   * @param options Options to modify the parsing behavior.
   * @return ParseResult containing the parsed XMLDocument, or an error if parsing failed.
   */
  static ParseResult<XMLDocument> Parse(std::string_view str, const Options& options = Options());

  /**
   * Parse the XML attributes and get the source location of a specific attribute.
   *
   * For example, for the following XML:
   * ```xml
   * <root>
   *   <child attr="Hello, world!">
   * </root>
   * ```
   *
   * The FileOffsetRange for the `attr` attribute should contain the substring `attr="Hello,
   * world!"`
   *
   * @param str XML data to parse. Will not be modified.
   * @param elementStartOffset Start offset of the element in the input string.
   * @param attributeName Name of the attribute to find.
   * @return std::optional<AttributeLocation> containing the start and end offsets of the attribute
   * in the input string, or std::nullopt if the attribute was not found.
   */
  static std::optional<FileOffsetRange> GetAttributeLocation(
      std::string_view str, FileOffset elementStartOffset,
      const XMLQualifiedNameRef& attributeName);
};

}  // namespace donner::xml
