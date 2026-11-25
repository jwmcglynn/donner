#pragma once
/// @file

#include "donner/base/ParseWarningSink.h"
#include "donner/svg/SVGDocument.h"
#include "donner/svg/components/style/StylePassScheduler.h"

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
   * @param warningSink Sink to collect warnings.
   * @param scheduler Optional scheduler borrowed during document preparation.
   */
  static void prepareDocumentForRendering(SVGDocument& document, bool verbose,
                                          ParseWarningSink& warningSink,
                                          StylePassScheduler scheduler = {});
};

}  // namespace donner::svg
