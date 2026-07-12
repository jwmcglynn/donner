#include "donner/svg/renderer/RenderSnapshot.h"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/layout/TransformComponent.h"
#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/MaskComponent.h"
#include "donner/svg/components/paint/PatternComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"

#ifdef DONNER_TEXT_ENABLED
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"
#endif

namespace donner::svg {
namespace {

struct BeginFrameCommand {
  RenderViewport viewport;
};

struct EndFrameCommand {};

struct SetTransformCommand {
  Transform2d transform;
};

struct PushTransformCommand {
  Transform2d transform;
};

struct PopTransformCommand {};

struct PushClipCommand {
  ResolvedClip clip;
};

struct PopClipCommand {};

struct PushIsolatedLayerCommand {
  double opacity = 1.0;
  MixBlendMode blendMode = MixBlendMode::Normal;
};

struct PopIsolatedLayerCommand {};

struct PushFilterLayerCommand {
  components::FilterGraph filterGraph;
  std::optional<Box2d> filterRegion;
};

struct PopFilterLayerCommand {};

struct PushMaskCommand {
  std::optional<Box2d> maskBounds;
};

struct TransitionMaskToContentCommand {};

struct PopMaskCommand {};

struct BeginPatternTileCommand {
  Box2d tileRect;
  Transform2d targetFromPattern;
};

struct EndPatternTileCommand {
  bool forStroke = false;
};

struct SetPaintCommand {
  PaintParams paint;
};

struct DrawPathCommand {
  PathShape path;
  StrokeParams stroke;
};

struct DrawRectCommand {
  Box2d rect;
  StrokeParams stroke;
};

struct DrawEllipseCommand {
  Box2d bounds;
  StrokeParams stroke;
};

struct DrawImageCommand {
  ImageResource image;
  ImageParams params;
};

struct DrawTextCommand {
  components::ComputedTextComponent text;
  TextParams params;
  std::vector<css::FontFace> fontFaces;
};

using RenderCommand =
    std::variant<BeginFrameCommand, EndFrameCommand, SetTransformCommand, PushTransformCommand,
                 PopTransformCommand, PushClipCommand, PopClipCommand, PushIsolatedLayerCommand,
                 PopIsolatedLayerCommand, PushFilterLayerCommand, PopFilterLayerCommand,
                 PushMaskCommand, TransitionMaskToContentCommand, PopMaskCommand,
                 BeginPatternTileCommand, EndPatternTileCommand, SetPaintCommand, DrawPathCommand,
                 DrawRectCommand, DrawEllipseCommand, DrawImageCommand, DrawTextCommand>;

template <typename... Ts>
struct Overloaded : Ts... {
  using Ts::operator()...;
};

template <typename... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

struct SourceEntityKey {
  Registry* registry = nullptr;
  Entity entity = entt::null;

  bool operator==(const SourceEntityKey& other) const = default;
};

struct SourceEntityKeyHash {
  std::size_t operator()(const SourceEntityKey& key) const {
    const std::size_t registryHash = std::hash<Registry*>{}(key.registry);
    const std::size_t entityHash =
        std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(key.entity));
    return registryHash ^ (entityHash + 0x9e3779b9 + (registryHash << 6) + (registryHash >> 2));
  }
};

class SnapshotResourceMapper {
public:
  explicit SnapshotResourceMapper(Registry& registry) : registry_(registry) {}

  components::ResolvedPaintServer mapPaint(components::ResolvedPaintServer paint) {
    if (auto* ref = std::get_if<components::PaintResolvedReference>(&paint)) {
      *ref = mapPaintReference(*ref);
    }
    return paint;
  }

  PaintParams mapPaintParams(PaintParams paint) {
    paint.fill = mapPaint(std::move(paint.fill));
    paint.stroke = mapPaint(std::move(paint.stroke));
    return paint;
  }

