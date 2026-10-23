#include "donner/svg/renderer/RenderingContext.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "donner/base/RcString.h"
#include "donner/base/parser/LengthParser.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/svg/components/ComputedClipPathsComponent.h"
#include "donner/svg/components/ConditionalProcessingComponent.h"
#include "donner/svg/components/DirtyFlagsComponent.h"
#include "donner/svg/components/ElementTypeComponent.h"
#include "donner/svg/components/FontMetricDependenciesComponent.h"
#include "donner/svg/components/FontPaintDependenciesComponent.h"
#include "donner/svg/components/RenderingBehaviorComponent.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/animation/AnimatedValuesComponent.h"
#include "donner/svg/components/animation/AnimationBaseValue.h"
#include "donner/svg/components/animation/AnimationSystem.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterSystem.h"
#include "donner/svg/components/layout/LayoutSystem.h"
#include "donner/svg/components/layout/SizedElementComponent.h"
#include "donner/svg/components/layout/SymbolComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
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
#include "donner/svg/components/text/TextRootComponent.h"
#include "donner/svg/components/text/TextSystem.h"
#include "donner/svg/core/CssTransform.h"
#include "donner/svg/graph/RecursionGuard.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/parser/TransformParser.h"

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
  /// The context element's resolved fill, substituted for `context-fill`.
  ResolvedPaintServer contextFill = PaintServer::None();
  /// The context element's resolved stroke, substituted for `context-stroke`.
  ResolvedPaintServer contextStroke = PaintServer::None();
  /// Maps the context element's local space to the world space of the tree it was instantiated in.
  Transform2d worldFromContextTransform;
  /// Bounding box of the context element in its own user space, for resolving `objectBoundingBox`
  /// paint units of inherited gradients/patterns. Empty when unknown or zero-size.
  Box2d contextBounds;
  /// True when this context was established by marker instantiation: consuming instances live in
  /// the marker's offscreen coordinate space, which is placed per path vertex at draw time, so
  /// paint remaps must be resolved by the renderer driver at draw time.
  bool resolveAtDrawTime = false;
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

/**
 * Returns true if the element type participates in `<switch>` child selection. Per the SVG spec,
 * `<switch>` evaluates conditional processing attributes only on its direct children that are
 * directly-rendered element types (`a`, `g`, `svg`, `switch`, `text`, `use`, `image`, and
 * graphical shape elements). Any other child (descriptive elements such as `title`/`desc`/
 * `metadata`, `defs`, `symbol`, and unknown elements) is ignored and does not consume the selection
 * slot.
 *
 * This is a positive whitelist of directly-rendered types, so non-rendered children are excluded
 * regardless of whether they are currently typed as \ref ElementType::Unknown or gain a dedicated
 * element type later (e.g. `title`/`desc`/`metadata`).
 */
bool IsSwitchProcessedElementType(ElementType type) {
  switch (type) {
    case ElementType::A:
    case ElementType::Circle:
    case ElementType::Ellipse:
    case ElementType::G:
    case ElementType::Image:
    case ElementType::Line:
    case ElementType::Path:
    case ElementType::Polygon:
    case ElementType::Polyline:
    case ElementType::Rect:
    case ElementType::SVG:
    case ElementType::Switch:
    case ElementType::Text:
    case ElementType::Use: return true;
    default: return false;
  }
}

/**
 * Selection score for one `<switch>` child. Ineligible children (wrong element type, failing
 * non-language conditionals, or unmatched `systemLanguage`) never render. Eligible children
 * either carry a language rank or match unconditionally at their document position.
 */
struct SwitchChildScore {
  bool eligible = false;
  bool hasLanguage = false;
  size_t languageRank = 0;
};

/**
 * Scores one `<switch>` child for selection against the user's preferred languages.
 *
 * @param registry Document registry holding the child entity.
 * @param child Direct child entity of the `<switch>` element (or its shadow instance).
 * @param userLanguages User's preferred languages, in priority order.
 * @return The child's selection score.
 */
SwitchChildScore ScoreSwitchChild(Registry& registry, Entity child,
                                  const std::vector<RcString>& userLanguages) {
  const auto* shadowEntityComponent = registry.try_get<ShadowEntityComponent>(child);
  const EntityHandle childDataHandle(
      registry, shadowEntityComponent ? shadowEntityComponent->lightEntity : child);

  const auto* typeComponent = childDataHandle.try_get<ElementTypeComponent>();
  if (!typeComponent || !IsSwitchProcessedElementType(typeComponent->type())) {
    return {};
  }

  const auto* conditional = childDataHandle.try_get<ConditionalProcessingComponent>();
  if (conditional && !NonLanguageConditionalProcessingPasses(*conditional)) {
    return {};
  }

  if (!conditional || !conditional->systemLanguage.has_value()) {
    return {.eligible = true, .hasLanguage = false, .languageRank = 0};
  }

  const std::optional<size_t> rank =
      SystemLanguageMatchRank(*conditional->systemLanguage, userLanguages);
  if (!rank.has_value()) {
    return {};
  }
  return {.eligible = true, .hasLanguage = true, .languageRank = *rank};
}

bool HasRenderableType(EntityHandle handle) {
  const auto* type = handle.try_get<ElementTypeComponent>();
  return type && type->type() != ElementType::Unknown;
}

struct ClipGeometry {
  EntityHandle dataEntity;
  EntityHandle textEntity;
  const ComputedPathComponent* path;
  const ComputedStyleComponent* style;
  bool isText;
};

