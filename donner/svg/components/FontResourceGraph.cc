#include "donner/svg/components/FontResourceGraph.h"

#include <algorithm>
#include <iterator>
#include <limits>
#include <map>
#include <tuple>
#include <unordered_set>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/FontMetricDependenciesComponent.h"
#include "donner/svg/components/FontPaintDependenciesComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/SubDocumentCache.h"
#include "donner/svg/components/text/ComputedTextGeometryComponent.h"
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/resources/FontManager.h"

namespace donner::svg::components {
namespace {

int ReadinessSeverity(const FontFaceDependency& dependency) {
  if (dependency.state == FontFaceLoadState::Failed) return 4;
  if (dependency.waitReason == FontFaceWaitReason::RetainedBudget) return 3;
  if (dependency.state == FontFaceLoadState::WaitingForAdmission) return 2;
  return dependency.state == FontFaceLoadState::Loaded ? 0 : 1;
}

bool DependencyLess(const FontFaceDependency& a, const FontFaceDependency& b) {
  return std::tie(a.family, a.request.weight, a.request.style, a.request.stretch,
                  a.availability.contentId, a.availability.contentGeneration) <
         std::tie(b.family, b.request.weight, b.request.style, b.request.stretch,
                  b.availability.contentId, b.availability.contentGeneration);
}

bool SameDependencyIdentity(const FontFaceDependency& a, const FontFaceDependency& b) {
  return !DependencyLess(a, b) && !DependencyLess(b, a);
}

void AppendDependencies(std::vector<FontFaceDependency>& result,
                        std::span<const FontFaceDependency> dependencies,
                        std::span<const FontFaceDependency> current) {
  std::map<std::tuple<std::string, int, int, int>, const FontFaceDependency*> currentByFace;
  for (const auto& face : current) {
    currentByFace.emplace(
        std::make_tuple(face.family, face.request.weight, static_cast<int>(face.request.style),
                        static_cast<int>(face.request.stretch)),
        &face);
  }
  std::vector<FontFaceDependency> incoming;
  incoming.reserve(dependencies.size());
  for (const auto& dependency : dependencies) {
    const auto found = currentByFace.find(std::make_tuple(
        dependency.family, dependency.request.weight, static_cast<int>(dependency.request.style),
        static_cast<int>(dependency.request.stretch)));
    incoming.push_back(found == currentByFace.end() ? dependency : *found->second);
  }
  std::stable_sort(incoming.begin(), incoming.end(), DependencyLess);
  std::vector<FontFaceDependency> merged;
  merged.reserve(result.size() + incoming.size());
  std::merge(result.begin(), result.end(), incoming.begin(), incoming.end(),
             std::back_inserter(merged), DependencyLess);
  result = std::move(merged);
  auto output = result.begin();
  for (auto item = result.begin(); item != result.end(); ++item) {
    if (output != result.begin() && SameDependencyIdentity(*(output - 1), *item)) {
      if (ReadinessSeverity(*item) > ReadinessSeverity(*(output - 1)))
        *(output - 1) = std::move(*item);
    } else {
      if (output != item) *output = std::move(*item);
      ++output;
    }
  }
  result.erase(output, result.end());
}

bool HasUnpreparedFragment(const Registry& registry, const RenderingInstanceComponent& instance,
                           const FontPaintDependenciesComponent* paint) {
  if (paint && paint->resourceLimit) return false;
  const auto captured = [&](const SVGDocumentHandle& document) {
    return !document || (paint && std::any_of(paint->children.begin(), paint->children.end(),
                                              [&](const auto& child) {
                                                return child.document.lock() == document;
                                              }));
  };
  if (instance.visible) {
    if (const auto* image = registry.try_get<LoadedSVGImageComponent>(instance.dataEntity);
        image && !captured(image->subDocument))
      return true;
    if (const auto* use = registry.try_get<ExternalUseComponent>(instance.dataEntity);
        use && !captured(use->subDocument))
      return true;
  }
  if (!instance.resolvedFilter) return false;
  if (const auto* effects = std::get_if<std::vector<FilterEffect>>(&*instance.resolvedFilter)) {
    return (!paint || !paint->prepared) &&
           std::any_of(effects->begin(), effects->end(), [](const FilterEffect& effect) {
             return effect.is<FilterEffect::ElementReference>();
           });
  }
  const auto* reference = std::get_if<ResolvedReference>(&*instance.resolvedFilter);
  if (!reference || reference->handle.registry() != &registry || !reference->valid()) return false;
  const auto* filter = registry.try_get<ComputedFilterComponent>(reference->handle.entity());
  if (!filter) return false;
  return std::any_of(
      filter->filterGraph.nodes.begin(), filter->filterGraph.nodes.end(), [&](const auto& node) {
        const auto* image = std::get_if<filter_primitive::Image>(&node.primitive);
        return image && ((!image->fragmentId.empty() && (!paint || !paint->prepared)) ||
                         (image->svgSubDocument && !captured(image->svgSubDocument)));
      });
}

bool HasCurrentRenderExclusion(const Registry& registry, Entity entity) {
  const auto* scope = registry.ctx().find<RenderedFontResourceScope>();
  const auto* preparation = registry.ctx().find<FontResourcePreparationState>();
  if (!scope || !preparation || !scope->complete || scope->preparationEpoch != preparation->epoch)
    return false;
  const auto* manager = registry.ctx().find<FontManager>();
  if (scope->fontResourceRevision != (manager ? manager->fontResourceRevision() : 0)) return false;
  return scope->excluded.contains(entity);
}

// Memoize ancestry across every seed, so deeply nested trees are not walked once per instance.
class TargetMembership {
public:
  TargetMembership(const Registry& registry, Entity target)
      : registry_(registry),
        maximumHops_(registry.view<const donner::components::TreeComponent>().size()) {
    membership_.emplace(target, true);
    membership_.emplace(entt::null, false);
  }

