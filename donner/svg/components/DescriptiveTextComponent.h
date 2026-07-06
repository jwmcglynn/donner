#pragma once
/// @file

#include "donner/base/RcString.h"

namespace donner::svg::components {

/**
 * Stores the concatenated text content of a descriptive element, such as \ref xml_title, \ref
 * xml_desc, or \ref xml_metadata.
 *
 * Descriptive elements are never rendered; their text is retained purely so it can be surfaced
 * through the DOM (for accessibility tooling, tooltips, and document metadata). The SVG element
 * tree does not keep the source `Data`/`CData` child nodes that the XML parse tree carried, so
 * this component is the authoritative live text source, mirroring how \ref StylesheetComponent
 * retains the raw CSS text for \ref xml_style.
 */
struct DescriptiveTextComponent {
  /// The concatenated text content of the descriptive element.
  RcString text;
};

}  // namespace donner::svg::components
