#include "donner/svg/renderer/RenderingContext.h"

#include <optional>
#include <set>

#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterSystem.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/SymbolComponent.h"
#include "donner/svg/components/paint/ClipPathComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/MarkerComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/components/paint/PaintSystem.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/components/resources/ResourceManagerContext.h"
#include "donner/svg/components/shadow/ComputedShadowTreeComponent.h"
#include "donner/svg/components/shape/CircleComponent.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/EllipseComponent.h"
#include "donner/svg/components/shape/LineComponent.h"
#include "donner/svg/components/shape/PathComponent.h"
#include "donner/svg/components/shape/RectComponent.h"
#include "donner/svg/graph/Reference.h"
#ifdef DONNER_TEXT_ENABLED
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"
#endif
#include "donner/svg/components/shadow/OffscreenShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowBranch.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowTreeSystem.h"
#include "donner/svg/components/shape/ComputedPathComponent.h"
#include "donner/svg/components/shape/ShapeSystem.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/StyleSystem.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/components/text/TextSystem.h"
#include "donner/svg/graph/RecursionGuard.h"
#include "donner/svg/graph/Reference.h"

namespace donner::svg::components {

namespace {

/// Get or create the RenderTreeState in the registry context.
RenderTreeState& getRenderTreeState(Registry& registry) {
  if (!registry.ctx().contains<RenderTreeState>()) {
    registry.ctx().emplace<RenderTreeState>();
  }
  return registry.ctx().get<RenderTreeState>();
}

/**
 * The current value of the context-fill and context-stroke paint servers, based on the rules
 * described here: https://www.w3.org/TR/SVG2/painting.html#SpecifyingPaint
 */
struct ContextPaintServers {
  ResolvedPaintServer contextFill = PaintServer::None();
  ResolvedPaintServer contextStroke = PaintServer::None();
};

/**
 * Creates a ShadowTreeSystem with a handler for shadow sized element components.
 * This allows LayoutSystem to process shadow sized elements without creating a circular dependency.
 */
ShadowTreeSystem createShadowTreeSystem() {
  return ShadowTreeSystem([](Registry& registry, Entity shadowEntity, EntityHandle useEntity,
                             Entity symbolEntity, ShadowBranchType branchType,
                             ParseWarningSink& warningSink) -> bool {
    // Only create shadow sized element components for the main branch
    if (branchType != ShadowBranchType::Main) {
      return false;
    }

    // Use LayoutSystem to handle the creation of shadow sized element components
    return LayoutSystem().createShadowSizedElementComponent(registry, shadowEntity, useEntity,
                                                            symbolEntity, branchType, warningSink);
  });
}

bool IsValidPaintServer(EntityHandle handle) {
  return handle.any_of<ComputedGradientComponent, ComputedPatternComponent>();
}

bool IsValidClipPath(EntityHandle handle) {
  return handle.all_of<ClipPathComponent>();
}

bool IsValidMask(EntityHandle handle) {
  return handle.all_of<MaskComponent>();
}

bool IsValidMarker(EntityHandle handle) {
  return handle.all_of<MarkerComponent>();
}

class RenderingContextImpl {
public:
  explicit RenderingContextImpl(Registry& registry, bool verbose,
                                const ContextPaintServers& initialContext = {},
                                bool ignoreNonrenderable = false)
      : registry_(registry),
        verbose_(verbose),
        ignoreNonrenderable_(ignoreNonrenderable),
        contextPaintServers_(initialContext) {
    // Get the LayoutSystem from the registry context if available
    LayoutSystem* layoutSystem = nullptr;
    if (registry_.ctx().contains<LayoutSystem*>()) {
      layoutSystem = registry_.ctx().get<LayoutSystem*>();
    }

    canvasFromDocumentWorldTransform_ =
        layoutSystem ? layoutSystem->getCanvasFromDocumentTransform(registry)
                     : LayoutSystem().getCanvasFromDocumentTransform(registry);
    if (verbose_) {
      std::cout << "Canvas from document-world transform: " << canvasFromDocumentWorldTransform_
                << "\n";
    }
  }

