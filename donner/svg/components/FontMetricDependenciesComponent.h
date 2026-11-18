#pragma once
/// @file

#include <cstdint>
#include <vector>

#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::svg::components {

/// Faces used to resolve font-relative shape lengths, independently of any text subtree.
struct FontMetricDependenciesComponent {
  std::vector<FontFaceDependency> fontDependencies;
  uint64_t fontResourceRevision = 0;
  /// \ref FontManager::hostFontKey when this was prepared; a different key makes it stale.
  uint64_t hostFontKey = 0;
};

}  // namespace donner::svg::components
