#include "donner/svg/renderer/AnimationReferences.h"

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <vector>

#include "donner/base/xml/components/AttributesComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/EvaluatedReferenceComponent.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationBaseValue.h"
#include "donner/svg/components/animation/AnimationResourceBudget.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/PaintSystem.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeSystem.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/TextInvalidation.h"
#include "donner/svg/components/text/TextPathComponent.h"

namespace donner::svg::components {
namespace {

struct CachedAnimationImage {
  std::optional<LoadedImageComponent> loaded;
  uint64_t lastUse = 0;
};

struct DecodedAnimationImages {
  uint64_t generation = 0;
  std::map<RcString, CachedAnimationImage> frames;
};

struct AnimationImageCacheBudget {
  std::size_t keys = 0;
  std::size_t bytes = 0;
  uint64_t nextUse = 1;

  bool admit(std::size_t size) {
    constexpr std::size_t kMaximumKeys = 4096;
    constexpr std::size_t kMaximumBytes = 1024 * 1024;
    if (keys >= kMaximumKeys || size > kMaximumBytes - bytes) {
      return false;
    }
    ++keys;
    bytes += size;
    return true;
  }

  uint64_t touch() { return nextUse++; }
};

void RetainDecodedImage(Registry& registry, Entity entity, const std::optional<RcString>& href) {
  auto* loaded = registry.try_get<LoadedImageComponent>(entity);
  if (!href || !loaded || !loaded->image) {
    return;
  }
  auto& cache = registry.get_or_emplace<DecodedAnimationImages>(entity).frames;
  auto slot = cache.find(*href);
  auto* budget = registry.ctx().find<AnimationImageCacheBudget>();
  if (slot == cache.end()) {
    if (!budget) {
      budget = &registry.ctx().emplace<AnimationImageCacheBudget>();
    }
    if (!budget->admit(href->size())) {
      return;
    }
    slot = cache.emplace(*href, CachedAnimationImage{}).first;
  }
  if (!budget) {
    budget = &registry.ctx().emplace<AnimationImageCacheBudget>();
  }
  slot->second.loaded = std::move(*loaded);
  slot->second.lastUse = budget->touch();
}

void ReuseDecodedImage(Registry& registry, Entity entity, const std::optional<RcString>& href) {
  auto* cache = registry.try_get<DecodedAnimationImages>(entity);
  if (!href || !cache) {
    return;
  }
  auto slot = cache->frames.find(*href);
  if (slot != cache->frames.end() && slot->second.loaded) {
    registry.emplace<LoadedImageComponent>(entity, std::move(*slot->second.loaded));
    slot->second.loaded.reset();
  }
}

void EvictDecodedImagesForMiss(Registry& registry, std::size_t requiredBytes) {
  const auto* familyContext = registry.ctx().find<DocumentResourceFamilyContext>();
  auto* cacheBudget = registry.ctx().find<AnimationImageCacheBudget>();
  if (requiredBytes == 0 || !familyContext || !familyContext->budget || !cacheBudget ||
      familyContext->budget->availableBytes(DocumentResourceFamilyBudget::Kind::DecodedImage) >=
          requiredBytes) {
    return;
  }

  struct Candidate {
    Entity entity;
    RcString href;
    uint64_t lastUse;
  };
  std::vector<Candidate> candidates;
  candidates.reserve(cacheBudget->keys);
  for (auto [candidateEntity, cache] : registry.view<DecodedAnimationImages>().each()) {
    for (const auto& [candidateHref, frame] : cache.frames) {
      if (frame.loaded) {
        candidates.push_back({candidateEntity, candidateHref, frame.lastUse});
      }
    }
  }
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate& a, const Candidate& b) { return a.lastUse < b.lastUse; });

  for (const Candidate& candidate : candidates) {
    if (familyContext->budget->availableBytes(DocumentResourceFamilyBudget::Kind::DecodedImage) >=
        requiredBytes) {
      break;
    }
    auto* cache = registry.try_get<DecodedAnimationImages>(candidate.entity);
    if (!cache) {
      continue;
    }
    auto slot = cache->frames.find(candidate.href);
    if (slot != cache->frames.end()) {
      slot->second.loaded.reset();
    }
  }
}

