#pragma once
/// @file

#include <optional>
#include <string>
#include <string_view>

#include "donner/base/FileOffset.h"
#include "donner/base/RcString.h"
#include "donner/base/xml/SourceDocument.h"
#include "donner/base/xml/XMLNode.h"

namespace donner::xml {

/**
 * Builds anchored replacements for nodes that lack recorded source spans by locally serializing
 * them and selecting insertion anchors relative to neighboring spans.
 */
class LocalizedEditBuilder {
public:
  /**
   * Construct a builder bound to the original source text.
   */
  explicit LocalizedEditBuilder(std::string_view source, std::string indentUnit = "  ");

  /**
   * Serialize \p node and create an insertion immediately before \p sibling's start span.
   * Returns std::nullopt if the sibling lacks a recorded location.
   */
  std::optional<SourceDocument::Replacement> insertBeforeSibling(const XMLNode& node,
                                                                 const XMLNode& sibling) const;

  /**
   * Serialize \p node and insert it as the last child of \p parent. The anchor is chosen just
   * before the parent's closing tag or before the closing marker of a self-closing tag. Returns
   * std::nullopt if no suitable anchor can be located.
   */
  std::optional<SourceDocument::Replacement> appendChild(const XMLNode& node,
                                                         const XMLNode& parent) const;

  /**
   * Remove the recorded span for \p node. If the node has no known span, returns std::nullopt.
   */
  std::optional<SourceDocument::Replacement> removeNode(const XMLNode& node) const;

private:
  std::string inferIndentation(size_t anchorOffset) const;
  bool isLineBreakBefore(size_t anchorOffset) const;
  std::optional<FileOffset> closingTagStart(const XMLNode& node) const;
  std::string serializeNode(const XMLNode& node, std::string_view indent) const;

  std::string source_;
  std::string indentUnit_;
};

}  // namespace donner::xml
