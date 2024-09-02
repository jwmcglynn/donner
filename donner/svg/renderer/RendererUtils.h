#pragma once
/// @file

#include "donner/svg/SVGDocument.h"

namespace donner::svg {

/**
 * Utility functions for the renderer.
 */
class RendererUtils {
public:
  /**
   * Prepare the document for rendering, instantiating computed components and the rendering tree.
   *
   * @param document Document to prepare.
   * @param verbose If true, enable verbose logging.
   * @param outWarnings If non-null, warnings will be added to this vector.
   */
  static void prepareDocumentForRendering(SVGDocument& document, bool verbose,
                                          std::vector<parser::ParseError>* outWarnings = nullptr);
};

}  // namespace donner::svg
