#pragma once
/// @file

#include "donner/svg/SVGElement.h"

namespace donner::svg {

/**
 * @defgroup xml_feMergeNode "<feMergeNode>"
 *
 * A child element of \ref xml_feMerge that specifies one input layer.
 *
 * - DOM object: SVGFEMergeNodeElement
 * - SVG2 spec: https://www.w3.org/TR/filter-effects/#elementdef-femergenode
 *
 * Each `<feMergeNode>` has an `in` attribute specifying the filter input to use as a layer.
 */

/**
 * DOM object for a \ref xml_feMergeNode element.
 */
class SVGFEMergeNodeElement : public SVGElement {
  friend class parser::SVGParserImpl;

protected:
  explicit SVGFEMergeNodeElement(EntityHandle handle) : SVGElement(handle) {}

  static SVGFEMergeNodeElement CreateOn(EntityHandle handle);

public:
  /// Element type.
  static constexpr ElementType Type = ElementType::FeMergeNode;
  /// XML tag name, \ref xml_feMergeNode.
  static constexpr std::string_view Tag{"feMergeNode"};
  /// This is an experimental/incomplete feature.
  static constexpr bool IsExperimental = true;
};

}  // namespace donner::svg