  components::ComputedTextComponent mapComputedText(components::ComputedTextComponent text) {
    for (auto& span : text.spans) {
      span.sourceEntity = entt::null;
      span.textPathSourceEntity = entt::null;
      span.resolvedFill = mapPaint(std::move(span.resolvedFill));
      span.resolvedStroke = mapPaint(std::move(span.resolvedStroke));
      span.resolvedDecorationFill = mapPaint(std::move(span.resolvedDecorationFill));
      span.resolvedDecorationStroke = mapPaint(std::move(span.resolvedDecorationStroke));
    }
    return text;
  }

  ResolvedClip mapClip(ResolvedClip clip) {
    for (PathShape& path : clip.clipPaths) {
      path.sourceEntity = EntityHandle();
    }
    if (clip.mask.has_value()) {
      clip.mask = mapMask(*clip.mask);
    }
    return clip;
  }

  components::FilterGraph mapFilterGraph(components::FilterGraph filterGraph) {
    for (components::FilterNode& node : filterGraph.nodes) {
      if (auto* image = std::get_if<components::filter_primitive::Image>(&node.primitive)) {
        // Snapshot replay must not lazy-render live SVG documents or same-document fragments.
        // RendererDriver pre-renders those to imageData during capture when an offscreen backend
        // is available; if not, replay treats the primitive as an unavailable image.
        image->svgSubDocument.reset();
        image->fragmentId = RcString();
      }
    }
    return filterGraph;
  }

private:
  template <typename Component>
  void cloneIfPresent(EntityHandle source, Entity target) {
    if (const auto* component = source.try_get<Component>()) {
      registry_.emplace<Component>(target, *component);
    }
  }

  EntityHandle mapHandle(EntityHandle source) {
    if (!source.valid()) {
      return EntityHandle();
    }

    SourceEntityKey key{source.registry(), source.entity()};
    if (auto it = mappedEntities_.find(key); it != mappedEntities_.end()) {
      return EntityHandle(registry_, it->second);
    }

    const Entity target = registry_.create();
    mappedEntities_.emplace(key, target);

    cloneIfPresent<components::ComputedGradientComponent>(source, target);
    cloneIfPresent<components::ComputedLinearGradientComponent>(source, target);
    cloneIfPresent<components::ComputedRadialGradientComponent>(source, target);
    cloneIfPresent<components::ComputedLocalTransformComponent>(source, target);
    cloneIfPresent<components::ComputedPatternComponent>(source, target);
    cloneIfPresent<components::MaskComponent>(source, target);
    cloneIfPresent<components::ComputedFilterComponent>(source, target);

    return EntityHandle(registry_, target);
  }

  ResolvedReference mapReference(ResolvedReference reference) {
    reference.handle = mapHandle(reference.handle);
    return reference;
  }

  components::PaintResolvedReference mapPaintReference(
      components::PaintResolvedReference reference) {
    reference.reference = mapReference(reference.reference);
    reference.subtreeInfo.reset();
    return reference;
  }

  components::ResolvedMask mapMask(components::ResolvedMask mask) {
    mask.reference = mapReference(mask.reference);
    mask.subtreeInfo.reset();
    return mask;
  }