  /**
   * Traverse a tree, instantiating each entity in the tree.
   *
   * @param treeEntity Current entity in the tree or shadow tree.
   * @param lastRenderedEntityIfSubtree Optional, entity of the last rendered element if this is a
   *   subtree.
   * @return The last rendered entity.
   */
  // TODO(jwmcglynn): Since 'stroke' and 'fill' may reference the same tree, we need to create two
  // instances of it in the render tree.
  void traverseTree(Entity treeEntity, Entity* lastRenderedEntityIfSubtree = nullptr) {
    const auto* shadowEntityComponent = registry_.try_get<ShadowEntityComponent>(treeEntity);
    const Entity styleEntity = treeEntity;
    const EntityHandle dataHandle(
        registry_, shadowEntityComponent ? shadowEntityComponent->lightEntity : treeEntity);
    bool traverseChildren = true;
    std::optional<Box2d> clipRect;
    int layerDepth = 0;
    std::optional<ContextPaintServers> savedContextPaintServers;
    const bool isShape = dataHandle.all_of<ComputedPathComponent>();

    if (!dataHandle.all_of<ElementTypeComponent>()) {
      return;
    }

    // ShadowOnlyChildren elements (e.g., <mask>, <pattern>) don't render content in the light
    // tree — only when instantiated as shadow trees. Track this so we can skip mask/clippath
    // resolution on them (which would wastefully consume shadow tree entities needed by the
    // actual users of those masks).
    bool isShadowOnlyInLightTree = false;

    if (const auto* behavior = dataHandle.try_get<RenderingBehaviorComponent>()) {
      if (behavior->behavior == RenderingBehavior::Nonrenderable && !ignoreNonrenderable_) {
        return;
      } else if (behavior->behavior == RenderingBehavior::NoTraverseChildren) {
        traverseChildren = false;
      } else if (behavior->behavior == RenderingBehavior::ShadowOnlyChildren) {
        if (!shadowEntityComponent) {
          traverseChildren = false;
          isShadowOnlyInLightTree = true;
        }
      }
    }

    const auto& styleComponent = registry_.get<ComputedStyleComponent>(styleEntity);
    const auto& properties = styleComponent.properties.value();

    if (properties.display.getRequired() == Display::None) {
      return;
    }

    bool isEmpty = false;

    // Check for regular sized element component
    if (const auto* sizedElement = dataHandle.try_get<ComputedSizedElementComponent>()) {
      isEmpty = sizedElement->bounds.isEmpty();
    }
    // Check for shadow sized element component if regular one doesn't exist or is empty
    else if (const auto* shadowSizedElement =
                 dataHandle.try_get<ComputedShadowSizedElementComponent>()) {
      isEmpty = shadowSizedElement->bounds.isEmpty();
    }

    if (isEmpty) {
      return;
    }

    if (auto maybeClipRect = LayoutSystem().clipRect(EntityHandle(registry_, treeEntity))) {
      const Overflow overflow = styleComponent.properties->overflow.getRequired();

      if (overflow != Overflow::Visible && overflow != Overflow::Auto) {
        ++layerDepth;
        clipRect = maybeClipRect;
      }
    }

    auto& instance = registry_.emplace<RenderingInstanceComponent>(styleEntity);
    instance.drawOrder = drawOrder_++;

    const auto& absoluteTransformComponent =
        LayoutSystem().getAbsoluteTransformComponent(EntityHandle(registry_, treeEntity));
    instance.entityFromWorldTransform =
        absoluteTransformComponent.entityFromWorld * (absoluteTransformComponent.worldIsCanvas
                                                          ? canvasFromDocumentWorldTransform_
                                                          : Transform2d());

    instance.clipRect = clipRect;
    instance.dataEntity = dataHandle.entity();

    if (verbose_) {
      std::cout << "Instantiating " << dataHandle.get<ElementTypeComponent>().type() << " ";

      if (const auto* idComponent = dataHandle.try_get<IdComponent>()) {
        std::cout << "id=" << idComponent->id() << " ";
      }

      std::cout << dataHandle.entity();
      if (instance.isShadow(registry_)) {
        std::cout << " (shadow " << styleEntity << ")";
      }

      std::cout << "\n";
    }

    const std::vector<FilterEffect>& filterEffects = properties.filter.getRequired();
    const bool hasFilterEffect = !filterEffects.empty();

    if (properties.visibility.getRequired() != Visibility::Visible) {
      instance.visible = false;
    }

    if (hasFilterEffect) {
      instance.resolvedFilter = resolveFilter(dataHandle, filterEffects);
    }

    if (!isShadowOnlyInLightTree) {
      if (properties.clipPath.get()) {
        if (auto resolved = resolveClipPath(dataHandle, properties.clipPath.getRequired());
            resolved.valid()) {
          instance.clipPath = resolved;

          // Get the paths and store them in a ComputedClipPaths component.
          auto& clipPaths = registry_.emplace<ComputedClipPathsComponent>(styleEntity);

          RecursionGuard guard;
          guard.add(styleEntity);
          collectClipPaths(resolved.reference.handle, clipPaths.clipPaths, guard);
        }
      }

      if (properties.mask.get()) {
        if (auto resolved =
                resolveMask(EntityHandle(registry_, styleEntity), properties.mask.getRequired());
            resolved.valid()) {
          instance.mask = std::move(resolved);
        }
      }
    }

    if (isShape) {
      if (properties.markerStart.get()) {
        if (auto resolved = resolveMarker(EntityHandle(registry_, styleEntity),
                                          properties.markerStart.getRequired(),
                                          ShadowBranchType::OffscreenMarkerStart);
            resolved.valid()) {
          instance.markerStart = resolved;
        }
      }

      if (properties.markerMid.get()) {
        if (auto resolved = resolveMarker(EntityHandle(registry_, styleEntity),
                                          properties.markerMid.getRequired(),
                                          ShadowBranchType::OffscreenMarkerMid);
            resolved.valid()) {
          instance.markerMid = resolved;
        }
      }

      if (properties.markerEnd.get()) {
        if (auto resolved = resolveMarker(EntityHandle(registry_, styleEntity),
                                          properties.markerEnd.getRequired(),
                                          ShadowBranchType::OffscreenMarkerEnd);
            resolved.valid()) {
          instance.markerEnd = resolved;
        }
      }
    }

    // Create a new layer if opacity is less than 1 or if there is an effect that requires an
    // isolated group.
    if (properties.opacity.getRequired() < 1.0) {
      instance.isolatedLayer = true;
      ++layerDepth;
    }

    if (instance.resolvedFilter) {
      instance.isolatedLayer = true;
      ++layerDepth;
    }

    if (instance.clipPath) {
      instance.isolatedLayer = true;
      ++layerDepth;
    }

    if (instance.mask) {
      instance.isolatedLayer = true;
      layerDepth += 2;
    }

    if (properties.mixBlendMode.getRequired() != MixBlendMode::Normal ||
        properties.isolation.getRequired() == Isolation::Isolate) {
      instance.isolatedLayer = true;
      ++layerDepth;
    }

    const ShadowTreeComponent* shadowTree = dataHandle.try_get<ShadowTreeComponent>();
    const bool setsContextColors = shadowTree && shadowTree->setsContextColors;

    if (setsContextColors || (instance.visible && (dataHandle.all_of<ComputedPathComponent>() ||
                                                   dataHandle.all_of<ComputedTextComponent>()))) {
      if (auto fill = properties.fill.get()) {
        instance.resolvedFill = resolvePaint(ShadowBranchType::OffscreenFill, dataHandle,
                                             fill.value(), contextPaintServers_);
      }

      if (auto stroke = properties.stroke.get()) {
        instance.resolvedStroke = resolvePaint(ShadowBranchType::OffscreenStroke, dataHandle,
                                               stroke.value(), contextPaintServers_);
      }

      // Save the current context paint servers if this is a shadow tree host.
      if (setsContextColors) {
        savedContextPaintServers = contextPaintServers_;
        contextPaintServers_.contextFill = instance.resolvedFill;
        contextPaintServers_.contextStroke = instance.resolvedStroke;
      }
    }

    lastRenderedEntity_ = styleEntity;

    if (traverseChildren) {
      const auto& tree = registry_.get<donner::components::TreeComponent>(treeEntity);
      for (auto cur = tree.firstChild(); cur != entt::null;
           cur = registry_.get<donner::components::TreeComponent>(cur).nextSibling()) {
        traverseTree(cur);
      }
    }

    if (layerDepth > 0) {
      instance.subtreeInfo = SubtreeInfo{styleEntity, lastRenderedEntity_, layerDepth};
    }

    if (savedContextPaintServers) {
      contextPaintServers_ = savedContextPaintServers.value();
    }

    if (lastRenderedEntityIfSubtree) {
      *lastRenderedEntityIfSubtree = lastRenderedEntity_;
    }
  }