std::optional<ClipGeometry> PrepareClipGeometry(EntityHandle entity, bool enforceVisibility) {
  const auto* shadowEntity = entity.try_get<ShadowEntityComponent>();
  const EntityHandle dataEntity =
      shadowEntity ? EntityHandle(*entity.registry(), shadowEntity->lightEntity) : entity;
  if (!HasRenderableType(dataEntity)) {
    return std::nullopt;
  }
  const auto* path = ShapeSystem::geometryHandle(entity).try_get<ComputedPathComponent>();
  const auto* style = entity.try_get<ComputedStyleComponent>();
#ifdef DONNER_TEXT_ENABLED
  const EntityHandle textEntity =
      entity.all_of<TextRootComponent, ComputedTextComponent>() ? entity : dataEntity;
  const bool isText = textEntity.all_of<TextRootComponent, ComputedTextComponent>();
#else
  const EntityHandle textEntity = dataEntity;
  const bool isText = false;
#endif
  if ((!path && !isText) || !style) {
    return std::nullopt;
  }
  const auto& properties = style->properties.value();
  if (enforceVisibility && (properties.visibility.get().value() != Visibility::Visible ||
                            properties.display.get().value() == Display::None)) {
    return std::nullopt;
  }
  return ClipGeometry{ShapeSystem::geometryHandle(entity), textEntity, path, style, isText};
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

    // Cache the user's preferred languages, used to evaluate the `systemLanguage` conditional
    // processing attribute (see ConditionalProcessingComponent). Defaults to {"en"} when unset.
    if (registry_.ctx().contains<SVGDocumentContext>()) {
      userLanguages_ = registry_.ctx().get<SVGDocumentContext>().userLanguages;
    }

    if (verbose_) {
      std::cout << "Canvas from document-world transform: " << canvasFromDocumentWorldTransform_
                << "\n";
    }
  }

  /**
   * @brief Whether this instance paints a group of text spans rather than one element.
   *
   * Two do: the instance of a `<text>` element, which renders as a unit because its descendants
   * carry \ref RenderingBehavior::NoTraverseChildren, and the instance of a span that declared
   * `clip-path`, `mask` or `filter`, which paints every span in its own subtree.
   *
   * A shadow entity never carries \ref TextRootComponent, so the test is on the data handle: a
   * `<use>` copy of a text element keeps its data on the light entity. `TextRootComponent` and
   * `ComputedTextComponent` mark the same entities, since the text system emplaces the computed
   * component only on a text root, and the draw side selects on the computed one.
   *
   * @param textSpanRoot Text root whose spans this instance paints, or `entt::null` when the
   *   instance is not a span instance.
   * @param dataHandle Handle supplying the instance's data components.
   * @return True when the instance's draw filters what it paints per span.
   */
  bool instancePaintsTextSpans(Entity textSpanRoot, EntityHandle dataHandle) const {
#ifdef DONNER_TEXT_ENABLED
    return textSpanRoot != entt::null || dataHandle.all_of<TextRootComponent>();
#else
    (void)textSpanRoot;
    (void)dataHandle;
    return false;
#endif
  }

  /**
   * @brief Whether an instance whose own style is not `visible` still has spans to paint.
   *
   * `visibility` is inherited but a descendant may set it back to `visible`, and an instance that
   * paints a group of spans would take such a descendant's glyphs with it when suppressed. Those
   * instances survive instead, and their draw drops the hidden spans per span through
   * \ref ClearUnpaintedSpanGlyphs. An instance that paints one element is simply hidden.
   *
   * The subtree scan is a superset of that per-span filter: it answers "some element below this one
   * is visible", not "some visible span below this one has glyphs", so a visible descendant that
   * contributes no glyph keeps the instance alive and its draw then paints nothing. Keeping an
   * empty layer is the safe direction; the exact span set is not resolved until draw time.
   *
   * An entity with no computed style of its own inherits its nearest styled ancestor's visibility,
   * which the walk already covers, so it needs no entry of its own. The walk reads the styles of
   * the copy being instantiated, which for a `<use>` copy are the shadow tree's, while the per-span
   * filter reads the light tree's spans; the two differ only when a selector matches one and not
   * the other.
   *
   * `visible` also gates hit-testing and the bounds of an instance range, so a text root kept alive
   * for a visible span is hit-testable and contributes bounds over the element's laid-out ink, the
   * same coarseness a visible element containing a hidden span already has.
   *
   * @param textSpanRoot Text root whose spans this instance paints, or `entt::null` when the
   *   instance is not a span instance.
   * @param styleEntity Entity supplying the instance's style, and the root of the span subtree.
   * @param dataHandle Handle supplying the instance's data components.
   * @return True when the instance must still be drawn.
   */
  bool hiddenInstancePaintsVisibleSpans(Entity textSpanRoot, Entity styleEntity,
                                        EntityHandle dataHandle) const {
    if (!instancePaintsTextSpans(textSpanRoot, dataHandle)) {
      return false;
    }

    bool anyVisible = false;
    donner::components::ForAllChildrenRecursivePruned(
        EntityHandle(registry_, styleEntity), [&anyVisible](EntityHandle handle) {
          const auto* style = handle.try_get<ComputedStyleComponent>();
          if (style != nullptr && style->properties.has_value() &&
              style->properties->visibility.get().value() == Visibility::Visible) {
            anyVisible = true;
            return false;
          }

          return true;
        });
    return anyVisible;
  }

  struct PreparedTraversalNode {
    Entity styleEntity = entt::null;
    bool traverseChildren = false;
    bool selectSwitchChild = false;
    int layerDepth = 0;
    std::optional<ContextPaintServers> savedContextPaintServers;
  };

  /// Instantiate one tree entity and retain only the state needed while its
  /// children are traversed. Keeping this large operation out of the recursive
  /// DFS bounds native/WASM call-frame usage on browsers with small stacks.
  // TODO(jwmcglynn): Since 'stroke' and 'fill' may reference the same tree, we need to create two
  // instances of it in the render tree.
  std::optional<PreparedTraversalNode> prepareTraversalNode(Entity treeEntity) {
    // Consumed by this entity alone. Offscreen subtrees (mask, pattern, and marker content) are
    // instantiated from inside this function through their own `traverseTree` call, and they are
    // not text spans of the element that pulled them in.
    const Entity textSpanRoot = std::exchange(currentTextSpanRoot_, entt::null);

    const auto* shadowEntityComponent = registry_.try_get<ShadowEntityComponent>(treeEntity);
    const Entity styleEntity = treeEntity;
    const EntityHandle dataHandle(
        registry_, shadowEntityComponent ? shadowEntityComponent->lightEntity : treeEntity);
    bool traverseChildren = true;
    std::optional<Box2d> clipRect;
    int layerDepth = 0;
    std::optional<ContextPaintServers> savedContextPaintServers;
    const EntityHandle geometryHandle =
        ShapeSystem::geometryHandle(EntityHandle(registry_, styleEntity));
    const bool isShape = geometryHandle.all_of<ComputedPathComponent>();

    if (!HasRenderableType(dataHandle)) {
      return std::nullopt;
    }

    // Conditional-processing attributes (requiredExtensions, systemLanguage) disable rendering of
    // the element and its entire subtree when they evaluate to false. Non-rendered elements
    // referenced by IRI (gradients, <clipPath>, <defs> content) are resolved elsewhere and are
    // intentionally not affected, matching resvg.
    if (const auto* conditional = dataHandle.try_get<ConditionalProcessingComponent>();
        conditional && !EvaluateConditionalProcessing(*conditional, userLanguages_)) {
      return std::nullopt;
    }

    // ShadowOnlyChildren elements (e.g., <mask>, <pattern>) don't render content in the light
    // tree - only when instantiated as shadow trees. Track this so we can skip mask/clippath
    // resolution on them (which would wastefully consume shadow tree entities needed by the
    // actual users of those masks).
    bool isShadowOnlyInLightTree = false;

    if (const auto* behavior = dataHandle.try_get<RenderingBehaviorComponent>()) {
      if (behavior->behavior == RenderingBehavior::Nonrenderable && !ignoreNonrenderable_) {
        return std::nullopt;
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

    if (properties.display.get().value() == Display::None) {
      return std::nullopt;
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
      return std::nullopt;
    }

    if (auto maybeClipRect = LayoutSystem().clipRect(EntityHandle(registry_, treeEntity))) {
      const Overflow overflow = styleComponent.properties->overflow.get().value();

      if (overflow != Overflow::Visible && overflow != Overflow::Auto) {
        ++layerDepth;
        clipRect = maybeClipRect;
      }
    }

    auto& instance = registry_.emplace<RenderingInstanceComponent>(styleEntity);
    instance.drawOrder = drawOrder_++;

    const auto& absoluteTransformComponent =
        LayoutSystem().getAbsoluteTransformComponent(EntityHandle(registry_, treeEntity));
    instance.worldFromEntityTransform =
        absoluteTransformComponent.worldFromEntity * (absoluteTransformComponent.worldIsCanvas
                                                          ? canvasFromDocumentWorldTransform_
                                                          : Transform2d());

    instance.clipRect = clipRect;
    instance.dataEntity = dataHandle.entity();
    instance.textSpanRoot = textSpanRoot;

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

    // No-copy access to the stored effect list (avoids copying the vector on
    // every recompute). An unset/initial `filter` has no stored value and means
    // "no effects", so fall back to an empty list.
    static const std::vector<FilterEffect> kNoFilterEffects;
    const std::vector<FilterEffect>* storedFilter = properties.filter.getStoredValue();
    const std::vector<FilterEffect>& filterEffects =
        storedFilter != nullptr ? *storedFilter : kNoFilterEffects;
    const bool hasFilterEffect = !filterEffects.empty();

    if (properties.visibility.get().value() != Visibility::Visible) {
      instance.visible = hiddenInstancePaintsVisibleSpans(textSpanRoot, styleEntity, dataHandle);
    }

    if (hasFilterEffect) {
      instance.resolvedFilter = resolveFilter(dataHandle, filterEffects);
    }

    if (!isShadowOnlyInLightTree) {
      if (properties.clipPath.get()) {
        if (auto resolved = resolveClipPath(dataHandle, properties.clipPath.get().value());
            resolved.valid()) {
          instance.clipPath = resolved;

          // Get the paths and store them in a ComputedClipPaths component.
          auto& clipPaths = registry_.emplace<ComputedClipPathsComponent>(styleEntity);

          RecursionGuard guard;
          guard.add(styleEntity);
#ifdef DONNER_TEXT_ENABLED
          std::optional<FontManager::DependencyCapture> capture;
          auto* fontManager = registry_.ctx().find<FontManager>();
          if (fontManager) {
            capture.emplace(*fontManager, clipPaths.fontDependencies);
          }
#endif
          collectClipPaths(resolved.reference.handle, clipPaths.clipPaths, guard);
#ifdef DONNER_TEXT_ENABLED
          if (fontManager) {
            clipPaths.fontResourceRevision = fontManager->fontResourceRevision();
          }
#endif
        }
      }

      if (properties.mask.get()) {
        if (auto resolved =
                resolveMask(EntityHandle(registry_, styleEntity), properties.mask.get().value());
            resolved.valid()) {
          instance.mask = std::move(resolved);
        }
      }
    }

    // Fill and stroke are resolved before markers so marker content can inherit them through
    // `context-fill` / `context-stroke` (SVG2): the shape referencing the marker is the context
    // element for the marker's content.
    const ShadowTreeComponent* shadowTree = dataHandle.try_get<ShadowTreeComponent>();
    const bool setsContextColors = shadowTree && shadowTree->setsContextColors;

    if (setsContextColors || (instance.visible && (geometryHandle.all_of<ComputedPathComponent>() ||
                                                   dataHandle.all_of<ComputedTextComponent>() ||
                                                   instance.textSpanRoot != entt::null))) {
      if (auto fill = properties.fill.get()) {
        instance.resolvedFill =
            resolvePaint(ShadowBranchType::OffscreenFill, dataHandle, fill.value(),
                         instance.worldFromEntityTransform, styleComponent.externalFill);
      }

      if (auto stroke = properties.stroke.get()) {
        instance.resolvedStroke =
            resolvePaint(ShadowBranchType::OffscreenStroke, dataHandle, stroke.value(),
                         instance.worldFromEntityTransform, styleComponent.externalStroke);
      }

      // Save the current context paint servers if this is a shadow tree host.
      if (setsContextColors) {
        savedContextPaintServers = contextPaintServers_;
        contextPaintServers_.contextFill = instance.resolvedFill;
        contextPaintServers_.contextStroke = instance.resolvedStroke;
        contextPaintServers_.worldFromContextTransform = instance.worldFromEntityTransform;
        contextPaintServers_.contextBounds = computeSubtreeContextBounds(treeEntity);
        if (verbose_) {
          std::cout << "Context bounds for " << treeEntity << ": "
                    << contextPaintServers_.contextBounds << "\n";
        }
        contextPaintServers_.resolveAtDrawTime = false;
      }
    }

    if (isShape) {
      // The shape is the context element for its markers' content: expose the shape's resolved
      // paints (and bounding box) while the marker subtrees are instantiated.
      const ContextPaintServers savedForMarkers = contextPaintServers_;
      contextPaintServers_.contextFill = instance.resolvedFill;
      contextPaintServers_.contextStroke = instance.resolvedStroke;
      contextPaintServers_.worldFromContextTransform = instance.worldFromEntityTransform;
      contextPaintServers_.contextBounds =
          geometryHandle.get<ComputedPathComponent>().localBounds();
      contextPaintServers_.resolveAtDrawTime = true;

      const bool ownsMarkerResolutionSession = activeMarkerStack_.empty();
      if (ownsMarkerResolutionSession) {
        cyclicMarkerReferences_.clear();
      }

      if (properties.markerStart.get()) {
        if (auto resolved = resolveMarker(dataHandle, EntityHandle(registry_, styleEntity),
                                          properties.markerStart.get().value(),
                                          ShadowBranchType::OffscreenMarkerStart);
            resolved.valid()) {
          instance.markerStart = resolved;
        }
      }

      if (properties.markerMid.get()) {
        if (auto resolved = resolveMarker(dataHandle, EntityHandle(registry_, styleEntity),
                                          properties.markerMid.get().value(),
                                          ShadowBranchType::OffscreenMarkerMid);
            resolved.valid()) {
          instance.markerMid = resolved;
        }
      }

      if (properties.markerEnd.get()) {
        if (auto resolved = resolveMarker(dataHandle, EntityHandle(registry_, styleEntity),
                                          properties.markerEnd.get().value(),
                                          ShadowBranchType::OffscreenMarkerEnd);
            resolved.valid()) {
          instance.markerEnd = resolved;
        }
      }

      if (ownsMarkerResolutionSession) {
        cyclicMarkerReferences_.clear();
      }

      contextPaintServers_ = savedForMarkers;
    }

    // Create a new layer if opacity is less than 1 or if there is an effect that requires an
    // isolated group.
    if (properties.opacity.get().value() < 1.0) {
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

    if (properties.mixBlendMode.get().value() != MixBlendMode::Normal ||
        properties.isolation.get().value() == Isolation::Isolate) {
      instance.isolatedLayer = true;
      ++layerDepth;
    }

    lastRenderedEntity_ = styleEntity;

    return PreparedTraversalNode{
        .styleEntity = styleEntity,
        .traverseChildren = traverseChildren,
        .selectSwitchChild = dataHandle.get<ElementTypeComponent>().type() == ElementType::Switch,
        .layerDepth = layerDepth,
        .savedContextPaintServers = std::move(savedContextPaintServers),
    };
  }

#ifdef DONNER_TEXT_ENABLED
  /// True when this text content element declares an effect that needs its own rendering layer.
  bool textSpanDeclaresEffect(Entity entity) const {
    const auto* style = registry_.try_get<ComputedStyleComponent>(entity);
    if (!style || !style->properties.has_value()) {
      return false;
    }

    const auto& properties = style->properties.value();
    const std::vector<FilterEffect>* filterEffects = properties.filter.getStoredValue();
    return properties.clipPath.get().has_value() || properties.mask.get().has_value() ||
           (filterEffects != nullptr && !filterEffects->empty());
  }

  /**
   * Give each outermost \ref xml_tspan or \ref xml_textPath that declares `clip-path`, `mask`, or
   * `filter` its own rendering instance, so the driver wraps that span's glyphs in the effect's
   * layer. A \ref xml_text element renders as a unit, so its descendants are never reached by the
   * ordinary traversal.
   *
   * Recursion stops at a span that gets an instance: an effect nested inside another span's effect
   * would need nested layers, which the span instance does not provide yet.
   *
   * @param textRootEntity The root \ref xml_text entity being instantiated.
   */
  void traverseTextSpanEffects(Entity textRootEntity) {
    if (!registry_.all_of<TextRootComponent>(textRootEntity) ||
        !registry_.all_of<RenderingInstanceComponent>(textRootEntity)) {
      return;
    }

    const auto visitChildren = [this, textRootEntity](Entity parent, auto& self) -> void {
      for (Entity cur = registry_.get<donner::components::TreeComponent>(parent).firstChild();
           cur != entt::null;
           cur = registry_.get<donner::components::TreeComponent>(cur).nextSibling()) {
        if (textSpanDeclaresEffect(cur)) {
          currentTextSpanRoot_ = textRootEntity;
          traverseTree(cur);
          currentTextSpanRoot_ = entt::null;
        } else {
          self(cur, self);
        }
      }
    };
    visitChildren(textRootEntity, visitChildren);
  }
#endif

  /**
   * Traverse a tree, instantiating each entity in the tree.
   *
   * @param treeEntity Current entity in the tree or shadow tree.
   * @param lastRenderedEntityIfSubtree Optional, entity of the last rendered element if this is a
   *   subtree.
   */
  void traverseTree(Entity treeEntity, Entity* lastRenderedEntityIfSubtree = nullptr) {
    std::optional<PreparedTraversalNode> prepared = prepareTraversalNode(treeEntity);
    if (!prepared.has_value()) {
      return;
    }

#ifdef DONNER_TEXT_ENABLED
    traverseTextSpanEffects(treeEntity);
#endif

    if (prepared->traverseChildren) {
      const auto& tree = registry_.get<donner::components::TreeComponent>(treeEntity);
      if (prepared->selectSwitchChild) {
        // <switch> ranks language matches up to the first unconditional child.
        if (const Entity selectedChild = selectSwitchChild(tree); selectedChild != entt::null) {
          traverseTree(selectedChild);
        }
      } else {
        for (auto cur = tree.firstChild(); cur != entt::null;
             cur = registry_.get<donner::components::TreeComponent>(cur).nextSibling()) {
          traverseTree(cur);
        }
      }
    }

    registry_.get<RenderingInstanceComponent>(prepared->styleEntity).subtreeLastEntity =
        lastRenderedEntity_;

    if (prepared->layerDepth > 0) {
      registry_.get<RenderingInstanceComponent>(prepared->styleEntity).subtreeInfo =
          SubtreeInfo{prepared->styleEntity, lastRenderedEntity_, prepared->layerDepth};
    }

    if (prepared->savedContextPaintServers) {
      contextPaintServers_ = std::move(prepared->savedContextPaintServers).value();
    }

    if (lastRenderedEntityIfSubtree) {
      *lastRenderedEntityIfSubtree = lastRenderedEntity_;
    }
  }

  /**
   * Select the direct child of a \ref xml_switch to render. Language-conditioned children before
   * the first unconditional child are ranked by user preference, with document order breaking
   * ties. An unconditional child matches at its document position, so later children cannot win.
   *
   * Non-element children (comments, text) and children that are not directly-rendered element
   * types (descriptive elements, `defs`, `symbol`, unknown elements) are never selected.
   * Children failing non-language conditionals (`requiredExtensions`) are ineligible regardless
   * of language. `display` does not participate in selection, so a selected child with
   * `display: none` still wins and renders nothing.
   *
   * @param switchTree Tree component of the `<switch>` element (or its shadow instance).
   * @return The selected child entity, or `entt::null` if no child matches.
   */
  Entity selectSwitchChild(const donner::components::TreeComponent& switchTree) const {
    Entity bestChild = entt::null;
    std::optional<size_t> bestRank;

    for (Entity cur = switchTree.firstChild(); cur != entt::null;
         cur = registry_.get<donner::components::TreeComponent>(cur).nextSibling()) {
      const SwitchChildScore score = ScoreSwitchChild(registry_, cur, userLanguages_);
      if (!score.eligible) {
        continue;
      }

      if (!score.hasLanguage) {
        return bestChild != entt::null ? bestChild : cur;
      }

      if (!bestRank.has_value() || score.languageRank < *bestRank) {
        bestChild = cur;
        bestRank = score.languageRank;
      }
    }

    return bestChild;
  }

  struct ClipPathCollection {
    std::vector<ComputedClipPathsComponent::ClipPath>& paths;
    RecursionGuard guard;
    Transform2d rootClipFromReference;
    int layer;
    bool hasAnyChildren = false;
    bool valid = true;
  };

  static bool isFiniteClipTransform(const Transform2d& clipFromEntity) {
    for (double value : clipFromEntity.data) {
      if (!std::isfinite(value)) {
        return false;
      }
    }
    return true;
  }

  static bool isFiniteNonemptyClipBox(const Box2d& box) {
    return std::isfinite(box.topLeft.x) && std::isfinite(box.topLeft.y) &&
           std::isfinite(box.width()) && std::isfinite(box.height()) && box.width() > 0.0 &&
           box.height() > 0.0;
  }

  static std::optional<Transform2d> nestedClipReferenceTransform(
      ClipPathUnits units, const ComputedPathComponent* clipPathData,
      const Transform2d& rootClipFromEntity) {
    if (units == ClipPathUnits::UserSpaceOnUse) {
      return isFiniteClipTransform(rootClipFromEntity) ? std::optional(rootClipFromEntity)
                                                       : std::nullopt;
    }

    // Container boxes are unmodeled here; retain their existing definition-space geometry.
    if (!clipPathData) {
      return Transform2d();
    }
    const Box2d box = clipPathData->localBounds();
    if (!isFiniteNonemptyClipBox(box)) {
      return std::nullopt;
    }
    const Transform2d entityFromNormalized =
        Transform2d::Scale(box.size()) * Transform2d::Translate(box.topLeft);
    const Transform2d rootClipFromNormalized = entityFromNormalized * rootClipFromEntity;
    return isFiniteClipTransform(rootClipFromNormalized) ? std::optional(rootClipFromNormalized)
                                                         : std::nullopt;
  }

  bool collectNestedClipForEntity(EntityHandle entity, const ComputedPathComponent* clipPathData,
                                  ClipPathCollection& collection) {
    const auto* computedStyle = entity.try_get<ComputedStyleComponent>();
    if (!computedStyle || !computedStyle->properties->clipPath.get()) {
      return true;
    }
    const ResolvedClipPath resolved =
        resolveClipPath(entity, computedStyle->properties->clipPath.get().value());
    if (!resolved.valid() || collection.guard.hasRecursion(resolved.reference.handle)) {
      return true;
    }

    const Transform2d referenceFromEntity =
        LayoutSystem().getAbsoluteTransformComponent(entity).worldFromEntity;
    const Transform2d rootClipFromEntity = referenceFromEntity * collection.rootClipFromReference;
    const auto rootClipFromNestedReference =
        nestedClipReferenceTransform(resolved.units, clipPathData, rootClipFromEntity);
    return rootClipFromNestedReference &&
           collectClipPathsInReference(resolved.reference.handle, collection.paths,
                                       collection.guard.with(resolved.reference.handle),
                                       collection.layer + 1, *rootClipFromNestedReference);
  }

  static bool clipChildIsVisible(const PropertyRegistry& style, bool enforceVisibility) {
    return !enforceVisibility || (style.visibility.get().value() == Visibility::Visible &&
                                  style.display.get().value() != Display::None);
  }

  void recordClipFontDependencies(EntityHandle dataEntity) {
#ifdef DONNER_TEXT_ENABLED
    if (const auto* dependencies = dataEntity.try_get<FontMetricDependenciesComponent>()) {
      if (auto* manager = registry_.ctx().find<FontManager>()) {
        manager->recordDependencies(dependencies->fontDependencies);
      }
    }
#else
    (void)dataEntity;
#endif
  }

  void appendClipGeometry(const ComputedPathComponent* clipPathData, EntityHandle dataEntity,
                          EntityHandle textEntity, EntityHandle instanceEntity,
                          const Transform2d& rootClipFromEntity, ClipRule clipRule,
                          ClipPathCollection& collection) {
    if (!isFiniteClipTransform(rootClipFromEntity)) {
      collection.valid = false;
      return;
    }
    if (clipPathData) {
      recordClipFontDependencies(dataEntity);
      if (instanceEntity.entity() != dataEntity.entity()) {
        recordClipFontDependencies(instanceEntity);
      }
      collection.paths.emplace_back(clipPathData->spline, rootClipFromEntity, clipRule,
                                    collection.layer);
    }
#ifdef DONNER_TEXT_ENABLED
    else if (auto* textEngine = registry_.ctx().find<TextEngine>()) {
      for (const Path& glyphPath : textEngine->computedGlyphPaths(textEntity)) {
        if (!glyphPath.empty()) {
          collection.paths.emplace_back(glyphPath, rootClipFromEntity, clipRule, collection.layer);
        }
      }
    }
#else
    (void)textEntity;
#endif
  }

  void appendClipPathFromEntity(EntityHandle entity, bool enforceVisibility,
                                ClipPathCollection& collection) {
    if (!collection.valid) {
      return;
    }
    const auto geometry = PrepareClipGeometry(entity, enforceVisibility);
    if (!geometry) {
      return;
    }
    const auto& [dataEntity, textEntity, clipPathData, computedStyle, isTextClipChild] = *geometry;
    const auto& style = computedStyle->properties.value();
    // Keep text glyphs separate for evenodd; reject nested clips until layers preserve groups.
    if (isTextClipChild && style.clipPath.get()) {
      collection.valid = false;
      return;
    }
    if (!collectNestedClipForEntity(entity, clipPathData, collection)) {
      collection.valid = false;
      return;
    }
    collection.hasAnyChildren = true;
    const Transform2d referenceFromEntity =
        LayoutSystem().getAbsoluteTransformComponent(entity).worldFromEntity;
    const Transform2d rootClipFromEntity = referenceFromEntity * collection.rootClipFromReference;
    appendClipGeometry(clipPathData, dataEntity, textEntity, entity, rootClipFromEntity,
                       style.clipRule.get().value_or(ClipRule::NonZero), collection);
  }

  void appendClipChildGeometryAndUse(EntityHandle child, ClipPathCollection& collection) {
    appendClipPathFromEntity(child, true, collection);
    const auto* type = child.try_get<ElementTypeComponent>();
    if (!type || type->type() != ElementType::Use) {
      return;
    }
    const auto* useStyle = child.try_get<ComputedStyleComponent>();
    if (!useStyle || !clipChildIsVisible(useStyle->properties.value(), true)) {
      return;
    }
    const auto* shadow = child.try_get<ComputedShadowTreeComponent>();
    if (shadow && shadow->mainBranch) {
      const Entity shadowRoot = shadow->mainBranch->shadowRoot();
      if (shadowRoot != entt::null) {
        appendClipPathFromEntity(EntityHandle(registry_, shadowRoot), false, collection);
      }
    }
  }

  void appendClipChildAndUse(EntityHandle child, ClipPathCollection& collection) {
    if (!collection.valid) {
      return;
    }
    const std::size_t pathCount = collection.paths.size();
    const bool hadChildren = collection.hasAnyChildren;
    appendClipChildGeometryAndUse(child, collection);
    if (!collection.valid) {
      collection.paths.erase(collection.paths.begin() + pathCount, collection.paths.end());
      collection.hasAnyChildren = hadChildren;
      collection.valid = true;
    }
  }

  bool collectClipPathsInReference(EntityHandle clipPathHandle,
                                   std::vector<ComputedClipPathsComponent::ClipPath>& clipPaths,
                                   RecursionGuard guard, int layer,
                                   const Transform2d& rootClipFromReference) {
    const std::size_t pathCount = clipPaths.size();
    ClipPathCollection collection{clipPaths, std::move(guard), rootClipFromReference, layer};
    collection.valid = collectNestedClipForEntity(clipPathHandle, nullptr, collection);
    const std::size_t childBegin = clipPaths.size();
    const bool hasGroupClip = childBegin != pathCount;
    if (hasGroupClip) {
      collection.layer = layer + 1;
    }
    donner::components::ForAllChildren(
        clipPathHandle, [&](EntityHandle child) { appendClipChildAndUse(child, collection); });
    const bool valid = collection.valid && collection.hasAnyChildren;
    if (!valid) {
      clipPaths.erase(clipPaths.begin() + pathCount, clipPaths.end());
    } else if (hasGroupClip) {
      clipPaths.emplace_back(Path{}, Transform2d{}, ClipRule::NonZero, layer,
                             clipPaths.size() - childBegin);
    }
    return valid;
  }

  bool collectClipPaths(EntityHandle clipPathHandle,
                        std::vector<ComputedClipPathsComponent::ClipPath>& clipPaths,
                        RecursionGuard guard, int layer = 0) {
    return collectClipPathsInReference(clipPathHandle, clipPaths, std::move(guard), layer,
                                       Transform2d());
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
      if (const auto it = offscreenSubtreeLastEntity_.find(firstEntity);
          it != offscreenSubtreeLastEntity_.end()) {
        return SubtreeInfo{firstEntity, it->second, 0};
      }

      const auto& rootInst = registry_.get<RenderingInstanceComponent>(firstEntity);
      if (rootInst.subtreeInfo) {
        return SubtreeInfo{firstEntity, rootInst.subtreeInfo->lastRenderedEntity, 0};
      }
      return SubtreeInfo{firstEntity, firstEntity, 0};
    }

    Entity lastEntity = entt::null;
    traverseTree(firstEntity, &lastEntity);

    if (lastEntity != entt::null) {
      offscreenSubtreeLastEntity_[firstEntity] = lastEntity;
      return SubtreeInfo{firstEntity, lastEntity, 0};
    } else {
      // This could happen if the subtree has no nodes.
      return std::nullopt;
    }
  }

  ResolvedPaintServer resolveExternalHostPaint(ShadowBranchType branchType) {
    const ResolvedPaintServer resolved =
        StyleSystem::externalHostPaint(registry_, branchType == ShadowBranchType::OffscreenStroke);
    if (const auto* reference = std::get_if<PaintResolvedReference>(&resolved);
        reference && reference->reference.handle.all_of<ComputedPatternComponent>()) {
      StyleSystem::rejectExternalHostResource(registry_);
      if (reference->fallback) {
        return PaintServer::Solid(*reference->fallback);
      }
      return PaintServer::None();
    }
    return resolved;
  }

  ResolvedPaintServer resolvePaint(ShadowBranchType branchType, EntityHandle dataHandle,
                                   const PaintServer& paint, const Transform2d& worldFromEntity,
                                   bool externalHost = false) {
    if (externalHost) {
      return resolveExternalHostPaint(branchType);
    }
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
                                      instantiateOffscreenSubtree(dataHandle, branchType),
                                      std::nullopt};
      } else if (ref.fallback) {
        return PaintServer::Solid(ref.fallback.value());
      } else {
        return PaintServer::None();
      }
    } else if (paint.is<PaintServer::ContextFill>()) {
      return resolveContextPaint(contextPaintServers_.contextFill, /*seededFromStroke=*/false,
                                 worldFromEntity);
    } else if (paint.is<PaintServer::ContextStroke>()) {
      return resolveContextPaint(contextPaintServers_.contextStroke, /*seededFromStroke=*/true,
                                 worldFromEntity);
    } else {
      return PaintServer::None();
    }
  }

  /**
   * Substitute a `context-fill` / `context-stroke` paint with the context element's resolved
   * paint. Solid colors and `none` pass through unchanged; gradient/pattern references are
   * augmented with a \ref PaintContextRemap so the renderer evaluates them in the context
   * element's space (objectBoundingBox units against the context bounding box, coordinates
   * remapped from the context element's user space into the consuming entity's space).
   *
   * @param storedPaint The context element's resolved paint.
   * @param seededFromStroke True if this paint came from the context element's stroke (used for
   *   draw-time resolution of marker contexts).
   * @param worldFromEntity The consuming entity's world transform, in the same tree-world as
   *   \ref ContextPaintServers::worldFromContextTransform.
   */
  ResolvedPaintServer resolveContextPaint(const ResolvedPaintServer& storedPaint,
                                          bool seededFromStroke,
                                          const Transform2d& worldFromEntity) {
    const auto* ref = std::get_if<PaintResolvedReference>(&storedPaint);
    if (!ref) {
      return storedPaint;
    }

    PaintResolvedReference augmented = *ref;
    if (augmented.contextRemap && augmented.contextRemap->resolveAtDrawTime) {
      // The stored paint is already tied to a draw-time marker context; the renderer driver
      // chains through intermediate hops at draw time, so keep the stored remap unchanged.
      return augmented;
    }

    if (contextPaintServers_.resolveAtDrawTime) {
      // Marker context: the consuming entity lives in the marker's offscreen space, which is
      // placed per path vertex at draw time - the driver computes the concrete transform then.
      // Preserve the original context bounds if the stored paint was itself inherited.
      const Box2d contextBounds = augmented.contextRemap ? augmented.contextRemap->contextBounds
                                                         : contextPaintServers_.contextBounds;
      augmented.contextRemap = PaintContextRemap{contextBounds, Transform2d(),
                                                 /*resolveAtDrawTime=*/true, seededFromStroke};
    } else {
      // Same-world (<use>) context: the transform is fully determined at instantiation time.
      const Transform2d entityFromContextTransform =
          contextPaintServers_.worldFromContextTransform * worldFromEntity.inverse();
      if (augmented.contextRemap) {
        // The stored paint was itself inherited from an outer context: keep the original context
        // bounds and extend the chain originalContext -> contextElement -> thisEntity.
        augmented.contextRemap->entityFromContextTransform =
            augmented.contextRemap->entityFromContextTransform * entityFromContextTransform;
      } else {
        augmented.contextRemap =
            PaintContextRemap{contextPaintServers_.contextBounds, entityFromContextTransform,
                              /*resolveAtDrawTime=*/false, /*seededFromStroke=*/false};
      }
    }

    return augmented;
  }

  /**
   * Compute the bounding box of a shadow-tree host's content in the host's local space, used as
   * the context bounding box for `context-fill` / `context-stroke` paints with
   * `objectBoundingBox` units.
   *
   * Accumulates bottom-up: each element's box is the union of its own tight path bounds and its
   * children's boxes mapped through the child transforms, taking an axis-aligned bounding box at
   * every level. This matches how resvg computes group bounding boxes (`Group::bounding_box`), so
   * rotated subtrees produce the same (level-wise AABB) context box as the reference renders.
   *
   * @param rootEntity Style/shadow entity of the shadow-tree host (e.g. the \ref xml_use element).
   */
  Box2d computeSubtreeContextBounds(Entity rootEntity) {
    return subtreeBoundsInLocalSpace(rootEntity).value_or(Box2d());
  }

  /**
   * Bounds of @p entity's own geometry plus its descendants, in the coordinate space established
   * by @p entity (i.e. the space its children live in). See \ref computeSubtreeContextBounds.
   */
  std::optional<Box2d> subtreeBoundsInLocalSpace(Entity entity) {
    const auto worldFromEntityOf = [&](Entity target) -> Transform2d {
      const auto& absoluteTransformComponent =
          LayoutSystem().getAbsoluteTransformComponent(EntityHandle(registry_, target));
      return absoluteTransformComponent.worldFromEntity * (absoluteTransformComponent.worldIsCanvas
                                                               ? canvasFromDocumentWorldTransform_
                                                               : Transform2d());
    };

    std::optional<Box2d> bounds;
    const auto addBox = [&bounds](const Box2d& box) {
      bounds = bounds ? Box2d::Union(*bounds, box) : box;
    };

    {
      const auto* shadowEntityComponent = registry_.try_get<ShadowEntityComponent>(entity);
      const EntityHandle dataHandle(
          registry_, shadowEntityComponent ? shadowEntityComponent->lightEntity : entity);
      if (const auto* path = ShapeSystem::geometryHandle(EntityHandle(registry_, entity))
                                 .try_get<ComputedPathComponent>();
          path && !path->spline.empty()) {
        addBox(path->localBounds());
      }
    }

    const Transform2d entityFromWorld = worldFromEntityOf(entity).inverse();
    const auto& tree = registry_.get<donner::components::TreeComponent>(entity);
    for (auto cur = tree.firstChild(); cur != entt::null;
         cur = registry_.get<donner::components::TreeComponent>(cur).nextSibling()) {
      if (const std::optional<Box2d> childBounds = subtreeBoundsInLocalSpace(cur)) {
        const Transform2d entityFromChild = worldFromEntityOf(cur) * entityFromWorld;
        addBox(entityFromChild.transformBox(*childBounds));
      }
    }

    return bounds;
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

  std::optional<ResolvedMarkerLengths> resolveMarkerLengths(EntityHandle dataHandle,
                                                            EntityHandle markerHandle) {
    Registry& registry = *dataHandle.registry();
    const Entity target = markerHandle.entity();
    const std::array<std::string_view, 4> names = {"markerWidth", "markerHeight", "refX", "refY"};
    if (std::none_of(names.begin(), names.end(), [&](std::string_view name) {
          return ContextualLengthFor(registry, target, name) != nullptr;
        })) {
      return std::nullopt;
    }
    LayoutSystem layout;
    const Box2d viewport = layout.getViewBox(dataHandle);
    const Box2d referenceViewport =
        layout.overridesViewBox(markerHandle) ? layout.getViewBox(markerHandle) : viewport;
    const auto& marker = markerHandle.get<MarkerComponent>();
    const std::array<Lengthd, 4> bases = {marker.markerWidth, marker.markerHeight, marker.refX,
                                          marker.refY};
    std::array<double, 4> resolved;
    for (std::size_t i = 0; i < names.size(); ++i) {
      AnimationValueContext context;
      context.viewBox = i < 2 ? viewport : referenceViewport;
      context.extent = i % 2 == 0 ? Lengthd::Extent::X : Lengthd::Extent::Y;
      const Lengthd length =
          ResolveContextualLength(registry, target, names[i], bases[i], context).value_or(bases[i]);
      resolved[i] = length.toPixels(context.viewBox, context.fontMetrics, context.extent);
    }
    return ResolvedMarkerLengths{resolved[0], resolved[1], resolved[2], resolved[3]};
  }

  ResolvedMarker resolveMarker(EntityHandle dataHandle, EntityHandle styleHandle,
                               const Reference& reference, ShadowBranchType branchType) {
    if (auto resolvedRef = reference.resolve(*styleHandle.registry());
        resolvedRef && IsValidMarker(resolvedRef->handle)) {
      const Entity markerElement = resolvedRef->handle.entity();
      if (!activeMarkerStack_.empty() &&
          cyclicMarkerReferences_.count({activeMarkerStack_.back(), markerElement})) {
        return ResolvedMarker{ResolvedReference{EntityHandle()}, std::nullopt,
                              MarkerUnits::Default};
      }

      if (activeMarkerElements_.count(markerElement)) {
        assert(!activeMarkerStack_.empty());
        cyclicMarkerReferences_.insert({activeMarkerStack_.back(), markerElement});
        return ResolvedMarker{ResolvedReference{EntityHandle()}, std::nullopt,
                              MarkerUnits::Default};
      }

      // When the style entity is a shadow entity (e.g. a shape instantiated through <use>), the
      // ComputedShadowTreeComponent holding the offscreen marker branches lives on the
      // corresponding light entity, not on the shadow entity itself.
      EntityHandle shadowTreeHost = styleHandle;
      if (const auto* shadowEntity = styleHandle.try_get<ShadowEntityComponent>()) {
        shadowTreeHost = EntityHandle(*styleHandle.registry(), shadowEntity->lightEntity);
      }

      if (const auto* computedShadow = shadowTreeHost.try_get<ComputedShadowTreeComponent>();
          computedShadow && computedShadow->findOffscreenShadow(branchType).has_value()) {
        activeMarkerElements_.insert(markerElement);
        activeMarkerStack_.push_back(markerElement);
        auto subtree = instantiateOffscreenSubtree(shadowTreeHost, branchType);
        activeMarkerStack_.pop_back();
        activeMarkerElements_.erase(markerElement);

        return ResolvedMarker{resolvedRef.value(), std::move(subtree),
                              resolvedRef->handle.get<MarkerComponent>().markerUnits,
                              resolveMarkerLengths(dataHandle, resolvedRef->handle)};
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

  int drawOrder_ = 0;  //!< The current draw order index.
  /// Root \ref xml_text entity for the one effect-carrying span about to be instantiated. Cleared
  /// by \ref prepareTraversalNode as soon as that span consumes it.
  Entity currentTextSpanRoot_ = entt::null;
  Entity lastRenderedEntity_ = entt::null;  //!< The last entity rendered.
  /// Holds the current paint servers for resolving the `context-fill` and `context-stroke` paint
  /// values.
  ContextPaintServers contextPaintServers_;

  /// Transform from the canvas to the SVG document root, for the current canvas scale.
  Transform2d canvasFromDocumentWorldTransform_;

  /// User's preferred languages for evaluating the `systemLanguage` conditional processing
  /// attribute. Defaults to {"en"} when the document context is unavailable.
  std::vector<RcString> userLanguages_ = {RcString("en")};

  /// Tracks mask elements currently being rendered to detect mutual recursion
  /// (e.g., mask1→mask2→mask1). When a mask reference resolves to an element already in this
  /// set, the cycle is broken by treating the mask attribute as "none".
  std::set<Entity> activeMaskElements_;

  /// Tracks marker elements currently being instantiated so recursive marker references stop
  /// after the first level instead of repeatedly expanding the same cycle.
  std::set<Entity> activeMarkerElements_;

  /// Marker expansion order, used to identify the exact reference edge that closes a cycle.
  std::vector<Entity> activeMarkerStack_;

  /// Cycle-closing marker reference edges, keyed by source and target marker. Decisions remain
  /// active while all markers on one host shape are resolved, so a reciprocal marker does not
  /// expand the same cycle again from the opposite endpoint.
  std::set<std::pair<Entity, Entity>> cyclicMarkerReferences_;

  /// The last rendered entity of each offscreen subtree instantiated by \ref
  /// instantiateOffscreenSubtree, keyed by the subtree's root entity. Used to rebuild a correct
  /// \ref SubtreeInfo when a later reference shares an already-instantiated subtree (the root's
  /// own \ref RenderingInstanceComponent::subtreeInfo only exists when the root pushes layers).
  std::map<Entity, Entity> offscreenSubtreeLastEntity_;
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
    case PointerEvents::Auto:
    case PointerEvents::VisiblePainted: return {true, true, true, true, true};
    case PointerEvents::None:
      return {false, false, false, false, false};  // Should be filtered before this call.
    case PointerEvents::BoundingBox:
      return {false, false, false, false, false};  // Handled separately.
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

bool IsDormantDefinitionUse(Registry& registry, Entity entity,
                            const ShadowTreeComponent& shadowTree) {
  const auto* hostType = registry.try_get<ElementTypeComponent>(entity);
  if (hostType == nullptr || hostType->type() != ElementType::Use) {
    return false;
  }
  const std::optional<RcString> href = shadowTree.mainHref();
  if (!href || Reference(*href).isExternal() || !shadowTree.mainTargetEntity(registry)) {
    return false;
  }
  for (Entity ancestor = registry.get<donner::components::TreeComponent>(entity).parent();
       ancestor != entt::null;
       ancestor = registry.get<donner::components::TreeComponent>(ancestor).parent()) {
    const auto* type = registry.try_get<ElementTypeComponent>(ancestor);
    if (type == nullptr) {
      return false;
    }
    if (type->type() == ElementType::Defs) {
      return true;
    }
    if (type->type() != ElementType::G) {
      return false;
    }
  }
  return false;
}

void InstantiateMainShadowTrees(Registry& registry, ParseWarningSink& warningSink) {
  for (auto view = registry.view<ShadowTreeComponent>(); auto entity : view) {
    auto [shadowTreeComponent] = view.get(entity);
    // A referencing host expands these local uses from their light tree when it is presented.
    if (IsDormantDefinitionUse(registry, entity, shadowTreeComponent)) {
      continue;
    }
    if (auto targetEntity = shadowTreeComponent.mainTargetEntity(registry)) {
      auto& shadow = registry.get_or_emplace<ComputedShadowTreeComponent>(entity);
      createShadowTreeSystem().populateInstance(
          EntityHandle(registry, entity), shadow, ShadowBranchType::Main, targetEntity.value(),
          shadowTreeComponent.mainHref().value(), warningSink);

      // populateInstance() just created these entities, each carrying an empty computed-style
      // placeholder that the passes below dereference unconditionally. No mutation hook can have
      // flagged them, so flag them here: the style pass is allowed to visit only dirty entities.
      if (shadow.mainBranch) {
        for (const Entity shadowEntity : shadow.mainBranch->shadowEntities) {
          registry.get_or_emplace<DirtyFlagsComponent>(shadowEntity)
              .mark(DirtyFlagsComponent::Style);
        }
      }
    } else if (shadowTreeComponent.mainHref()) {
      // Same-document resolution failed. Check if this is an external reference.
      const Reference ref(shadowTreeComponent.mainHref().value());
      if (ref.isExternal()) {
        // Load the external SVG document via ResourceManagerContext.
        auto& resourceManager = registry.ctx().get<ResourceManagerContext>();
        const RcString docUrl(ref.documentUrl());
        auto subDoc = resourceManager.loadExternalSVG(docUrl, warningSink);
        if (subDoc) {
          registry.emplace_or_replace<ExternalUseComponent>(entity, std::move(*subDoc),
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
}

}  // namespace

RenderingContext::RenderingContext(Registry& registry) : registry_(registry) {}

void RenderingContext::setInitialContextPaint(const ResolvedPaintServer& fill,
                                              const ResolvedPaintServer& stroke) {
  initialContextFill_ = fill;
  initialContextStroke_ = stroke;
  getRenderTreeState(registry_).needsFullRebuild = true;
}

void RenderingContext::restoreInitialPaintState(InitialPaintState state) {
  initialContextFill_ = std::move(state.first);
  initialContextStroke_ = std::move(state.second);
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

  renderState.needsFullRebuild = false;
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
  ShadowTreeSystem::beginRebuild(registry_);
  std::vector<Entity> shadowHosts;
  auto shadowView = registry_.view<ComputedShadowTreeComponent>();
  shadowHosts.reserve(shadowView.size());
  for (const Entity entity : shadowView) {
    shadowHosts.push_back(entity);
  }
  for (const Entity entity : shadowHosts) {
    if (!registry_.valid(entity)) {
      continue;
    }
    if (auto* shadow = registry_.try_get<ComputedShadowTreeComponent>(entity)) {
      createShadowTreeSystem().teardown(registry_, *shadow);
    }
  }
  registry_.clear<ComputedShadowTreeComponent>();
  registry_.clear<RenderingInstanceComponent>();
  registry_.clear<ComputedClipPathsComponent>();
  registry_.clear<FontPaintDependenciesComponent>();

  // Animated presentation attributes are written straight into the cached computed styles (see
  // applyAnimationOverrides above), which is only sound while every pass rebuilds those styles
  // from their unanimated base. Keep documents carrying animation overrides on the whole-tree
  // restyle; every other document may use the dirty-entity style pass.
  //
  // Entities that the shadow-tree instantiation below creates are handled there: it flags the
  // fresh clones dirty so the dirty-entity pass populates their placeholder styles.
  if (!registry_.view<AnimatedValuesComponent>().empty()) {
    renderState.needsFullStyleRecompute = true;
  }

  createComputedComponents(warningSink);

  registry_.clear<DirtyFlagsComponent>();
  renderState.needsFullRebuild = false;
  renderState.needsFullStyleRecompute = false;
  renderState.hasBeenBuilt = true;
}

bool RenderingContext::HitGeometry::contains(const Vector2d& point, const Path* path) const {
  const Vector2d localPoint = entityFromDocumentTransform.transformPosition(point);
  if (textBounds) {
    return textBounds->contains(localPoint);
  }
  if (!documentBounds.contains(point)) {
    return false;
  }
  if (boundsOnly) {
    return true;
  }
  if (path == nullptr) {
    return false;
  }
  return (testFill && path->isInside(localPoint, fillRule)) ||
         (testStroke && path->isOnPath(localPoint, strokeWidth));
}

namespace {
void ConfigureHitPaint(const ComputedStyleComponent& style, const HitTestConfig& config,
                       RenderingContext::HitGeometry& geometry) {
  const bool fillPainted = style.properties->fill.get().value() != PaintServer::None();
  const bool strokePainted = style.properties->stroke.get().value() != PaintServer::None();
  geometry.strokeWidth = strokePainted ? style.properties->strokeWidth.get().value().value : 0.0;
  geometry.testFill = config.testFill && (!config.requirePaintedFill || fillPainted);
  geometry.testStroke = config.testStroke && (!config.requirePaintedStroke || strokePainted);
  geometry.fillRule = style.properties->fillRule.get().value();
}

std::optional<Box2d> TextHitBounds(Registry& registry, Entity entity) {
#ifdef DONNER_TEXT_ENABLED
  if (registry.any_of<TextRootComponent>(entity) && registry.ctx().contains<TextEngine>()) {
    return registry.ctx().get<TextEngine>().computedInkBounds(EntityHandle(registry, entity));
  }
#endif
  return std::nullopt;
}
}  // namespace

Entity RenderingContext::EventTargetEntity(const Registry& registry, Entity entity) {
  // A `<use>` shadow root is attached under the element that instantiated it, while offscreen
  // roots are detached, so only `<use>` content walks out of its shadow tree.
  while (entity != entt::null && registry.all_of<ShadowEntityComponent>(entity)) {
    const auto* tree = registry.try_get<donner::components::TreeComponent>(entity);
    entity = tree != nullptr ? tree->parent() : Entity(entt::null);
  }
  return entity;
}

bool RenderingContext::prepareHitGeometry(Entity entity, HitGeometry& geometry, const Path*& path,
                                          HitTestMode mode) {
  path = nullptr;
  const auto* instance = registry_.try_get<RenderingInstanceComponent>(entity);
  if (instance == nullptr || EventTargetEntity(registry_, entity) == entt::null) {
    return false;
  }
  ParseWarningSink sink = ParseWarningSink::Disabled();
  const auto& style = StyleSystem().computeStyle(EntityHandle(registry_, entity), sink);
  const PointerEvents pointerEvents = mode == HitTestMode::EditorSelection
                                          ? PointerEvents::VisiblePainted
                                          : style.properties->pointerEvents.get().value();
  const HitTestConfig config = configFromPointerEvents(pointerEvents);
  if (pointerEvents == PointerEvents::None || (config.requireVisible && !instance->visible)) {
    return false;
  }
  ConfigureHitPaint(style, config, geometry);
  geometry.entityFromDocumentTransform =
      LayoutSystem().getEntityFromWorldTransform(EntityHandle(registry_, entity)).inverse();
  geometry.textBounds = TextHitBounds(registry_, entity);
  if (geometry.textBounds) {
    return !geometry.textBounds->isEmpty();
  }
  const auto bounds = ShapeSystem().getShapeWorldBounds(EntityHandle(registry_, entity));
  if (!bounds) {
    return false;
  }
  geometry.documentBounds = bounds->inflatedBy(geometry.strokeWidth);
  geometry.boundsOnly = pointerEvents == PointerEvents::BoundingBox;
  if (const auto* computed = ShapeSystem().createComputedPathIfShape(
          EntityHandle(registry_, entity), FontMetrics(), sink)) {
    path = &computed->spline;
  }
  return true;
}

bool RenderingContext::hitTestEntity(Entity entity, const Vector2d& point, HitTestMode mode) {
  HitGeometry geometry;
  const Path* path = nullptr;
  return prepareHitGeometry(entity, geometry, path, mode) && geometry.contains(point, path);
}

std::optional<std::vector<RenderingContext::HitRecord>> RenderingContext::captureHitGeometry(
    std::size_t maximumRecords, std::size_t maximumPathBytes) {
  ParseWarningSink sink = ParseWarningSink::Disabled();
  instantiateRenderTree(false, sink);
  maximumRecords = std::min<std::size_t>(maximumRecords, 65536);
  maximumPathBytes = std::min<std::size_t>(maximumPathBytes, 32 * 1024 * 1024);
  std::size_t visited = 0;
  std::size_t retainedBytes = 0;
  std::vector<HitRecord> records;
  auto view = registry_.view<RenderingInstanceComponent>();
  for (auto it = view.rbegin(); it != view.rend(); ++it) {
    if (++visited > maximumRecords) {
      return std::nullopt;
    }
    HitGeometry geometry;
    const Path* path = nullptr;
    if (!prepareHitGeometry(*it, geometry, path)) {
      continue;
    }
    const auto bytes = path != nullptr ? path->retainedBytes() : std::optional<std::size_t>(0);
    if (!bytes || *bytes > maximumPathBytes - retainedBytes) {
      return std::nullopt;
    }
    retainedBytes += *bytes;
    records.push_back(HitRecord{*it, geometry, path != nullptr ? *path : Path()});
  }
  return records;
}

Entity RenderingContext::findIntersecting(const Vector2d& point, HitTestMode mode) {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  instantiateRenderTree(false, disabledSink);

  // Brute-force reverse scan.
  auto view = registry_.view<RenderingInstanceComponent>();
  for (auto it = view.rbegin(); it != view.rend(); ++it) {
    if (hitTestEntity(*it, point, mode)) {
      return *it;
    }
  }

  return entt::null;
}

std::vector<Entity> RenderingContext::findAllIntersecting(const Vector2d& point, HitTestMode mode) {
  ParseWarningSink disabledSink = ParseWarningSink::Disabled();
  instantiateRenderTree(false, disabledSink);

  std::vector<Entity> results;

  auto view = registry_.view<RenderingInstanceComponent>();
  for (auto it = view.rbegin(); it != view.rend(); ++it) {
    if (hitTestEntity(*it, point, mode)) {
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

    const auto bounds = ShapeSystem().getShapeWorldBounds(instance.styleHandle(registry_));
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

  return ShapeSystem().getShapeWorldBounds(instance->styleHandle(registry_));
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
namespace {

void InstantiateStyledPaintTrees(Registry& registry, ParseWarningSink& warningSink) {
  for (auto view = registry.view<ComputedStyleComponent>(); auto entity : view) {
    auto [styleComponent] = view.get(entity);

    const auto& properties = styleComponent.properties.value();

    if (auto fill = StyleSystem::localPaintForInstantiation(styleComponent, false)) {
      InstantiatePaintShadowTree(registry, entity, ShadowBranchType::OffscreenFill, fill.value(),
                                 warningSink);
    }

    if (auto stroke = StyleSystem::localPaintForInstantiation(styleComponent, true)) {
      InstantiatePaintShadowTree(registry, entity, ShadowBranchType::OffscreenStroke,
                                 stroke.value(), warningSink);
    }

    if (auto mask = properties.mask.get()) {
      InstantiateMaskShadowTree(registry, entity, mask.value(), warningSink);
    }

    if (auto markerStart = properties.markerStart.get()) {
      InstantiateMarkerShadowTree(registry, entity, ShadowBranchType::OffscreenMarkerStart,
                                  markerStart.value(), warningSink);
    }

    if (auto markerMid = properties.markerMid.get()) {
      InstantiateMarkerShadowTree(registry, entity, ShadowBranchType::OffscreenMarkerMid,
                                  markerMid.value(), warningSink);
    }

    if (auto markerEnd = properties.markerEnd.get()) {
      InstantiateMarkerShadowTree(registry, entity, ShadowBranchType::OffscreenMarkerEnd,
                                  markerEnd.value(), warningSink);
    }
  }
}

void PopulateOffscreenPaintTrees(Registry& registry, ParseWarningSink& warningSink) {
  for (auto view = registry.view<OffscreenShadowTreeComponent>(); auto entity : view) {
    auto [offscreenTree] = view.get(entity);
    for (auto [branchType, ref] : offscreenTree.branches()) {
      if (auto targetEntity = offscreenTree.branchTargetEntity(registry, branchType)) {
        auto& computedShadow = registry.get_or_emplace<ComputedShadowTreeComponent>(entity);

        const std::optional<size_t> maybeInstanceIndex = createShadowTreeSystem().populateInstance(
            EntityHandle(registry, entity), computedShadow, branchType, targetEntity.value(),
            ref.href, warningSink);

        if (maybeInstanceIndex) {
          // Apply styles to the tree.
          const std::span<const Entity> shadowEntities =
              computedShadow.offscreenShadowEntities(maybeInstanceIndex.value());
          StyleSystem().computeStylesFor(registry, shadowEntities, warningSink);
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
}

void InstantiateNestedMarkerTrees(Registry& registry, ParseWarningSink& warningSink) {
  {
    // Collect shadow entities that need marker shadow trees (can't modify registry while
    // iterating views).
    struct MarkerWork {
      Entity shadowEntity;
      ShadowBranchType branchType;
      Reference reference;
    };
    std::vector<MarkerWork> nestedMarkerWork;

    for (auto view = registry.view<ComputedShadowTreeComponent>(); auto entity : view) {
      const auto& computedShadow = view.get<ComputedShadowTreeComponent>(entity);
      const auto* offscreen = registry.try_get<OffscreenShadowTreeComponent>(entity);
      if (!offscreen) {
        continue;
      }
      for (auto [branchType, ref] : offscreen->branches()) {
        const std::optional<size_t> instanceIndex = computedShadow.findOffscreenShadow(branchType);
        if (!instanceIndex) {
          continue;
        }
        for (const Entity shadowEntity : computedShadow.offscreenShadowEntities(*instanceIndex)) {
          const auto* shadowStyle = registry.try_get<ComputedStyleComponent>(shadowEntity);
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
      InstantiateMarkerShadowTree(registry, work.shadowEntity, work.branchType, work.reference,
                                  warningSink);
    }

    // Populate and style the newly-created nested marker shadow trees.
    for (const auto& work : nestedMarkerWork) {
      auto* offscreen = registry.try_get<OffscreenShadowTreeComponent>(work.shadowEntity);
      if (!offscreen) {
        continue;
      }
      auto targetEntity = offscreen->branchTargetEntity(registry, work.branchType);
      if (!targetEntity) {
        continue;
      }
      auto& computedShadow =
          registry.get_or_emplace<ComputedShadowTreeComponent>(work.shadowEntity);
      if (computedShadow.findOffscreenShadow(work.branchType).has_value()) {
        continue;  // Already instantiated.
      }
      const std::optional<size_t> maybeInstanceIndex = createShadowTreeSystem().populateInstance(
          EntityHandle(registry, work.shadowEntity), computedShadow, work.branchType,
          targetEntity.value(), work.reference.href, warningSink);
      if (maybeInstanceIndex) {
        const std::span<const Entity> shadowEntities =
            computedShadow.offscreenShadowEntities(maybeInstanceIndex.value());
        StyleSystem().computeStylesFor(registry, shadowEntities, warningSink);
      }
    }
  }
}

}  // namespace

void RenderingContext::createComputedComponents(ParseWarningSink& warningSink) {
  RestoreAnimationPresentation(registry_);

  if (!registry_.ctx().contains<AnimationStyleContext>()) {
    registry_.ctx().emplace<AnimationStyleContext>();
  }
  registry_.ctx().get<AnimationStyleContext>().enabled = false;
  StyleSystem().computeAllStyles(registry_, warningSink);

  // Advance SMIL animations and apply their value overrides on top of the computed styles/geometry,
  // before layout and shape computation.
  ApplyAnimationPresentation(registry_);
  // Expand references from this sample, sharing the original rebuild's resource limits.
  PaintSystem().createShadowTrees(registry_, warningSink);
  InstantiateMainShadowTrees(registry_, warningSink);
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

  InstantiateStyledPaintTrees(registry_, warningSink);
  PopulateOffscreenPaintTrees(registry_, warningSink);
  InstantiateNestedMarkerTrees(registry_, warningSink);

  LayoutSystem().instantiateAllComputedComponents(registry_, warningSink);

  // Shapes are computed before text so that a <textPath> referencing a basic shape, such as a
  // <rect>, finds that shape's equivalent path already attached.
  ShapeSystem().instantiateAllComputedPaths(registry_, warningSink);

  TextSystem().instantiateAllComputedComponents(registry_, warningSink);

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
  ShapeSystem().instantiateShadowComputedPaths(registry_, shadowEntities, disabledSink);

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