  Registry& registry_;
  std::unordered_map<SourceEntityKey, Entity, SourceEntityKeyHash> mappedEntities_;
};

std::size_t CountReferenceToRegistry(const EntityHandle& handle, const Registry& registry) {
  return handle.registry() == &registry ? 1u : 0u;
}

std::size_t CountReferenceToRegistry(const ResolvedReference& reference, const Registry& registry) {
  return CountReferenceToRegistry(reference.handle, registry);
}

std::size_t CountReferenceToRegistry(const components::ResolvedPaintServer& paint,
                                     const Registry& registry) {
  if (const auto* ref = std::get_if<components::PaintResolvedReference>(&paint)) {
    return CountReferenceToRegistry(ref->reference, registry);
  }
  return 0;
}

std::size_t CountReferenceToRegistry(const PaintParams& paint, const Registry& registry) {
  return CountReferenceToRegistry(paint.fill, registry) +
         CountReferenceToRegistry(paint.stroke, registry);
}

std::size_t CountReferenceToRegistry(const components::ResolvedMask& mask,
                                     const Registry& registry) {
  return CountReferenceToRegistry(mask.reference, registry);
}

std::size_t CountReferenceToRegistry(const ResolvedClip& clip, const Registry& registry) {
  std::size_t count = 0;
  for (const PathShape& path : clip.clipPaths) {
    count += CountReferenceToRegistry(path.sourceEntity, registry);
  }
  if (clip.mask.has_value()) {
    count += CountReferenceToRegistry(*clip.mask, registry);
  }
  return count;
}

std::size_t CountReferenceToRegistry(const components::ComputedTextComponent& text,
                                     const Registry& registry) {
  std::size_t count = 0;
  for (const auto& span : text.spans) {
    count += CountReferenceToRegistry(span.resolvedFill, registry);
    count += CountReferenceToRegistry(span.resolvedStroke, registry);
    count += CountReferenceToRegistry(span.resolvedDecorationFill, registry);
    count += CountReferenceToRegistry(span.resolvedDecorationStroke, registry);
  }
  return count;
}

std::size_t CountReferenceToRegistry(const RenderCommand& command, const Registry& registry) {
  return std::visit(Overloaded{
                        [&](const PushClipCommand& value) {
                          return CountReferenceToRegistry(value.clip, registry);
                        },
                        [&](const SetPaintCommand& value) {
                          return CountReferenceToRegistry(value.paint, registry);
                        },
                        [&](const DrawPathCommand& value) {
                          return CountReferenceToRegistry(value.path.sourceEntity, registry);
                        },
                        [&](const DrawTextCommand& value) {
                          return CountReferenceToRegistry(value.text, registry);
                        },
                        [](const auto&) -> std::size_t { return 0; },
                    },
                    command);
}

void ReplayCommand(const RenderCommand& command, RendererInterface& renderer,
                   Registry& textRegistry) {
  std::visit(
      Overloaded{
          [&](const BeginFrameCommand& value) { renderer.beginFrame(value.viewport); },
          [&](const EndFrameCommand&) { renderer.endFrame(); },
          [&](const SetTransformCommand& value) { renderer.setTransform(value.transform); },
          [&](const PushTransformCommand& value) { renderer.pushTransform(value.transform); },
          [&](const PopTransformCommand&) { renderer.popTransform(); },
          [&](const PushClipCommand& value) { renderer.pushClip(value.clip); },
          [&](const PopClipCommand&) { renderer.popClip(); },
          [&](const PushIsolatedLayerCommand& value) {
            renderer.pushIsolatedLayer(value.opacity, value.blendMode);
          },
          [&](const PopIsolatedLayerCommand&) { renderer.popIsolatedLayer(); },
          [&](const PushFilterLayerCommand& value) {
            renderer.pushFilterLayer(value.filterGraph, value.filterRegion);
          },
          [&](const PopFilterLayerCommand&) { renderer.popFilterLayer(); },
          [&](const PushMaskCommand& value) { renderer.pushMask(value.maskBounds); },
          [&](const TransitionMaskToContentCommand&) { renderer.transitionMaskToContent(); },
          [&](const PopMaskCommand&) { renderer.popMask(); },
          [&](const BeginPatternTileCommand& value) {
            (void)renderer.beginPatternTile(value.tileRect, value.targetFromPattern);
          },
          [&](const EndPatternTileCommand& value) { renderer.endPatternTile(value.forStroke); },
          [&](const SetPaintCommand& value) { renderer.setPaint(value.paint); },
          [&](const DrawPathCommand& value) { renderer.drawPath(value.path, value.stroke); },
          [&](const DrawRectCommand& value) { renderer.drawRect(value.rect, value.stroke); },
          [&](const DrawEllipseCommand& value) {
            renderer.drawEllipse(value.bounds, value.stroke);
          },
          [&](const DrawImageCommand& value) { renderer.drawImage(value.image, value.params); },
          [&](const DrawTextCommand& value) {
#ifdef DONNER_TEXT_ENABLED
            auto& fontManager = textRegistry.ctx().contains<FontManager>()
                                    ? textRegistry.ctx().get<FontManager>()
                                    : textRegistry.ctx().emplace<FontManager>(textRegistry);
            auto& textEngine =
                textRegistry.ctx().contains<TextEngine>()
                    ? textRegistry.ctx().get<TextEngine>()
                    : textRegistry.ctx().emplace<TextEngine>(fontManager, textRegistry);
            textEngine.addFontFaces(value.fontFaces);
#endif

            TextParams params = value.params;
            params.fontFaces =
                std::span<const css::FontFace>(value.fontFaces.data(), value.fontFaces.size());
            params.textRootEntity = entt::null;
            renderer.drawText(textRegistry, value.text, params);
          },
      },
      command);
}

}  // namespace