  bool collectClipPaths(EntityHandle clipPathHandle,
                        std::vector<ComputedClipPathsComponent::ClipPath>& clipPaths,
                        RecursionGuard guard, int layer = 0) {
    bool hasAnyChildren = false;
    bool encounteredInvalidReference = false;
    const auto appendClipPathFromEntity = [&](EntityHandle entity, bool enforceVisibility) {
      if (encounteredInvalidReference) {
        return;
      }

      // Shadow entities (from <use>) store data on the light entity but styles on the shadow.
      const auto* shadowEntity = entity.try_get<ShadowEntityComponent>();
      const EntityHandle dataEntity =
          shadowEntity ? EntityHandle(registry_, shadowEntity->lightEntity) : entity;

      const auto* clipPathData = dataEntity.try_get<components::ComputedPathComponent>();
      const auto* computedStyle = entity.try_get<components::ComputedStyleComponent>();
      if (!clipPathData || !computedStyle) {
        return;
      }

      const auto& style = computedStyle->properties.value();
      if (enforceVisibility && (style.visibility.getRequired() != Visibility::Visible ||
                                style.display.getRequired() == Display::None)) {
        return;
      }

      // Check to see if this element has its own clip paths set.
      if (style.clipPath.get()) {
        if (auto resolved = resolveClipPath(entity, style.clipPath.getRequired());
            resolved.valid()) {
          if (!guard.hasRecursion(resolved.reference.handle)) {
            if (!collectClipPaths(resolved.reference.handle, clipPaths,
                                  guard.with(resolved.reference.handle), layer + 1)) {
              encounteredInvalidReference = true;
              return;
            }
          }
        }
      }

      hasAnyChildren = true;

      const Transform2d entityFromParent = LayoutSystem().getEntityFromWorldTransform(entity);
      const ClipRule clipRule = style.clipRule.get().value_or(ClipRule::NonZero);
      clipPaths.emplace_back(clipPathData->spline, entityFromParent, clipRule, layer);
    };

    // Check for clip-path on the <clipPath> itself
    if (const auto* computedStyle = clipPathHandle.try_get<components::ComputedStyleComponent>()) {
      const auto& style = computedStyle->properties.value();
      if (style.clipPath.get()) {
        if (auto resolved = resolveClipPath(clipPathHandle, style.clipPath.getRequired());
            resolved.valid()) {
          if (!guard.hasRecursion(resolved.reference.handle)) {
            if (!collectClipPaths(resolved.reference.handle, clipPaths,
                                  guard.with(resolved.reference.handle), layer + 1)) {
              return false;
            }
          }
        }
      }
    }

    donner::components::ForAllChildren(clipPathHandle, [&](EntityHandle child) {
      appendClipPathFromEntity(child, true);

      const auto* typeComponent = child.try_get<ElementTypeComponent>();
      if (!typeComponent || typeComponent->type() != ElementType::Use) {
        return;
      }

      const auto* useStyle = child.try_get<components::ComputedStyleComponent>();
      if (!useStyle) {
        return;
      }

      const auto& useProperties = useStyle->properties.value();
      if (useProperties.visibility.getRequired() != Visibility::Visible ||
          useProperties.display.getRequired() == Display::None) {
        return;
      }

      if (const auto* computedShadow = child.try_get<ComputedShadowTreeComponent>();
          computedShadow && computedShadow->mainBranch) {
        const Entity shadowRoot = computedShadow->mainBranch->shadowRoot();
        if (shadowRoot != entt::null) {
          appendClipPathFromEntity(EntityHandle(registry_, shadowRoot), false);
        }
      }
    });

    if (encounteredInvalidReference) {
      return false;
    }

    return hasAnyChildren;
  }

  std::optional<SubtreeInfo> instantiateOffscreenSubtree(EntityHandle shadowHostHandle,
                                                         ShadowBranchType branchType) {
    const auto* computedShadowTree = shadowHostHandle.try_get<ComputedShadowTreeComponent>();
    if (!computedShadowTree) {
      // If there's not a shadow tree, there is no offscreen subtree.  This is a gradient and not a
      // <pattern>.
      return std::nullopt;
    }

    const std::optional<size_t> maybeShadowIndex =
        computedShadowTree->findOffscreenShadow(branchType);
    if (!maybeShadowIndex) {
      // Theres no subtree here.
      return std::nullopt;
    }

    Entity firstEntity = computedShadowTree->offscreenShadowRoot(maybeShadowIndex.value());

    // If this subtree was already traversed (e.g., multiple shadow entities sharing the same
    // light entity's offscreen branch), reuse the cached SubtreeInfo to avoid re-traversal
    // and assertion failures from duplicate RenderingInstanceComponent emplacement. Mask
    // recursion is detected at a higher level by activeMaskElements_ in resolveMask().
    if (registry_.try_get<RenderingInstanceComponent>(firstEntity)) {
      const auto& rootInst = registry_.get<RenderingInstanceComponent>(firstEntity);
      if (rootInst.subtreeInfo) {
        return SubtreeInfo{firstEntity, rootInst.subtreeInfo->lastRenderedEntity, 0};
      }
      return SubtreeInfo{firstEntity, firstEntity, 0};
    }

    Entity lastEntity = entt::null;
    traverseTree(firstEntity, &lastEntity);

    if (lastEntity != entt::null) {
      return SubtreeInfo{firstEntity, lastEntity, 0};
    } else {
      // This could happen if the subtree has no nodes.
      return std::nullopt;
    }
  }