bool SynchronizeImageCachePolicy(Registry& registry, Entity entity) {
  auto* reclaimer = registry.ctx().find<DecodedImageCacheReclaimer>();
  if (!reclaimer) {
    reclaimer = &registry.ctx().emplace<DecodedImageCacheReclaimer>();
  }
  reclaimer->callback = &EvictDecodedImagesForMiss;
  auto& cache = registry.get_or_emplace<DecodedAnimationImages>(entity);
  const auto& resources = registry.ctx().get<ResourceManagerContext>();
  const bool secure = resources.processingMode() == ProcessingMode::SecureStatic ||
                      resources.processingMode() == ProcessingMode::SecureAnimated;
  if (secure || cache.generation != resources.cacheGeneration()) {
    cache.frames.clear();
    cache.generation = resources.cacheGeneration();
    return false;
  }
  return true;
}

struct PendingReferenceChange {
  ElementType type;
  std::optional<RcString> previous;
};

struct AnimationReferenceBackup {
  ElementType type;
  std::optional<RcString> original;
  std::optional<RcString> source;
  RcString applied;
  std::optional<ShadowTreeComponent> originalUse;
};

std::optional<RcString> RawHref(Registry& registry, Entity entity) {
  const auto* attributes = registry.try_get<donner::components::AttributesComponent>(entity);
  return attributes ? attributes->getAttribute("href") : std::nullopt;
}

std::optional<RcString> AuthoredHref(Registry& registry, Entity entity) {
  if (auto href = RawHref(registry, entity)) {
    return href;
  }
  const auto* attributes = registry.try_get<donner::components::AttributesComponent>(entity);
  return attributes ? attributes->getAttribute(xml::XMLQualifiedNameRef("xlink", "href"))
                    : std::nullopt;
}

std::optional<ElementType> ReferenceTargetType(Registry& registry, Entity entity) {
  const auto* type = registry.try_get<ElementTypeComponent>(entity);
  if (!type || !IsAnimationInSvgNamespace(registry, entity)) {
    return std::nullopt;
  }
  constexpr std::array types = {
      ElementType::Use,     ElementType::LinearGradient, ElementType::RadialGradient,
      ElementType::Pattern, ElementType::Filter,         ElementType::Image,
      ElementType::FeImage, ElementType::TextPath};
  return std::find(types.begin(), types.end(), type->type()) != types.end()
             ? std::optional(type->type())
             : std::nullopt;
}

void InvalidateReference(Registry& registry, Entity entity, ElementType type) {
  EntityHandle handle(registry, entity);
  ShadowTreeSystem().teardownInstances(handle);
  registry.remove<ComputedShadowTreeComponent, ExternalUseComponent, LoadedImageComponent,
                  LoadedSVGImageComponent, DeferredDecodedImageComponent,
                  EvaluatedReferenceComponent<PaintSystem>>(entity);
  if (type == ElementType::LinearGradient || type == ElementType::RadialGradient ||
      type == ElementType::Pattern) {
    registry.remove<ShadowTreeComponent>(entity);
  }
  LayoutSystem().invalidate(handle);
  if (type == ElementType::TextPath) {
    const Entity root = InvalidateTextLayout(handle);
    if (root != entt::null) {
      registry.remove<ComputedTextComponent>(root);
    }
  }
}

template <typename Component>
void WriteReference(Registry& registry, Entity entity, const std::optional<RcString>& value) {
  if constexpr (std::is_same_v<decltype(Component::href), RcString>) {
    if (value) {
      registry.get_or_emplace<Component>(entity).href = *value;
    } else if constexpr (std::is_same_v<Component, ImageComponent>) {
      registry.remove<Component>(entity);
    } else if (auto* component = registry.try_get<Component>(entity)) {
      component->href = RcString();
    }
  } else {
    registry.get_or_emplace<Component>(entity).href =
        value ? std::optional(Reference(*value)) : std::nullopt;
  }
}

void SetReference(Registry& registry, Entity entity, ElementType type,
                  const std::optional<RcString>& value) {
  if (!registry.all_of<PendingReferenceChange>(entity)) {
    registry.emplace<PendingReferenceChange>(entity, type,
                                             AnimationReferenceValue(registry, entity));
  }
  switch (type) {
    case ElementType::Use:
      if (value) {
        auto& shadow = registry.get_or_emplace<ShadowTreeComponent>(entity);
        shadow.setMainHref(*value);
        shadow.setsContextColors = true;
      } else {
        registry.remove<ShadowTreeComponent>(entity);
      }
      break;
    case ElementType::LinearGradient:
    case ElementType::RadialGradient:
      WriteReference<GradientComponent>(registry, entity, value);
      break;
    case ElementType::Pattern: WriteReference<PatternComponent>(registry, entity, value); break;
    case ElementType::Filter: WriteReference<FilterComponent>(registry, entity, value); break;
    case ElementType::Image: WriteReference<ImageComponent>(registry, entity, value); break;
    case ElementType::FeImage:
      WriteReference<ImageComponent>(registry, entity, value);
      WriteReference<FEImageComponent>(registry, entity, value);
      break;
    case ElementType::TextPath: WriteReference<TextPathComponent>(registry, entity, value); break;
    default: break;
  }
}