  bool contains(Entity entity) {
    if (resourceLimit_) return false;
    std::vector<Entity> path;
    Entity current = entity;
    while (!membership_.contains(current)) {
      if (path.size() > maximumHops_) {
        resourceLimit_ = true;
        return false;
      }
      path.push_back(current);
      const auto* tree = registry_.try_get<donner::components::TreeComponent>(current);
      current = tree ? tree->parent() : entt::null;
    }
    const bool result = membership_.at(current);
    for (const Entity visited : path) membership_.emplace(visited, result);
    return result;
  }

  bool resourceLimit() const { return resourceLimit_; }

private:
  const Registry& registry_;
  const std::size_t maximumHops_;
  std::unordered_map<Entity, bool> membership_;
  bool resourceLimit_ = false;
};

bool HasCompletedCurrentScope(const Registry& registry) {
  const auto* scope = registry.ctx().find<RenderedFontResourceScope>();
  const auto* preparation = registry.ctx().find<FontResourcePreparationState>();
  return scope && preparation && scope->complete && scope->preparationEpoch == preparation->epoch;
}

bool HasRenderCoverage(const Registry& registry, Entity target) {
  if (!HasCompletedCurrentScope(registry)) return false;
  const auto& scope = registry.ctx().get<RenderedFontResourceScope>();
  if (scope.coverageRoot == entt::null) return false;
  TargetMembership membership(registry, scope.coverageRoot);
  return membership.contains(target) && !membership.resourceLimit();
}

}  // namespace

void InvalidateFontResourcePreparation(Registry& registry) {
  ++registry.ctx().emplace<FontResourcePreparationState>().epoch;
}

ScopedFontResourceRender::ScopedFontResourceRender(Registry& registry, Entity coverageRoot)
    : registry_(registry) {
  auto& scope = registry_.ctx().emplace<RenderedFontResourceScope>();
  outermost_ = scope.depth++ == 0;
  if (outermost_) {
    scope.pendingCoverageRoot = coverageRoot;
    scope.pendingComplete = true;
    ++scope.token;
    scope.complete = false;
    scope.pendingExcluded.clear();
    scope.painted.clear();
  }
}

ScopedFontResourceRender::~ScopedFontResourceRender() {
  finish(false);
}

void ScopedFontResourceRender::finish(bool completed) {
  if (finished_) return;
  finished_ = true;
  auto& scope = registry_.ctx().get<RenderedFontResourceScope>();
  scope.pendingComplete &= completed;
  if (--scope.depth != 0) return;
  if (scope.excluded != scope.pendingExcluded || scope.coverageRoot != scope.pendingCoverageRoot) {
    InvalidateFontResourcePreparation(registry_);
  }
  scope.excluded = std::move(scope.pendingExcluded);
  scope.coverageRoot = scope.pendingCoverageRoot;
  scope.complete = scope.pendingComplete;
  scope.preparationEpoch = registry_.ctx().emplace<FontResourcePreparationState>().epoch;
  const auto* manager = registry_.ctx().find<FontManager>();
  scope.fontResourceRevision = manager ? manager->fontResourceRevision() : 0;
}

void ScopedFontResourceRender::setCoverageRoot(Entity root) {
  if (outermost_ && !finished_) {
    registry_.ctx().get<RenderedFontResourceScope>().pendingCoverageRoot = root;
  }
}

void ScopedFontResourceRender::recordDraw(Registry& registry, Entity entity, bool excluded) {
  auto* scope = registry.ctx().find<RenderedFontResourceScope>();
  if (!scope || scope->depth == 0) return;
  if (excluded) {
    if (!scope->painted.contains(entity)) scope->pendingExcluded.insert(entity);
  } else {
    scope->painted.insert(entity);
    scope->pendingExcluded.erase(entity);
  }
}

FontResourceGraph::FontResourceGraph(const Registry& registry) {
  const auto* manager = registry.ctx().find<FontManager>();
  const std::vector<FontFaceDependency> current =
      manager ? manager->faceDependencies() : std::vector<FontFaceDependency>();
  resourceLimit_ = manager && manager->fontDependenciesOverflowed();
  for (const Entity entity : registry.view<const RenderingInstanceComponent>()) {
    instances_.push_back(snapshotInstance(registry, entity, current));
  }
  if (!initializeIntervals()) return;
  for (std::size_t i = 0; i < instances_.size(); ++i) addInstanceReferences(registry, i);
}

FontResourceGraph::Instance FontResourceGraph::snapshotInstance(
    const Registry& registry, Entity entity, std::span<const FontFaceDependency> current,
    const Instance* previous) {
  const auto& rendered = registry.get<RenderingInstanceComponent>(entity);
  const Entity geometryOwner =
      registry.any_of<TextRootComponent, ComputedTextGeometryComponent>(entity)
          ? entity
          : rendered.dataEntity;
  Instance instance{.storageEntity = entity,
                    .geometryOwner = geometryOwner,
                    .drawOrder = rendered.drawOrder,
                    .visible = rendered.visible,
                    .textRoot = registry.all_of<TextRootComponent>(geometryOwner)};
  if (instance.visible)
    appendGeometryDependencies(instance, registry, rendered.dataEntity, current);
  if (previous && instance.textRoot &&
      !registry.all_of<ComputedTextGeometryComponent>(geometryOwner)) {
    AppendDependencies(instance.dependencies, previous->geometryDependencies, current);
  }
  instance.geometryDependencies = instance.dependencies;
  if (const auto* clip = registry.try_get<ComputedClipPathsComponent>(entity)) {
    AppendDependencies(instance.dependencies, clip->fontDependencies, current);
  }
  instance.renderedDependencies = instance.dependencies;
  const auto* paint = registry.try_get<FontPaintDependenciesComponent>(entity);
  if (paint) appendPaintDependencies(instance, *paint, current);
  const bool unprepared = HasUnpreparedFragment(registry, rendered, paint);
  instance.needsRender |= unprepared;
  instance.renderedNeedsRender |= unprepared;
  instance.excludedFromRender = HasCurrentRenderExclusion(registry, entity);
  return instance;
}

void FontResourceGraph::appendGeometryDependencies(Instance& instance, const Registry& registry,
                                                   Entity dataEntity,
                                                   std::span<const FontFaceDependency> current) {
  if (const auto* text = registry.try_get<ComputedTextGeometryComponent>(instance.geometryOwner)) {
    AppendDependencies(instance.dependencies, text->fontDependencies, current);
  }
  const Entity metricsOwner =
      registry.all_of<FontMetricDependenciesComponent>(instance.storageEntity)
          ? instance.storageEntity
          : dataEntity;
  if (const auto* metrics = registry.try_get<FontMetricDependenciesComponent>(metricsOwner)) {
    AppendDependencies(instance.dependencies, metrics->fontDependencies, current);
  }
}

void FontResourceGraph::appendPaintDependencies(Instance& instance,
                                                const FontPaintDependenciesComponent& paint,
                                                std::span<const FontFaceDependency> current) {
  AppendDependencies(instance.dependencies, paint.fontDependencies, current);
  instance.resourceLimit |= paint.resourceLimit;
  AppendDependencies(instance.renderedDependencies, paint.fontDependencies, current);
  instance.renderedResourceLimit |= paint.resourceLimit;
  for (const auto& child : paint.children) {
    // Child snapshots are already resolved against their own registry. Never rebind them
    // through the parent manager, even when both documents request the same family/face.
    AppendDependencies(instance.dependencies, child.fontDependencies, {});
    instance.resourceLimit |= child.resourceLimit;
    instance.needsRender |= child.needsRender || child.document.expired();
    AppendDependencies(instance.renderedDependencies, child.renderedFontDependencies, {});
    instance.renderedResourceLimit |= child.renderedResourceLimit;
    instance.renderedNeedsRender |= child.renderedNeedsRender || child.document.expired();
  }
}

bool FontResourceGraph::initializeIntervals() {
  std::sort(instances_.begin(), instances_.end(),
            [](const Instance& lhs, const Instance& rhs) { return lhs.drawOrder < rhs.drawOrder; });
  const std::size_t count = instances_.size();
  if (count > std::numeric_limits<std::size_t>::max() / 2) {
    resourceLimit_ = true;
    return false;
  }
  dependencies_.resize(count * 2);
  consumers_.resize(count * 2);
  for (std::size_t i = 0; i < count; ++i) indices_.emplace(instances_[i].storageEntity, i);
  for (std::size_t i = 1; i < count; ++i) {
    addEdge(i, i * 2);
    addEdge(i, i * 2 + 1);
  }
  return true;
}

void FontResourceGraph::addInstanceReferences(const Registry& registry, std::size_t index) {
  const auto& instance = registry.get<RenderingInstanceComponent>(instances_[index].storageEntity);
  const auto addSubtree = [&](const std::optional<SubtreeInfo>& subtree) {
    if (subtree) {
      addRange(instances_.size() + index, subtree->firstRenderedEntity,
               subtree->lastRenderedEntity);
    }
  };
  if (const auto* fill = std::get_if<PaintResolvedReference>(&instance.resolvedFill)) {
    addSubtree(fill->subtreeInfo);
  }
  if (const auto* stroke = std::get_if<PaintResolvedReference>(&instance.resolvedStroke)) {
    addSubtree(stroke->subtreeInfo);
  }
  if (instance.mask) addSubtree(instance.mask->subtreeInfo);
  if (instance.markerStart) addSubtree(instance.markerStart->subtreeInfo);
  if (instance.markerMid) addSubtree(instance.markerMid->subtreeInfo);
  if (instance.markerEnd) addSubtree(instance.markerEnd->subtreeInfo);
}

void FontResourceGraph::addEdge(std::size_t consumer, std::size_t dependency) {
  dependencies_[consumer].push_back(dependency);
  consumers_[dependency].push_back(consumer);
}

void FontResourceGraph::addRange(std::size_t consumer, Entity first, Entity last) {
  const auto firstIndex = indices_.find(first);
  const auto lastIndex = indices_.find(last);
  if (firstIndex == indices_.end() || lastIndex == indices_.end() ||
      firstIndex->second > lastIndex->second) {
    resourceLimit_ = true;
    return;
  }
  std::size_t begin = instances_.size() + firstIndex->second;
  std::size_t end = instances_.size() + lastIndex->second + 1;
  while (begin < end) {
    if (begin & 1) addEdge(consumer, begin++);
    if (end & 1) addEdge(consumer, --end);
    begin /= 2;
    end /= 2;
  }
}

std::optional<std::size_t> FontResourceGraph::containingTextRootIndex(const Registry& registry,
                                                                      Entity target,
                                                                      Collection& result) const {
  // A tspan has no independent rendering instance; its containing text root owns the layout.
  Entity ancestor = target;
  const std::size_t maximumHops = registry.view<const donner::components::TreeComponent>().size();
  for (std::size_t hops = 0; ancestor != entt::null; ++hops) {
    if (hops > maximumHops) {
      result.resourceLimit = true;
      break;
    }
    if (const auto found = indices_.find(ancestor);
        found != indices_.end() && instances_[found->second].textRoot) {
      return found->second;
    }
    const auto* tree = registry.try_get<donner::components::TreeComponent>(ancestor);
    ancestor = tree ? tree->parent() : entt::null;
  }
  return std::nullopt;
}

std::vector<std::size_t> FontResourceGraph::collectSeeds(const Registry& registry, Entity target,
                                                         Collection& result) const {
  TargetMembership membership(registry, target);
  std::vector<std::size_t> seeds;
  for (std::size_t i = 0; i < instances_.size(); ++i) {
    if (membership.contains(instances_[i].storageEntity)) seeds.push_back(instances_.size() + i);
  }
  if (const auto index = containingTextRootIndex(registry, target, result)) {
    seeds.push_back(instances_.size() + *index);
  }
  if (seeds.empty()) {
    for (const Entity root : registry.view<const TextRootComponent>()) {
      result.needsRender |= membership.contains(root);
    }
    for (const Entity owner : registry.view<const FontMetricDependenciesComponent>()) {
      result.needsRender |= membership.contains(owner);
    }
  }
  result.resourceLimit |= membership.resourceLimit();
  return seeds;
}

struct FontResourceGraph::DependencyAccumulator {
  using Key = std::tuple<std::string, int, int, int, std::string, uint64_t>;
  std::map<Key, FontFaceDependency> values;

