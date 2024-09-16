

#pragma once
/// @file

namespace donner::svg::components {

/**
 * There are two types of shadow trees:
 * - \b Main: One for elements in the \b main render graph, such as \ref xml_use elements, where
 * each \ref xml_use element holds one instantiation, which behaves as if the referenced element was
 * copy-pasted within the tree.
 * - \b Offscreen: Shadow trees for paint servers and offscreen purposes, which are not
 * enumerated as part of the render graph. For example, a 'fill' attribute may reference a
 * \ref xml_pattern paint server, which needs to be rendered into a buffer before being tiled into
 * the main render graph.
 *
 * Shadow trees always belong to specific branches, and there are no multiples; there are a finite
 * number of branches. To enable this, list each branch individually, with everything except \b
 * Main being an offscreen branch.
 */
enum class ShadowBranchType {
  Main,             //!< For entities enumerated in the main render graph.
  OffscreenFill,    //!< For 'fill' attributes.
  OffscreenStroke,  //!< For 'stroke' attributes.
  OffscreenMask,    //!< For mask contents, used for the 'mask' attribute.
};

}  // namespace donner::svg::components