void RestoreReference(Registry& registry, Entity entity) {
  auto* stored = registry.try_get<AnimationReferenceBackup>(entity);
  if (!stored) {
    return;
  }
  auto backup = std::move(*stored);
  registry.remove<AnimationReferenceBackup>(entity);
  const auto source = AuthoredHref(registry, entity);
  if (source != backup.source) {
    SetReference(registry, entity, backup.type, source);
  } else if (AnimationReferenceValue(registry, entity) == std::optional(backup.applied)) {
    SetReference(registry, entity, backup.type, backup.original);
    if (backup.originalUse) {
      registry.emplace_or_replace<ShadowTreeComponent>(entity, *backup.originalUse);
    }
  }
}

bool ApplyReference(Registry& registry, Entity entity, ElementType type, const std::string& value) {
  auto& budget = registry.ctx().get<AnimationResourceBudget>();
  const bool image = type == ElementType::Image || type == ElementType::FeImage;
  const auto maximum =
      image ? AnimationResourceBudget::kMaximumSourceBytes : Reference::kMaximumHrefBytes;
  if (value.size() > maximum || value.find('\0') != std::string::npos) {
    return true;
  }
  const auto original = AnimationReferenceValue(registry, entity);
  const auto source = AuthoredHref(registry, entity);
  if (!budget.reserveOutput(sizeof(AnimationReferenceBackup) + sizeof(PendingReferenceChange)) ||
      !budget.reserveOutput(value.size()) ||
      !budget.reserveOutput(original ? original->size() : 0) ||
      !budget.reserveOutput(source ? source->size() : 0)) {
    return false;
  }
  RcString applied(value);
  if (!image && !Reference(applied).hasSafeRepresentation()) {
    return true;
  }
  auto& backup =
      registry.emplace<AnimationReferenceBackup>(entity, type, original, source, applied);
  if (type == ElementType::Use) {
    if (const auto* shadow = registry.try_get<ShadowTreeComponent>(entity)) {
      backup.originalUse = *shadow;
    }
  }
  SetReference(registry, entity, type, applied);
  return true;
}

}  // namespace

void FinalizeAnimationReferences(Registry& registry) {
  for (Entity entity : registry.view<PendingReferenceChange>()) {
    auto pending = std::move(registry.get<PendingReferenceChange>(entity));
    registry.remove<PendingReferenceChange>(entity);
    const auto current = AnimationReferenceValue(registry, entity);
    const bool image = pending.type == ElementType::Image || pending.type == ElementType::FeImage;
    const bool reuseAllowed = image && SynchronizeImageCachePolicy(registry, entity);
    if (current != pending.previous || (image && !reuseAllowed)) {
      if (reuseAllowed) {
        RetainDecodedImage(registry, entity, pending.previous);
      }
      InvalidateReference(registry, entity, pending.type);
      if (reuseAllowed) {
        ReuseDecodedImage(registry, entity, current);
      }
    }
  }
}

void RestoreAnimationReferences(Registry& registry, Entity target) {
  if (target != entt::null) {
    RestoreReference(registry, target);
  } else {
    for (Entity entity : registry.view<AnimationReferenceBackup>()) {
      RestoreReference(registry, entity);
    }
  }
}

bool ApplyAnimationReferences(Registry& registry) {
  for (auto [entity, values] : registry.view<AnimatedValuesComponent>().each()) {
    const auto type = ReferenceTargetType(registry, entity);
    if (!type) {
      continue;
    }
    auto it = values.overrides.find("href");
    if (it == values.overrides.end() && !RawHref(registry, entity)) {
      it = values.overrides.find("xlink:href");
    }
    if (it != values.overrides.end() && !ApplyReference(registry, entity, *type, it->second)) {
      return false;
    }
  }
  return true;
}

}  // namespace donner::svg::components