struct RenderSnapshot::Impl {
  std::uint64_t sourceRevision = 0;
  Registry resourceRegistry;
  SnapshotResourceMapper resourceMapper{resourceRegistry};
  std::vector<RenderCommand> commands;
};

RenderSnapshot::RenderSnapshot() : impl_(std::make_unique<Impl>()) {}

RenderSnapshot::~RenderSnapshot() = default;

RenderSnapshot::RenderSnapshot(RenderSnapshot&& other) noexcept = default;

RenderSnapshot& RenderSnapshot::operator=(RenderSnapshot&& other) noexcept = default;

std::uint64_t RenderSnapshot::sourceRevision() const {
  return impl_->sourceRevision;
}

std::size_t RenderSnapshot::commandCount() const {
  return impl_->commands.size();
}

std::size_t RenderSnapshot::estimatedCommandStorageBytes() const {
  return sizeof(Impl) + impl_->commands.capacity() * sizeof(RenderCommand);
}

std::size_t RenderSnapshot::liveRegistryReferenceCountForTesting(const Registry& registry) const {
  std::size_t count = 0;
  for (const RenderCommand& command : impl_->commands) {
    count += CountReferenceToRegistry(command, registry);
  }
  return count;
}

void RenderSnapshot::replay(RendererInterface& renderer) const {
  Registry textRegistry;
  std::size_t rejectedPatternDepth = 0;
  for (const RenderCommand& command : impl_->commands) {
    if (const auto* beginPattern = std::get_if<BeginPatternTileCommand>(&command)) {
      if (rejectedPatternDepth > 0 ||
          !renderer.beginPatternTile(beginPattern->tileRect, beginPattern->targetFromPattern)) {
        ++rejectedPatternDepth;
      }
      continue;
    }

    if (const auto* endPattern = std::get_if<EndPatternTileCommand>(&command)) {
      if (rejectedPatternDepth > 0) {
        --rejectedPatternDepth;
      } else {
        renderer.endPatternTile(endPattern->forStroke);
      }
      continue;
    }

    if (rejectedPatternDepth > 0) {
      continue;
    }
    ReplayCommand(command, renderer, textRegistry);
  }
}

void RenderSnapshot::setSourceRevision(std::uint64_t revision) {
  impl_->sourceRevision = revision;
}

RenderSnapshotRecorder::RenderSnapshotRecorder(RenderSnapshot& snapshot,
                                               RendererInterface& offscreenFactory)
    : snapshot_(snapshot), offscreenFactory_(offscreenFactory) {}

void RenderSnapshotRecorder::draw(SVGDocument&) {}

int RenderSnapshotRecorder::width() const {
  return offscreenFactory_.width();
}

int RenderSnapshotRecorder::height() const {
  return offscreenFactory_.height();
}

void RenderSnapshotRecorder::beginFrame(const RenderViewport& viewport) {
  snapshot_.impl_->commands.push_back(BeginFrameCommand{viewport});
}

void RenderSnapshotRecorder::endFrame() {
  snapshot_.impl_->commands.push_back(EndFrameCommand{});
}

void RenderSnapshotRecorder::setTransform(const Transform2d& transform) {
  snapshot_.impl_->commands.push_back(SetTransformCommand{transform});
}

void RenderSnapshotRecorder::pushTransform(const Transform2d& transform) {
  snapshot_.impl_->commands.push_back(PushTransformCommand{transform});
}

void RenderSnapshotRecorder::popTransform() {
  snapshot_.impl_->commands.push_back(PopTransformCommand{});
}

