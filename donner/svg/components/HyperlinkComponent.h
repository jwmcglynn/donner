#pragma once
/// @file

#include "donner/base/RcString.h"

namespace donner::svg::components {

/**
 * Stores the hyperlink target for an \ref xml_a element.
 *
 * The `<a>` element is a transparent grouping/text-content container whose only element-specific
 * state is the link target (`href` / `xlink:href`). The reference is retained so it round-trips
 * through the DOM, but it does not affect rendering - Donner has no interactive navigation, so the
 * link target is purely informational, matching how resvg rasterizes `<a>`.
 */
struct HyperlinkComponent {
  /// Link target, an IRI reference (e.g. "https://example.com/" or "#fragment").
  RcString href;
};

}  // namespace donner::svg::components