  void append(std::span<const FontFaceDependency> dependencies) {
    for (const auto& face : dependencies) {
      Key key{face.family,
              face.request.weight,
              static_cast<int>(face.request.style),
              static_cast<int>(face.request.stretch),
              face.availability.contentId,
              face.availability.contentGeneration};
      auto [entry, inserted] = values.try_emplace(std::move(key), face);
      if (!inserted && ReadinessSeverity(face) > ReadinessSeverity(entry->second))
        entry->second = face;
    }
  }

  std::vector<FontFaceDependency> finish() {
    std::vector<FontFaceDependency> result;
    result.reserve(values.size());
    for (auto& [key, face] : values) result.push_back(std::move(face));
    return result;
  }
};

void FontResourceGraph::appendCollectedInstance(Collection& result, std::size_t index,
                                                std::unordered_set<Entity>& textRoots,
                                                Purpose purpose,
                                                DependencyAccumulator& dependencies) const {
  const Instance& instance = instances_[index];
  if (instance.visible && instance.textRoot && textRoots.insert(instance.geometryOwner).second) {
    result.textRoots.push_back(instance.geometryOwner);
  }
  // Container effects also apply to visibility-overriding descendants.
  const bool rendered = purpose == Purpose::RenderedFrame;
  dependencies.append(rendered ? instance.renderedDependencies : instance.dependencies);
  result.needsRender |= rendered ? instance.renderedNeedsRender : instance.needsRender;
  result.resourceLimit |= rendered ? instance.renderedResourceLimit : instance.resourceLimit;
}

FontResourceGraph::Collection FontResourceGraph::collect(const Registry& registry, Entity target,
                                                         Purpose purpose) const {
  Collection result{.resourceLimit = resourceLimit_};
  if (resourceLimit_) return result;
  result.needsRender = purpose == Purpose::RenderedFrame && !HasRenderCoverage(registry, target);
  std::vector<bool> visited(dependencies_.size(), false);
  std::vector<std::size_t> pending;
  const auto enqueue = [&](std::size_t node) {
    if (!visited[node]) {
      visited[node] = true;
      pending.push_back(node);
    }
  };
  for (const std::size_t seed : collectSeeds(registry, target, result)) enqueue(seed);
  std::unordered_set<Entity> textRoots;
  DependencyAccumulator accumulated;
  while (!pending.empty()) {
    const std::size_t node = pending.back();
    pending.pop_back();
    if (node >= instances_.size()) {
      const auto index = node - instances_.size();
      if (purpose == Purpose::RenderedFrame && instances_[index].excludedFromRender) continue;
      appendCollectedInstance(result, index, textRoots, purpose, accumulated);
    }
    for (const std::size_t dependency : dependencies_[node]) enqueue(dependency);
  }
  result.dependencies = accumulated.finish();
  return result;
}

void FontResourceGraph::refreshMetadata(const Registry& registry) {
  const auto* manager = registry.ctx().find<FontManager>();
  const auto current = manager ? manager->faceDependencies() : std::vector<FontFaceDependency>();
  for (auto& instance : instances_) {
    instance = snapshotInstance(registry, instance.storageEntity, current, &instance);
  }
  resourceLimit_ |= manager && manager->fontDependenciesOverflowed();
}

void FontResourceGraph::invalidateDependents(Registry& registry,
                                             std::span<const Entity> changedOwners) const {
  if (dependencies_.empty() || changedOwners.empty()) return;
  const std::unordered_set<Entity> changed(changedOwners.begin(), changedOwners.end());
  std::vector<bool> visited(consumers_.size(), false);
  std::vector<std::size_t> pending;
  const auto enqueue = [&](std::size_t node) {
    if (!visited[node]) {
      visited[node] = true;
      pending.push_back(node);
    }
  };
  for (std::size_t i = 0; i < instances_.size(); ++i) {
    if (changed.contains(instances_[i].storageEntity) ||
        changed.contains(instances_[i].geometryOwner)) {
      enqueue(instances_.size() + i);
    }
  }
  while (!pending.empty()) {
    const std::size_t node = pending.back();
    pending.pop_back();
    if (node >= instances_.size()) {
      const Entity entity = instances_[node - instances_.size()].storageEntity;
      if (registry.valid(entity)) {
        registry.get_or_emplace<DirtyFlagsComponent>(entity).mark(
            DirtyFlagsComponent::TextGeometry | DirtyFlagsComponent::Paint |
            DirtyFlagsComponent::RenderInstance);
      }
    }
    for (const std::size_t consumer : consumers_[node]) enqueue(consumer);
  }
}

namespace {
static_assert(FontResourceGraphCache::kMaximumWork ==
              SubDocumentCache::Limits{}.maximumAggregateEntities * 1024);

std::size_t AddWork(std::size_t a, std::size_t b) {
  constexpr auto maximum = FontResourceGraphCache::kMaximumWork;
  return a > maximum || b > maximum - a ? maximum + 1 : a + b;
}

std::size_t ScaleWork(std::size_t count, std::size_t factor) {
  constexpr auto maximum = FontResourceGraphCache::kMaximumWork;
  return factor && count > maximum / factor ? maximum + 1 : count * factor;
}

std::size_t WorkLevels(std::size_t count) {
  std::size_t levels = 1;
  for (; count > 1; count /= 2) ++levels;
  return levels;
}

std::size_t CollectionWork(const Registry& registry) {
  const auto count = registry.view<const RenderingInstanceComponent>().size();
  // Six resolved ranges each add at most 2 log(N) edges, plus the shared interval tree.
  return AddWork(ScaleWork(count, 12 * WorkLevels(count) + 12),
                 registry.view<const donner::components::TreeComponent>().size() + 1);
}

struct MetadataWork {
  std::size_t items = 0;
  std::size_t build = 0;
};

MetadataWork InstanceMetadataWork(const Registry& registry, Entity entity, std::size_t faces) {
  const auto& rendered = registry.get<RenderingInstanceComponent>(entity);
  const Entity owner = registry.any_of<TextRootComponent, ComputedTextGeometryComponent>(entity)
                           ? entity
                           : rendered.dataEntity;
  std::size_t items = 0;
  if (const auto* text = registry.try_get<ComputedTextGeometryComponent>(owner))
    items = text->fontDependencies.size();
  const auto* metrics = registry.try_get<FontMetricDependenciesComponent>(entity);
  if (!metrics) metrics = registry.try_get<FontMetricDependenciesComponent>(rendered.dataEntity);
  if (metrics) items = AddWork(items, metrics->fontDependencies.size());
  if (const auto* clip = registry.try_get<ComputedClipPathsComponent>(entity))
    items = AddWork(items, clip->fontDependencies.size());
  const auto* paint = registry.try_get<FontPaintDependenciesComponent>(entity);
  if (paint) items = AddWork(items, paint->fontDependencies.size());
  items = ScaleWork(items, 2);
  if (paint) {
    for (const auto& child : paint->children) {
      items = AddWork(
          items, AddWork(child.fontDependencies.size(), child.renderedFontDependencies.size()));
    }
  }
  const std::size_t appendCalls = 8 + (paint ? 2 * paint->children.size() : 0);
  const std::size_t mergeWork = ScaleWork(items, WorkLevels(items) + appendCalls);
  return {items, AddWork(mergeWork, ScaleWork(faces, 8 * WorkLevels(faces)))};
}

MetadataWork EstimateMetadataWork(const Registry& registry, std::size_t faces) {
  MetadataWork total;
  for (const Entity entity : registry.view<const RenderingInstanceComponent>()) {
    const auto instance = InstanceMetadataWork(registry, entity, faces);
    total.items = AddWork(total.items, instance.items);
    total.build = AddWork(total.build, instance.build);
    if (total.build > FontResourceGraphCache::kMaximumWork) break;
  }
  return total;
}
}  // namespace

bool FontResourceGraphCache::reserve(std::size_t work) {
  if (stats_->resourceLimit || stats_->work > kMaximumWork || work > kMaximumWork - stats_->work) {
    stats_->resourceLimit = true;
    return false;
  }
  stats_->work += work;
  return true;
}

bool FontResourceGraphCache::Entry::matches(const SVGDocumentHandle& currentDocument,
                                            uint64_t epoch,
                                            const std::vector<FontFaceDependency>& currentFaces,
                                            bool scopeComplete) const {
  return graph && document.lock() == currentDocument &&
         documentRevision == currentDocument->revision() && preparationEpoch == epoch &&
         faces == currentFaces && completedScope == scopeComplete;
}

bool FontResourceGraphCache::prepareEntry(Entry& entry, const SVGDocumentHandle& document) {
  const Registry& registry = document->registry();
  const auto* preparation = registry.ctx().find<FontResourcePreparationState>();
  const uint64_t epoch = preparation ? preparation->epoch : 0;
  const auto* manager = registry.ctx().find<FontManager>();
  const auto faces = manager ? manager->faceDependencies() : std::vector<FontFaceDependency>();
  if (!reserve(ScaleWork(faces.size(), 2))) return false;
  const bool scopeComplete = HasCompletedCurrentScope(registry);
  if (entry.matches(document, epoch, faces, scopeComplete)) return true;
  entry = Entry{};
  const std::size_t traversalWork = CollectionWork(registry);
  // The pre-scan reads at most 64 child evidence records per rendered instance.
  if (!reserve(AddWork(traversalWork,
                       ScaleWork(registry.view<const RenderingInstanceComponent>().size(), 64))))
    return false;
  const auto metadata = EstimateMetadataWork(registry, faces.size());
  const std::size_t buildWork = AddWork(traversalWork, metadata.build);
  if (!reserve(buildWork)) return false;
  entry.document = document;
  entry.documentRevision = document->revision();
  entry.preparationEpoch = epoch;
  entry.completedScope = scopeComplete;
  entry.faces = faces;
  entry.collectionWork =
      AddWork(traversalWork, ScaleWork(metadata.items, WorkLevels(metadata.items) + 1));
  entry.buildWork = buildWork;
  entry.graph = std::make_unique<FontResourceGraph>(registry);
  ++stats_->graphBuilds;
  return true;
}

void FontResourceGraphCache::preserveBeforeRefresh(const SVGDocumentHandle& document) {
  auto& entry = entries_[document.get()];
  (void)prepareEntry(entry, document);
}

void FontResourceGraphCache::finishRefresh(const SVGDocumentHandle& document,
                                           std::span<const Entity> changedOwners) {
  auto& entry = entries_[document.get()];
  if (!entry.graph || !reserve(entry.buildWork)) return;
  entry.graph->invalidateDependents(document->registry(), changedOwners);
  entry.graph->refreshMetadata(document->registry());
  for (auto& targets : entry.targets) targets.clear();
  const auto* manager = document->registry().ctx().find<FontManager>();
  entry.faces = manager ? manager->faceDependencies() : std::vector<FontFaceDependency>();
  entry.preparationEpoch = document->registry().ctx().emplace<FontResourcePreparationState>().epoch;
  entry.completedScope = HasCompletedCurrentScope(document->registry());
}

FontResourceGraph::Collection FontResourceGraphCache::collect(const SVGDocumentHandle& document,
                                                              Entity target,
                                                              FontResourceGraph::Purpose purpose) {
  if (target == entt::null) return {};
  if (purpose == FontResourceGraph::Purpose::RenderedFrame &&
      !HasCompletedCurrentScope(document->registry()))
    return {.needsRender = true};
  auto& entry = entries_[document.get()];
  if (!prepareEntry(entry, document)) return {.resourceLimit = true};
  const auto* scope = document->registry().ctx().find<RenderedFontResourceScope>();
  const uint64_t token = scope ? scope->token : 0;
  auto& targets = entry.targets[static_cast<std::size_t>(purpose)];
  if (const auto found = targets.find(target); found != targets.end()) {
    // prepareEntry already validated the epoch, including the completed exclusion set. Identical
    // immutable evidence may be rebound to this completed token without rescanning the child.
    if (!reserve(found->second.value.dependencies.size())) return {.resourceLimit = true};
    found->second.renderScopeToken = token;
    ++stats_->cacheHits;
    return found->second.value;
  }
  if (!reserve(entry.collectionWork)) return {.resourceLimit = true};
  auto result = entry.graph->collect(document->registry(), target, purpose);
  // Layout-only root identities are not part of painted evidence. Keeping them would turn a
  // repeated-host cache hit back into an O(child scene size) copy.
  std::vector<Entity>().swap(result.textRoots);
  targets.emplace(target, Entry::TargetCollection{result, token});
  ++stats_->targetCollections;
  return result;
}

}  // namespace donner::svg::components