void RenderSnapshotRecorder::pushClip(const ResolvedClip& clip) {
  snapshot_.impl_->commands.push_back(
      PushClipCommand{snapshot_.impl_->resourceMapper.mapClip(clip)});
}

void RenderSnapshotRecorder::popClip() {
  snapshot_.impl_->commands.push_back(PopClipCommand{});
}

void RenderSnapshotRecorder::pushIsolatedLayer(double opacity, MixBlendMode blendMode) {
  snapshot_.impl_->commands.push_back(PushIsolatedLayerCommand{opacity, blendMode});
}

void RenderSnapshotRecorder::popIsolatedLayer() {
  snapshot_.impl_->commands.push_back(PopIsolatedLayerCommand{});
}

void RenderSnapshotRecorder::pushFilterLayer(const components::FilterGraph& filterGraph,
                                             const std::optional<Box2d>& filterRegion) {
  snapshot_.impl_->commands.push_back(PushFilterLayerCommand{
      snapshot_.impl_->resourceMapper.mapFilterGraph(filterGraph), filterRegion});
}

void RenderSnapshotRecorder::popFilterLayer() {
  snapshot_.impl_->commands.push_back(PopFilterLayerCommand{});
}

void RenderSnapshotRecorder::pushMask(const std::optional<Box2d>& maskBounds) {
  snapshot_.impl_->commands.push_back(PushMaskCommand{maskBounds});
}

void RenderSnapshotRecorder::transitionMaskToContent() {
  snapshot_.impl_->commands.push_back(TransitionMaskToContentCommand{});
}

void RenderSnapshotRecorder::popMask() {
  snapshot_.impl_->commands.push_back(PopMaskCommand{});
}

bool RenderSnapshotRecorder::beginPatternTile(const Box2d& tileRect,
                                              const Transform2d& targetFromPattern) {
  snapshot_.impl_->commands.push_back(BeginPatternTileCommand{tileRect, targetFromPattern});
  return true;
}

void RenderSnapshotRecorder::endPatternTile(bool forStroke) {
  snapshot_.impl_->commands.push_back(EndPatternTileCommand{forStroke});
}

void RenderSnapshotRecorder::setPaint(const PaintParams& paint) {
  snapshot_.impl_->commands.push_back(
      SetPaintCommand{snapshot_.impl_->resourceMapper.mapPaintParams(paint)});
}

void RenderSnapshotRecorder::drawPath(const PathShape& path, const StrokeParams& stroke) {
  PathShape snapshotPath = path;
  snapshotPath.sourceEntity = EntityHandle();
  snapshot_.impl_->commands.push_back(DrawPathCommand{std::move(snapshotPath), stroke});
}

void RenderSnapshotRecorder::drawRect(const Box2d& rect, const StrokeParams& stroke) {
  snapshot_.impl_->commands.push_back(DrawRectCommand{rect, stroke});
}

void RenderSnapshotRecorder::drawEllipse(const Box2d& bounds, const StrokeParams& stroke) {
  snapshot_.impl_->commands.push_back(DrawEllipseCommand{bounds, stroke});
}

void RenderSnapshotRecorder::drawImage(const ImageResource& image, const ImageParams& params) {
  snapshot_.impl_->commands.push_back(DrawImageCommand{image, params});
}

void RenderSnapshotRecorder::drawText(Registry&, const components::ComputedTextComponent& text,
                                      const TextParams& params) {
  DrawTextCommand command;
  command.text = snapshot_.impl_->resourceMapper.mapComputedText(text);
  command.params = params;
  command.params.fontFaces = {};
  command.params.textRootEntity = entt::null;
  command.fontFaces.assign(params.fontFaces.begin(), params.fontFaces.end());
  snapshot_.impl_->commands.push_back(std::move(command));
}

RendererBitmap RenderSnapshotRecorder::takeSnapshot() const {
  return RendererBitmap();
}

std::unique_ptr<RendererInterface> RenderSnapshotRecorder::createOffscreenInstance() const {
  return offscreenFactory_.createOffscreenInstance();
}

}  // namespace donner::svg