  ResolvedPaintServer resolvePaint(ShadowBranchType branchType, EntityHandle dataHandle,
                                   const PaintServer& paint,
                                   const ContextPaintServers& contextPaintServers) {
    if (paint.is<PaintServer::Solid>()) {
      return paint.get<PaintServer::Solid>();
    } else if (paint.is<PaintServer::ElementReference>()) {
      const PaintServer::ElementReference& ref = paint.get<PaintServer::ElementReference>();

      // Only resolve paints if the paint server references a supported <pattern> or gradient
      // element, and the shadow tree was instantiated. If the shadow tree is not instantiated, that
      // indicates there was recursion and we treat the reference as invalid.
      if (auto resolvedRef = ref.reference.resolve(*dataHandle.registry());
          resolvedRef && IsValidPaintServer(resolvedRef->handle)) {
        return PaintResolvedReference{resolvedRef.value(), ref.fallback,
                                      instantiateOffscreenSubtree(dataHandle, branchType)};
      } else if (ref.fallback) {
        return PaintServer::Solid(ref.fallback.value());
      } else {
        return PaintServer::None();
      }
    } else if (paint.is<PaintServer::ContextFill>()) {
      return contextPaintServers.contextFill;
    } else if (paint.is<PaintServer::ContextStroke>()) {
      return contextPaintServers.contextStroke;
    } else {
      return PaintServer::None();
    }
  }

  ResolvedClipPath resolveClipPath(EntityHandle dataHandle, const Reference& reference) {
    // Only resolve paints if the paint server references a supported <clipPath> element, and the
    // shadow tree was instantiated. If the shadow tree is not instantiated, that indicates there
    // was recursion and we treat the reference as invalid.
    if (auto resolvedRef = reference.resolve(*dataHandle.registry());
        resolvedRef && IsValidClipPath(resolvedRef->handle)) {
      return ResolvedClipPath{resolvedRef.value(),
                              resolvedRef->handle.get<ClipPathComponent>().clipPathUnits.value_or(
                                  ClipPathUnits::Default)};
    }

    return ResolvedClipPath{ResolvedReference{EntityHandle()}, ClipPathUnits::Default};
  }

  ResolvedMask resolveMask(EntityHandle styleHandle, const Reference& reference) {
    // Only resolve paints if the paint server references a supported <mask> element, and the
    // shadow tree was instantiated. If the shadow tree is not instantiated, that indicates
    // there was recursion and we treat the reference as invalid.
    if (auto resolvedRef = reference.resolve(*styleHandle.registry());
        resolvedRef && IsValidMask(resolvedRef->handle)) {
      // Check for mutual recursion: if the referenced mask element is already being rendered
      // as mask content higher up the call stack (e.g., mask1→mask2→mask1), break the cycle.
      const Entity maskElement = resolvedRef->handle.entity();
      if (activeMaskElements_.count(maskElement)) {
        return ResolvedMask{ResolvedReference{EntityHandle()}, std::nullopt,
                            MaskContentUnits::Default};
      }

      // One-step lookahead: if the referenced mask element itself has a mask property that
      // references an already-active mask element, applying this mask would create a cycle
      // one level deeper. Break the cycle now to match SVG spec behavior where all masks
      // in a mutual recursion cycle have their mask attributes treated as "none".
      if (const auto* maskStyle = resolvedRef->handle.try_get<ComputedStyleComponent>()) {
        if (maskStyle->properties.has_value()) {
          if (auto nestedMask = maskStyle->properties->mask.get()) {
            if (auto nestedRef = nestedMask->resolve(*styleHandle.registry());
                nestedRef && activeMaskElements_.count(nestedRef->handle.entity())) {
              return ResolvedMask{ResolvedReference{EntityHandle()}, std::nullopt,
                                  MaskContentUnits::Default};
            }
          }
        }
      }

      // When the style entity is a shadow entity (e.g. a child inside a mask's shadow tree),
      // the ComputedShadowTreeComponent lives on the corresponding light entity, not on the
      // shadow entity itself. Fall back to the light entity for lookup.
      EntityHandle shadowTreeHost = styleHandle;
      if (const auto* shadowEntity = styleHandle.try_get<ShadowEntityComponent>()) {
        shadowTreeHost = EntityHandle(*styleHandle.registry(), shadowEntity->lightEntity);
      }

      if (const auto* computedShadow = shadowTreeHost.try_get<ComputedShadowTreeComponent>();
          computedShadow &&
          computedShadow->findOffscreenShadow(ShadowBranchType::OffscreenMask).has_value()) {
        activeMaskElements_.insert(maskElement);
        auto subtree = instantiateOffscreenSubtree(shadowTreeHost, ShadowBranchType::OffscreenMask);
        activeMaskElements_.erase(maskElement);

        return ResolvedMask{resolvedRef.value(), std::move(subtree),
                            resolvedRef->handle.get<MaskComponent>().maskContentUnits};
      }
    }

    return ResolvedMask{ResolvedReference{EntityHandle()}, std::nullopt, MaskContentUnits::Default};
  }

  ResolvedMarker resolveMarker(EntityHandle styleHandle, const Reference& reference,
                               ShadowBranchType branchType) {
    if (auto resolvedRef = reference.resolve(*styleHandle.registry());
        resolvedRef && IsValidMarker(resolvedRef->handle)) {
      const Entity markerElement = resolvedRef->handle.entity();
      if (activeMarkerElements_.count(markerElement)) {
        return ResolvedMarker{ResolvedReference{EntityHandle()}, std::nullopt,
                              MarkerUnits::Default};
      }

      if (const auto* computedShadow = styleHandle.try_get<ComputedShadowTreeComponent>();
          computedShadow && computedShadow->findOffscreenShadow(branchType).has_value()) {
        activeMarkerElements_.insert(markerElement);
        auto subtree = instantiateOffscreenSubtree(styleHandle, branchType);
        activeMarkerElements_.erase(markerElement);

        return ResolvedMarker{resolvedRef.value(), std::move(subtree),
                              resolvedRef->handle.get<MarkerComponent>().markerUnits};
      }
    }
    return ResolvedMarker{ResolvedReference{EntityHandle()}, std::nullopt, MarkerUnits::Default};
  }

