#pragma once
/// @file

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_set>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::svg {
class DocumentState;
}

namespace donner::svg::components {

/// Owned evidence for pixels drawn from a separate registry. The weak handle does not keep an
/// obsolete source document alive. Its budget and revision cannot be replaced by a parent's face.
struct ChildFontPaintDependencies {
  std::weak_ptr<DocumentState> document;
  Entity target = entt::null;
  uint64_t preparationEpoch = 0;
  uint64_t renderScopeToken = 0;
  std::vector<FontFaceDependency> fontDependencies;
  std::vector<FontFaceDependency> renderedFontDependencies;
  bool renderedNeedsRender = false;
  bool renderedResourceLimit = false;
  uint64_t fontResourceRevision = 0;
  bool needsRender = false;
  bool resourceLimit = false;
};

/// Faces used by lazy paint work on one consuming rendering instance. These snapshots survive
/// temporary offscreen traversals and include external SVG images as separate consumers.
struct FontPaintDependenciesComponent {
  std::vector<FontFaceDependency> fontDependencies;
  std::vector<ChildFontPaintDependencies> children;
  uint64_t fontResourceRevision = 0;
  bool prepared = false;
  bool resourceLimit = false;
};

/// Changes to prepared geometry or captured child evidence invalidate operation-local graph caches.
struct FontResourcePreparationState {
  uint64_t epoch = 0;
};

/// Exclusions belong only to the last completed render. Target preflight never consults this proof.
struct RenderedFontResourceScope {
  std::size_t depth = 0;
  uint64_t token = 0;
  Entity coverageRoot = entt::null;
  Entity pendingCoverageRoot = entt::null;
  bool pendingComplete = true;
  bool complete = false;
  uint64_t preparationEpoch = 0;
  uint64_t fontResourceRevision = 0;
  std::unordered_set<Entity> excluded;
  std::unordered_set<Entity> pendingExcluded;
  std::unordered_set<Entity> painted;
};

/// Use the existing parsed-child envelope for recursive traversal and per-host child references.
inline constexpr std::size_t kMaximumFontChildDocuments = 64;

}  // namespace donner::svg::components
