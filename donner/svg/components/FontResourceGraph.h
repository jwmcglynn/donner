#pragma once
/// @file

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/SVGDocumentHandle.h"
#include "donner/svg/resources/FontCatalogTypes.h"

namespace donner::svg::components {

struct FontPaintDependenciesComponent;

void InvalidateFontResourcePreparation(Registry& registry);

/// A render scope accepts exclusions only after its caller completes the corresponding traversal.
class ScopedFontResourceRender {
public:
  explicit ScopedFontResourceRender(Registry& registry, Entity coverageRoot = entt::null);
  ~ScopedFontResourceRender();
  ScopedFontResourceRender(const ScopedFontResourceRender&) = delete;
  ScopedFontResourceRender& operator=(const ScopedFontResourceRender&) = delete;
  void finish(bool completed = true);
  void setCoverageRoot(Entity root);
  static void recordDraw(Registry& registry, Entity entity, bool excluded);

private:
  Registry& registry_;
  bool finished_ = false;
  bool outermost_ = false;
};

/**
 * A frame-local snapshot of the font dependencies and resolved reference ranges of the render
 * tree. Component pointers never survive construction. Build before resource refresh removes
 * geometry, then use the same snapshot to invalidate its actual consumers.
 */
class FontResourceGraph {
public:
  enum class Purpose { CompleteTarget, RenderedFrame };

  /// Owned result of following the prepared references reachable from a target.
  struct Collection {
    std::vector<FontFaceDependency> dependencies;
    std::vector<Entity> textRoots;  ///< Reached geometry owners, for guarded layout preparation.
    bool needsRender = false;
    bool resourceLimit = false;
  };

  /// Snapshot a prepared tree under the document access guard. Does not load any font.
  explicit FontResourceGraph(const Registry& registry);

  /// Collect main-tree instances beneath target, including `<use>` shadow children, and follow
  /// only their resolved paint/mask/marker ranges. A text span includes its containing text root.
  Collection collect(const Registry& registry, Entity target,
                     Purpose purpose = Purpose::CompleteTarget) const;

  /// Mark consumers of changed geometry through the references captured before refresh.
  /// Ordinary ancestor-layer propagation remains the renderer's existing dirty-entity path.
  void invalidateDependents(Registry& registry, std::span<const Entity> changedOwners) const;
  void refreshMetadata(const Registry& registry);

private:
  struct Instance {
    Entity storageEntity = entt::null;
    Entity geometryOwner = entt::null;
    int drawOrder = 0;
    bool visible = false;
    bool textRoot = false;
    bool needsRender = false;
    bool resourceLimit = false;
    bool renderedNeedsRender = false;
    bool renderedResourceLimit = false;
    bool excludedFromRender = false;
    std::vector<FontFaceDependency> geometryDependencies;
    std::vector<FontFaceDependency> dependencies;
    std::vector<FontFaceDependency> renderedDependencies;
  };

  static Instance snapshotInstance(const Registry& registry, Entity entity,
                                   std::span<const FontFaceDependency> current,
                                   const Instance* previous = nullptr);
  static void appendGeometryDependencies(Instance& instance, const Registry& registry,
                                         Entity dataEntity,
                                         std::span<const FontFaceDependency> current);
  static void appendPaintDependencies(Instance& instance,
                                      const FontPaintDependenciesComponent& paint,
                                      std::span<const FontFaceDependency> current);
  bool initializeIntervals();
  void addInstanceReferences(const Registry& registry, std::size_t index);
  std::optional<std::size_t> containingTextRootIndex(const Registry& registry, Entity target,
                                                     Collection& result) const;
  std::vector<std::size_t> collectSeeds(const Registry& registry, Entity target,
                                        Collection& result) const;
  struct DependencyAccumulator;
  void appendCollectedInstance(Collection& result, std::size_t index,
                               std::unordered_set<Entity>& textRoots, Purpose purpose,
                               DependencyAccumulator& dependencies) const;
  void addEdge(std::size_t consumer, std::size_t dependency);
  void addRange(std::size_t consumer, Entity first, Entity last);

  std::vector<Instance> instances_;
  std::unordered_map<Entity, std::size_t> indices_;
  // Leaves [N, 2N) are instances; inner nodes represent intervals in draw order. Reference
  // edges cover a range with O(log N) nodes instead of copying its descendants per consumer.
  std::vector<std::vector<std::size_t>> dependencies_;
  std::vector<std::vector<std::size_t>> consumers_;
  bool resourceLimit_ = false;
};

/// Bounded reuse within one render preparation or font-refresh traversal. No entry survives the
/// operation, and preparation changes invalidate both the graph and its target collections.
class FontResourceGraphCache {
public:
  /// New compute ceiling: 1024 passes over the existing 32 Ki parsed-child entity envelope.
  static constexpr std::size_t kMaximumWork = 32 * 1024 * 1024;
  struct Stats {
    std::size_t graphBuilds = 0;
    std::size_t targetCollections = 0;
    std::size_t cacheHits = 0;
    std::size_t work = 0;
    bool resourceLimit = false;
  };
  explicit FontResourceGraphCache(Stats* stats = nullptr) : stats_(stats ? stats : &ownedStats_) {}
  void preserveBeforeRefresh(const SVGDocumentHandle& document);
  void finishRefresh(const SVGDocumentHandle& document, std::span<const Entity> changedOwners);
  FontResourceGraph::Collection collect(
      const SVGDocumentHandle& document, Entity target,
      FontResourceGraph::Purpose purpose = FontResourceGraph::Purpose::CompleteTarget);

private:
  struct Entry {
    std::weak_ptr<DocumentState> document;
    uint64_t documentRevision = 0;
    uint64_t preparationEpoch = 0;
    bool completedScope = false;
    std::vector<FontFaceDependency> faces;
    std::unique_ptr<FontResourceGraph> graph;
    struct TargetCollection {
      FontResourceGraph::Collection value;
      uint64_t renderScopeToken = 0;
    };
    std::array<std::unordered_map<Entity, TargetCollection>, 2> targets;
    std::size_t collectionWork = 0;
    std::size_t buildWork = 0;
    bool matches(const SVGDocumentHandle& currentDocument, uint64_t epoch,
                 const std::vector<FontFaceDependency>& currentFaces, bool scopeComplete) const;
  };
  bool reserve(std::size_t work);
  bool prepareEntry(Entry& entry, const SVGDocumentHandle& document);
  Stats ownedStats_;
  Stats* stats_;
  std::unordered_map<const DocumentState*, Entry> entries_;
};

}  // namespace donner::svg::components