  ResolvedFilterEffect resolveFilter(EntityHandle dataHandle,
                                     const std::vector<FilterEffect>& filters) {
    // If the list is exactly one element reference, resolve it directly to keep the existing
    // ResolvedReference path (which provides filter region info from the <filter> element).
    if (filters.size() == 1 && filters.front().is<FilterEffect::ElementReference>()) {
      const FilterEffect::ElementReference& ref =
          filters.front().get<FilterEffect::ElementReference>();

      if (auto resolvedRef = ref.reference.resolve(*dataHandle.registry());
          resolvedRef && resolvedRef->handle.all_of<ComputedFilterComponent>()) {
        return resolvedRef.value();
      } else {
        return std::vector<FilterEffect>();
      }
    }

    // For lists of filter functions (possibly including url() references), return the vector
    // directly. The renderer will handle resolution of element references within the list.
    return filters;
  }

private:
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  Registry& registry_;        //!< Registry being operated on for rendering..
  bool verbose_;              //!< If true, enable verbose logging.
  bool ignoreNonrenderable_;  //!< If true, skip the Nonrenderable behavior check.

  int drawOrder_ = 0;                       //!< The current draw order index.
  Entity lastRenderedEntity_ = entt::null;  //!< The last entity rendered.
  /// Holds the current paint servers for resolving the `context-fill` and `context-stroke` paint
  /// values.
  ContextPaintServers contextPaintServers_;

  /// Transform from the canvas to the SVG document root, for the current canvas scale.
  Transform2d canvasFromDocumentWorldTransform_;

  /// Tracks mask elements currently being rendered to detect mutual recursion
  /// (e.g., mask1→mask2→mask1). When a mask reference resolves to an element already in this
  /// set, the cycle is broken by treating the mask attribute as "none".
  std::set<Entity> activeMaskElements_;

  /// Tracks marker elements currently being instantiated so recursive marker references stop
  /// after the first level instead of repeatedly expanding the same cycle.
  std::set<Entity> activeMarkerElements_;
};

void InstantiatePaintShadowTree(Registry& registry, Entity entity, ShadowBranchType branchType,
                                const PaintServer& paint, ParseWarningSink& warningSink) {
  if (paint.is<PaintServer::ElementReference>()) {
    const PaintServer::ElementReference& ref = paint.get<PaintServer::ElementReference>();

    if (auto resolvedRef = ref.reference.resolve(registry);
        resolvedRef && resolvedRef->handle.all_of<PatternComponent>()) {
      auto& offscreenShadowComponent =
          registry.get_or_emplace<OffscreenShadowTreeComponent>(entity);
      offscreenShadowComponent.setBranchHref(branchType, ref.reference.href);
    }
  }
}

void InstantiateMaskShadowTree(Registry& registry, Entity entity, const Reference& reference,
                               ParseWarningSink& warningSink) {
  if (auto resolvedRef = reference.resolve(registry);
      resolvedRef && resolvedRef->handle.all_of<MaskComponent>()) {
    auto& offscreenShadowComponent = registry.get_or_emplace<OffscreenShadowTreeComponent>(entity);
    offscreenShadowComponent.setBranchHref(ShadowBranchType::OffscreenMask, reference.href);
  }
}

void InstantiateMarkerShadowTree(Registry& registry, Entity entity, ShadowBranchType branchType,
                                 const Reference& reference, ParseWarningSink& warningSink) {
  if (auto resolvedRef = reference.resolve(registry);
      resolvedRef && resolvedRef->handle.all_of<MarkerComponent>()) {
    auto& offscreenShadowComponent = registry.get_or_emplace<OffscreenShadowTreeComponent>(entity);
    offscreenShadowComponent.setBranchHref(branchType, reference.href);
  }
}

/// Configuration for hit-testing based on the pointer-events property.
struct HitTestConfig {
  bool testFill;              ///< Whether to test fill intersection.
  bool testStroke;            ///< Whether to test stroke intersection.
  bool requireVisible;        ///< Whether element must be visible.
  bool requirePaintedFill;    ///< Whether fill must be non-none.
  bool requirePaintedStroke;  ///< Whether stroke must be non-none.
};

/// Returns the hit-test configuration for a given PointerEvents value.
HitTestConfig configFromPointerEvents(PointerEvents pe) {
  switch (pe) {
    case PointerEvents::None:
      return {false, false, false, false, false};  // Should be filtered before this call.
    case PointerEvents::BoundingBox:
      return {false, false, false, false, false};  // Handled separately.
    case PointerEvents::VisiblePainted: return {true, true, true, true, true};
    case PointerEvents::VisibleFill: return {true, false, true, false, false};
    case PointerEvents::VisibleStroke: return {false, true, true, false, false};
    case PointerEvents::Visible: return {true, true, true, false, false};
    case PointerEvents::Painted: return {true, true, false, true, true};
    case PointerEvents::Fill: return {true, false, false, false, false};
    case PointerEvents::Stroke: return {false, true, false, false, false};
    case PointerEvents::All: return {true, true, false, false, false};
  }
  UTILS_UNREACHABLE();
}

}  // namespace

RenderingContext::RenderingContext(Registry& registry) : registry_(registry) {}

void RenderingContext::setInitialContextPaint(const ResolvedPaintServer& fill,
                                              const ResolvedPaintServer& stroke) {
  initialContextFill_ = fill;
  initialContextStroke_ = stroke;
  getRenderTreeState(registry_).needsFullRebuild = true;
}

void RenderingContext::clearInitialContextPaint() {
  initialContextFill_.reset();
  initialContextStroke_.reset();
  getRenderTreeState(registry_).needsFullRebuild = true;
}

void RenderingContext::instantiateRenderTree(bool verbose, ParseWarningSink& warningSink) {
  auto& renderState = getRenderTreeState(registry_);
  const bool hasDirtyEntities = !registry_.view<DirtyFlagsComponent>().empty();

  // Fast path: if the render tree has been built, nothing is dirty, and no full rebuild
  // is required, skip all recomputation.
  if (renderState.hasBeenBuilt && !renderState.needsFullStyleRecompute &&
      !renderState.needsFullRebuild && !hasDirtyEntities) {
    return;
  }

  ensureComputedComponents(warningSink);
  instantiateRenderTreeWithPrecomputedTree(verbose);

  renderState.needsFullStyleRecompute = false;
  renderState.hasBeenBuilt = true;
}

void RenderingContext::ensureComputedComponents(ParseWarningSink& warningSink) {
  auto& renderState = getRenderTreeState(registry_);
  const bool hasDirtyEntities = !registry_.view<DirtyFlagsComponent>().empty();

  if (!renderState.needsFullStyleRecompute && !renderState.needsFullRebuild && !hasDirtyEntities) {
    return;
  }

  // Full rebuild path: tear down shadow trees and recompute everything.
  // TODO(jwmcglynn): Support partial invalidation, where we only recompute dirty entities
  // instead of the full tree.
  for (auto view = registry_.view<ComputedShadowTreeComponent>(); auto entity : view) {
    auto& shadow = view.get<ComputedShadowTreeComponent>(entity);
    createShadowTreeSystem().teardown(registry_, shadow);
  }
  registry_.clear<ComputedShadowTreeComponent>();
  registry_.clear<RenderingInstanceComponent>();
  registry_.clear<ComputedClipPathsComponent>();

  createComputedComponents(warningSink);

  registry_.clear<DirtyFlagsComponent>();
  renderState.needsFullRebuild = false;
  renderState.needsFullStyleRecompute = false;
  renderState.hasBeenBuilt = true;
}

bool RenderingContext::hitTestEntity(Entity entity, const Vector2d& point) {
  const auto* instance = registry_.try_get<RenderingInstanceComponent>(entity);
  if (!instance) {
    return false;
  }

  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  const ComputedStyleComponent& style =
      StyleSystem().computeStyle(EntityHandle(registry_, entity), disabledSink);
  const PointerEvents pointerEvents = style.properties->pointerEvents.getRequired();

  if (pointerEvents == PointerEvents::None) {
    return false;
  }

  const HitTestConfig config = configFromPointerEvents(pointerEvents);

  // Check visibility requirement.
  if (config.requireVisible && !instance->visible) {
    return false;
  }

  const bool hasFillPaint = style.properties->fill.getRequired() != PaintServer::None();
  const bool hasStrokePaint = style.properties->stroke.getRequired() != PaintServer::None();
  const double strokeWidth =
      hasStrokePaint ? style.properties->strokeWidth.getRequired().value : 0.0;

  if (const auto bounds = ShapeSystem().getShapeWorldBounds(EntityHandle(registry_, entity));
      bounds && bounds->inflatedBy(strokeWidth).contains(point)) {
    if (pointerEvents == PointerEvents::BoundingBox) {
      return true;
    }

    const Vector2d pointInLocal = LayoutSystem()
                                      .getEntityFromWorldTransform(EntityHandle(registry_, entity))
                                      .inverse()
                                      .transformPosition(point);

    // Test fill intersection.
    if (config.testFill) {
      const bool skipBecauseNotPainted = config.requirePaintedFill && !hasFillPaint;
      if (!skipBecauseNotPainted &&
          ShapeSystem().pathFillIntersects(EntityHandle(registry_, entity), pointInLocal,
                                           style.properties->fillRule.getRequired())) {
        return true;
      }
    }

    // Test stroke intersection.
    if (config.testStroke) {
      const bool skipBecauseNotPainted = config.requirePaintedStroke && !hasStrokePaint;
      if (!skipBecauseNotPainted &&
          ShapeSystem().pathStrokeIntersects(EntityHandle(registry_, entity), pointInLocal,
                                             strokeWidth)) {
        return true;
      }
    }
  }

  return false;
}

Entity RenderingContext::findIntersecting(const Vector2d& point) {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  instantiateRenderTree(false, disabledSink);

  // Brute-force reverse scan.
  auto view = registry_.view<RenderingInstanceComponent>();
  for (auto it = view.rbegin(); it != view.rend(); ++it) {
    if (hitTestEntity(*it, point)) {
      return *it;
    }
  }

  return entt::null;
}

std::vector<Entity> RenderingContext::findAllIntersecting(const Vector2d& point) {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  instantiateRenderTree(false, disabledSink);

  std::vector<Entity> results;

  auto view = registry_.view<RenderingInstanceComponent>();
  for (auto it = view.rbegin(); it != view.rend(); ++it) {
    if (hitTestEntity(*it, point)) {
      results.push_back(*it);
    }
  }

  return results;
}

std::vector<Entity> RenderingContext::findIntersectingRect(const Box2d& rect) {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  instantiateRenderTree(false, disabledSink);

  std::vector<Entity> results;

  // Brute-force scan in reverse draw order.
  auto view = registry_.view<RenderingInstanceComponent>();
  for (auto it = view.rbegin(); it != view.rend(); ++it) {
    const auto& instance = view.get<RenderingInstanceComponent>(*it);
    if (instance.dataEntity == entt::null) {
      continue;
    }

    const auto bounds =
        ShapeSystem().getShapeWorldBounds(EntityHandle(registry_, instance.dataEntity));
    if (bounds && bounds->topLeft.x <= rect.bottomRight.x &&
        bounds->bottomRight.x >= rect.topLeft.x && bounds->topLeft.y <= rect.bottomRight.y &&
        bounds->bottomRight.y >= rect.topLeft.y) {
      results.push_back(*it);
    }
  }

  return results;
}

std::optional<Box2d> RenderingContext::getWorldBounds(Entity entity) {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  instantiateRenderTree(false, disabledSink);

  const auto* instance = registry_.try_get<RenderingInstanceComponent>(entity);
  if (!instance || instance->dataEntity == entt::null) {
    return std::nullopt;
  }

  return ShapeSystem().getShapeWorldBounds(EntityHandle(registry_, instance->dataEntity));
}

void RenderingContext::invalidateRenderTree() {
  registry_.clear<RenderingInstanceComponent>();
  registry_.clear<ComputedClipPathsComponent>();
  auto& renderState = getRenderTreeState(registry_);
  renderState.needsFullRebuild = true;
  renderState.needsFullStyleRecompute = true;
}

// 1. Setup shadow trees
// 2. Evaluate and propagate styles
// 3. Instantiate shadow trees and propagate style information to them
// 4. Determine element sizes and layout
// 5. Compute transforms
// 6. Decompose shapes to paths
// 7. Resolve fill and stroke references (paints)
// 8. Resolve filter references
void RenderingContext::createComputedComponents(ParseWarningSink& warningSink) {
  // Evaluate conditional components which may create shadow trees.
  PaintSystem().createShadowTrees(registry_, warningSink);

  // Instantiate shadow trees.
  for (auto view = registry_.view<ShadowTreeComponent>(); auto entity : view) {
    auto [shadowTreeComponent] = view.get(entity);
    if (auto targetEntity = shadowTreeComponent.mainTargetEntity(registry_)) {
      auto& shadow = registry_.get_or_emplace<ComputedShadowTreeComponent>(entity);
      createShadowTreeSystem().populateInstance(
          EntityHandle(registry_, entity), shadow, ShadowBranchType::Main, targetEntity.value(),
          shadowTreeComponent.mainHref().value(), warningSink);

    } else if (shadowTreeComponent.mainHref()) {
      // Same-document resolution failed. Check if this is an external reference.
      const Reference ref(shadowTreeComponent.mainHref().value());
      if (ref.isExternal()) {
        // Load the external SVG document via ResourceManagerContext.
        auto& resourceManager = registry_.ctx().get<ResourceManagerContext>();
        const RcString docUrl(ref.documentUrl());
        auto subDoc = resourceManager.loadExternalSVG(docUrl, warningSink);
        if (subDoc) {
          registry_.emplace_or_replace<ExternalUseComponent>(entity, std::move(*subDoc),
                                                             RcString(ref.fragment()));
        }
      } else {
        ParseDiagnostic err;
        err.reason = std::string("Warning: Failed to resolve shadow tree target with href '") +
                     shadowTreeComponent.mainHref().value_or("") + "'";
        warningSink.add(std::move(err));
      }
    }
  }

  StyleSystem().computeAllStyles(registry_, warningSink);

  // After styles are computed, we can load fonts and other embedded resources.
  registry_.ctx().get<components::ResourceManagerContext>().loadResources(warningSink);

#ifdef DONNER_TEXT_ENABLED
  // Create shared font registry + text engine in the registry context.
  // This must happen after loadResources() so @font-face data is available.
  {
    auto& resourceManager = registry_.ctx().get<components::ResourceManagerContext>();
    auto& fontManager = registry_.ctx().emplace<FontManager>(registry_);
    for (const auto& face : resourceManager.fontFaces()) {
      fontManager.addFontFace(face);
    }
    registry_.ctx().emplace<TextEngine>(fontManager, registry_);
  }
#endif

  // Instantiate shadow trees for 'fill' and 'stroke' referencing a <pattern>. This needs to occur
  // after those styles are evaluated, and after which we need to compute the styles for that subset
  // of the tree.
  for (auto view = registry_.view<ComputedStyleComponent>(); auto entity : view) {
    auto [styleComponent] = view.get(entity);

    const auto& properties = styleComponent.properties.value();

    if (auto fill = properties.fill.get()) {
      InstantiatePaintShadowTree(registry_, entity, ShadowBranchType::OffscreenFill, fill.value(),
                                 warningSink);
    }

    if (auto stroke = properties.stroke.get()) {
      InstantiatePaintShadowTree(registry_, entity, ShadowBranchType::OffscreenStroke,
                                 stroke.value(), warningSink);
    }

    if (auto mask = properties.mask.get()) {
      InstantiateMaskShadowTree(registry_, entity, mask.value(), warningSink);
    }

    if (auto markerStart = properties.markerStart.get()) {
      InstantiateMarkerShadowTree(registry_, entity, ShadowBranchType::OffscreenMarkerStart,
                                  markerStart.value(), warningSink);
    }

    if (auto markerMid = properties.markerMid.get()) {
      InstantiateMarkerShadowTree(registry_, entity, ShadowBranchType::OffscreenMarkerMid,
                                  markerMid.value(), warningSink);
    }

    if (auto markerEnd = properties.markerEnd.get()) {
      InstantiateMarkerShadowTree(registry_, entity, ShadowBranchType::OffscreenMarkerEnd,
                                  markerEnd.value(), warningSink);
    }
  }

  for (auto view = registry_.view<OffscreenShadowTreeComponent>(); auto entity : view) {
    auto [offscreenTree] = view.get(entity);
    for (auto [branchType, ref] : offscreenTree.branches()) {
      if (auto targetEntity = offscreenTree.branchTargetEntity(registry_, branchType)) {
        auto& computedShadow = registry_.get_or_emplace<ComputedShadowTreeComponent>(entity);

        const std::optional<size_t> maybeInstanceIndex = createShadowTreeSystem().populateInstance(
            EntityHandle(registry_, entity), computedShadow, branchType, targetEntity.value(),
            ref.href, warningSink);

        if (maybeInstanceIndex) {
          // Apply styles to the tree.
          const std::span<const Entity> shadowEntities =
              computedShadow.offscreenShadowEntities(maybeInstanceIndex.value());
          StyleSystem().computeStylesFor(registry_, shadowEntities, warningSink);
        }
      } else {
        // We had a href but it failed to resolve.
        ParseDiagnostic err;
        err.reason =
            std::string("Warning: Failed to resolve offscreen shadow tree target with href '") +
            ref.href + "'";
        warningSink.add(std::move(err));
      }
    }
  }

  // Instantiate nested marker shadow trees for entities inside shadow trees.
  // The first pass (above) only checks entities with pre-existing ComputedStyleComponent.
  // Shadow tree entities get their styles computed after populateInstance, so we need a second
  // pass to create marker shadow trees for those newly-styled entities (e.g., paths inside
  // markers that have their own marker-start/mid/end properties for nested markers).
  {
    // Collect shadow entities that need marker shadow trees (can't modify registry while
    // iterating views).
    struct MarkerWork {
      Entity shadowEntity;
      ShadowBranchType branchType;
      Reference reference;
    };
    std::vector<MarkerWork> nestedMarkerWork;

    for (auto view = registry_.view<ComputedShadowTreeComponent>(); auto entity : view) {
      const auto& computedShadow = view.get<ComputedShadowTreeComponent>(entity);
      const auto* offscreen = registry_.try_get<OffscreenShadowTreeComponent>(entity);
      if (!offscreen) {
        continue;
      }
      for (auto [branchType, ref] : offscreen->branches()) {
        const std::optional<size_t> instanceIndex = computedShadow.findOffscreenShadow(branchType);
        if (!instanceIndex) {
          continue;
        }
        for (const Entity shadowEntity : computedShadow.offscreenShadowEntities(*instanceIndex)) {
          const auto* shadowStyle = registry_.try_get<ComputedStyleComponent>(shadowEntity);
          if (!shadowStyle || !shadowStyle->properties) {
            continue;
          }
          const auto& props = shadowStyle->properties.value();
          if (auto m = props.markerStart.get()) {
            nestedMarkerWork.push_back(
                {shadowEntity, ShadowBranchType::OffscreenMarkerStart, m.value()});
          }
          if (auto m = props.markerMid.get()) {
            nestedMarkerWork.push_back(
                {shadowEntity, ShadowBranchType::OffscreenMarkerMid, m.value()});
          }
          if (auto m = props.markerEnd.get()) {
            nestedMarkerWork.push_back(
                {shadowEntity, ShadowBranchType::OffscreenMarkerEnd, m.value()});
          }
        }
      }
    }

    for (const auto& work : nestedMarkerWork) {
      InstantiateMarkerShadowTree(registry_, work.shadowEntity, work.branchType, work.reference,
                                  warningSink);
    }

    // Populate and style the newly-created nested marker shadow trees.
    for (const auto& work : nestedMarkerWork) {
      auto* offscreen = registry_.try_get<OffscreenShadowTreeComponent>(work.shadowEntity);
      if (!offscreen) {
        continue;
      }
      auto targetEntity = offscreen->branchTargetEntity(registry_, work.branchType);
      if (!targetEntity) {
        continue;
      }
      auto& computedShadow =
          registry_.get_or_emplace<ComputedShadowTreeComponent>(work.shadowEntity);
      if (computedShadow.findOffscreenShadow(work.branchType).has_value()) {
        continue;  // Already instantiated.
      }
      const std::optional<size_t> maybeInstanceIndex = createShadowTreeSystem().populateInstance(
          EntityHandle(registry_, work.shadowEntity), computedShadow, work.branchType,
          targetEntity.value(), work.reference.href, warningSink);
      if (maybeInstanceIndex) {
        const std::span<const Entity> shadowEntities =
            computedShadow.offscreenShadowEntities(maybeInstanceIndex.value());
        StyleSystem().computeStylesFor(registry_, shadowEntities, warningSink);
      }
    }
  }

  LayoutSystem().instantiateAllComputedComponents(registry_, warningSink);

  TextSystem().instantiateAllComputedComponents(registry_, warningSink);

  ShapeSystem().instantiateAllComputedPaths(registry_, warningSink);

  PaintSystem().instantiateAllComputedComponents(registry_, warningSink);

  FilterSystem().instantiateAllComputedComponents(registry_, warningSink);
}

std::optional<RenderingContext::FeImageSubtreeResult> RenderingContext::createFeImageShadowTree(
    Entity hostEntity, Entity targetEntity, bool verbose) {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  auto& computedShadow = registry_.get_or_emplace<ComputedShadowTreeComponent>(hostEntity);
  const std::optional<size_t> maybeIndex = createShadowTreeSystem().populateInstance(
      EntityHandle(registry_, hostEntity), computedShadow, ShadowBranchType::OffscreenFeImage,
      targetEntity, RcString(), disabledSink);

  if (!maybeIndex) {
    return std::nullopt;
  }

  const std::span<const Entity> shadowEntities =
      computedShadow.offscreenShadowEntities(maybeIndex.value());
  StyleSystem().computeStylesFor(registry_, shadowEntities, disabledSink);

  const Entity shadowRoot = computedShadow.offscreenShadowRoot(maybeIndex.value());
  if (shadowRoot == entt::null) {
    return std::nullopt;
  }

  RenderingContextImpl impl(registry_, verbose);
  Entity lastEntity = entt::null;
  impl.traverseTree(shadowRoot, &lastEntity);

  registry_.sort<RenderingInstanceComponent>(
      [](const RenderingInstanceComponent& lhs, const RenderingInstanceComponent& rhs) {
        return lhs.drawOrder < rhs.drawOrder;
      });

  if (lastEntity == entt::null) {
    return std::nullopt;
  }

  return FeImageSubtreeResult{shadowRoot, lastEntity};
}

void RenderingContext::instantiateRenderTreeWithPrecomputedTree(bool verbose) {
  invalidateRenderTree();

  const Entity rootEntity = registry_.ctx().get<SVGDocumentContext>().rootEntity;

  // Build initial context paint servers if set.
  ContextPaintServers initialContext;
  if (initialContextFill_.has_value()) {
    initialContext.contextFill = *initialContextFill_;
  }
  if (initialContextStroke_.has_value()) {
    initialContext.contextStroke = *initialContextStroke_;
  }

  RenderingContextImpl impl(registry_, verbose, initialContext);
  impl.traverseTree(rootEntity);

  registry_.sort<RenderingInstanceComponent>(
      [](const RenderingInstanceComponent& lhs, const RenderingInstanceComponent& rhs) {
        return lhs.drawOrder < rhs.drawOrder;
      });
}

}  // namespace donner::svg::components
