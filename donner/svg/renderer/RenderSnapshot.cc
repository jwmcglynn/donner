#include "donner/svg/renderer/RenderSnapshot.h"

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
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
#include "donner/svg/renderer/PlacedTextGeometry.h"
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"
#endif

namespace donner::svg {
#ifdef DONNER_TEXT_ENABLED
struct SnapshotFontPayload {
  std::vector<uint8_t> bytes;
  FontDataTrust trust = FontDataTrust::Untrusted;
  FontDataRenderingOptions rendering;
};
#endif
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
  MaskType maskType = MaskType::Luminance;
  ColorInterpolation colorInterpolation = ColorInterpolation::SRGB;
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

/// Recorded `drawPath`. A snapshot outlives the document it was captured from, so unlike the
/// borrowed `PathShape` handed to the renderer this owns its geometry; replay hands a
/// `PathShape` pointing back at that owned copy.
struct DrawPathCommand {
  Path path;
  FillRule fillRule = FillRule::NonZero;
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

#ifdef DONNER_TEXT_ENABLED

using SnapshotFont = std::shared_ptr<const SnapshotFontPayload>;
#endif

/// Each source registry keeps its own font entity namespace and font-loading budget on replay.
struct SnapshotReplayFontState {
  struct Domain {
    Registry registry;
    std::shared_ptr<components::DocumentResourceFamilyBudget> decorationFamily;
#ifdef DONNER_TEXT_ENABLED
    std::unordered_map<const SnapshotFontPayload*, FontHandle> handles;
#endif
  };

  Domain& domain(size_t index) {
    if (index >= domains.size()) {
      domains.resize(index + 1);
    }
    if (!domains[index]) {
      domains[index] = std::make_unique<Domain>();
    }
    return *domains[index];
  }

  std::vector<std::unique_ptr<Domain>> domains;
};

struct DrawTextCommand {
  components::ComputedTextComponent text;
  TextParams params;
  std::vector<css::FontFace> fontFaces;
  std::optional<size_t> fontDomain;
#ifdef DONNER_TEXT_ENABLED
  std::vector<SnapshotFont> preparedFonts;
  /// Fallback faces of prepared glyphs, keyed by the glyph's ordinal across all prepared runs.
  std::vector<std::pair<size_t, SnapshotFont>> preparedGlyphFonts;
#endif
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

// Font identities use weak ownership, so a destroyed source cannot alias a new source at the
// same address. The pointer is used only for sources without an installed font-loading domain.
struct FontDomainKey {
  std::weak_ptr<const void> owner;
  Registry* uninitializedSource = nullptr;
  size_t retainedIdentityBytes = 0;

  bool operator<(const FontDomainKey& other) const {
    const std::owner_less<std::weak_ptr<const void>> less;
    if (less(owner, other.owner)) {
      return true;
    }
    if (less(other.owner, owner)) {
      return false;
    }
    return std::less<Registry*>{}(uninitializedSource, other.uninitializedSource);
  }
};

FontDomainKey GetFontDomainKey(Registry& source) {
#ifdef DONNER_TEXT_ENABLED
  if (const auto* manager = source.ctx().find<FontManager>()) {
    auto identity = manager->fontDomainIdentity();
    auto& owner = identity.owner;
    const std::weak_ptr<const void> empty;
    if (owner.owner_before(empty) || empty.owner_before(owner)) {
      return {std::move(owner), nullptr, identity.retainedStorageBytes};
    }
  }
#endif
  return {{}, &source};
}

struct FontSourceKey {
  FontDomainKey domain;
  Entity entity = entt::null;

  bool operator<(const FontSourceKey& other) const {
    if (domain < other.domain) {
      return true;
    }
    if (other.domain < domain) {
      return false;
    }
    return entity < other.entity;
  }
};

// Conservative map-node/control-block allowances supplement the owned value sizes. The cap
// accounts for weak keys as well as encoded bytes, including domains whose faces are refused.
template <typename Value>
constexpr size_t SnapshotNodeBytes = sizeof(Value) + 4 * sizeof(void*);

class SnapshotResourceMapper {
public:
  explicit SnapshotResourceMapper(Registry& registry) : registry_(registry) {}

  void setMaximumCapturedFontBytes(size_t maximum) { maximumCapturedFontBytes_ = maximum; }
  void setMaximumCapturedDecorationBytes(std::size_t maximum) {
    maximumCapturedDecorationBytes_ = maximum;
    if (capturedDecorationBytes_ == 0) {
      components::DocumentResourceFamilyBudget::Limits limits;
      limits.geometryBytes = maximum;
      limits.maximumTotalRetainedBytes = maximum;
      decorationFamily_ = std::make_shared<components::DocumentResourceFamilyBudget>(limits);
    }
  }
  bool fontPayloadLimitExceeded() const { return fontPayloadLimitExceeded_; }

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

  /// Decoration faces are copied only when the draw's font domain was admitted; a refused
  /// domain replays in isolation and must not retain or borrow any face.
  components::ComputedTextComponent mapComputedText(Registry& sourceRegistry,
                                                    components::ComputedTextComponent text,
                                                    bool captureFonts) {
    for (auto& span : text.spans) {
      span.sourceEntity = entt::null;
      span.textPathSourceEntity = entt::null;
      span.resolvedFill = mapPaint(std::move(span.resolvedFill));
      span.resolvedStroke = mapPaint(std::move(span.resolvedStroke));
      if (span.decorations) {
        const auto previous = span.decorations;
        std::array<components::TextDecorationOrigin,
                   components::TextDecorationData::kMaximumOrigins>
            origins;
        std::copy(previous->origins().begin(), previous->origins().end(), origins.begin());
        for (std::size_t index = 0; index < previous->origins().size(); ++index) {
          auto& origin = origins[index];
          origin.fill = mapPaint(std::move(origin.fill));
          origin.stroke = mapPaint(std::move(origin.stroke));
#ifdef DONNER_TEXT_ENABLED
          if (origin.font) {
            origin.snapshotFont =
                captureFonts ? captureFont(sourceRegistry, origin.font->font) : SnapshotFont();
            origin.font->font = FontHandle();
          }
#else
          (void)sourceRegistry;
          (void)captureFonts;
#endif
        }
        const std::size_t bytes =
            sizeof(components::TextDecorationData) +
            previous->origins().size() * sizeof(components::TextDecorationOrigin);
        if (capturedDecorationBytes_ > maximumCapturedDecorationBytes_ ||
            bytes > maximumCapturedDecorationBytes_ - capturedDecorationBytes_) {
          span.decorations.reset();
        } else {
          span.decorations = components::TextDecorationData::Create(
              decorationFamily_, std::span(origins).first(previous->origins().size()));
          if (span.decorations) {
            capturedDecorationBytes_ += bytes;
          }
        }
        if (!span.decorations) {
          span.textDecoration = TextDecoration::None;
        }
      }
    }
    return text;
  }

  std::optional<size_t> captureTextDomain(Registry& sourceRegistry) {
    const FontDomainKey key = GetFontDomainKey(sourceRegistry);
    if (const auto it = textDomains_.find(key); it != textDomains_.end()) {
      return it->second;
    }
    const size_t cost =
        SnapshotNodeBytes<std::pair<const FontDomainKey, size_t>> + key.retainedIdentityBytes;
    if (!admitFontStorage(cost)) {
      return std::nullopt;
    }
    const size_t domain = textDomains_.size();
    textDomains_.emplace(key, domain);
    return domain;
  }

  bool admitFontStorage(size_t bytes) {
    if (fontPayloadLimitExceeded_ || capturedFontBytes_ > maximumCapturedFontBytes_ ||
        bytes > maximumCapturedFontBytes_ - capturedFontBytes_) {
      fontPayloadLimitExceeded_ = true;
      return false;
    }
    capturedFontBytes_ += bytes;
    return true;
  }

#ifdef DONNER_TEXT_ENABLED
  SnapshotFont captureFont(Registry& sourceRegistry, FontHandle handle) {
    if (!handle) {
      return {};
    }
    // Entity IDs can collide across subdocuments; capture bytes once per source font.
    const FontSourceKey key{GetFontDomainKey(sourceRegistry), handle.entity()};
    if (const auto it = capturedFonts_.find(key); it != capturedFonts_.end()) {
      return it->second;
    }
    const auto* manager = sourceRegistry.ctx().find<FontManager>();
    if (manager == nullptr) {
      return {};
    }
    const std::span<const uint8_t> bytes = manager->fontData(handle);
    if (bytes.empty()) {
      return {};
    }
    constexpr size_t metadata = SnapshotNodeBytes<SnapshotFontPayload> +
                                SnapshotNodeBytes<std::pair<const FontSourceKey, SnapshotFont>>;
    if (bytes.size() > maximumCapturedFontBytes_ ||
        metadata > maximumCapturedFontBytes_ - bytes.size() ||
        !admitFontStorage(bytes.size() + metadata)) {
      fontPayloadLimitExceeded_ = true;
      return {};
    }
    auto payload = std::make_shared<SnapshotFontPayload>();
    payload->bytes.assign(bytes.begin(), bytes.end());
    payload->trust =
        manager->isTrustedFont(handle) ? FontDataTrust::Trusted : FontDataTrust::Untrusted;
    payload->rendering = FontDataRenderingOptions{manager->providerFaceRequest(handle),
                                                  manager->isGenericSansProviderFont(handle),
                                                  manager->isImmutableCatalogFont(handle)};
    capturedFonts_.emplace(key, payload);
    return payload;
  }
#endif

  ResolvedClip mapClip(ResolvedClip clip) {
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

  std::shared_ptr<components::DocumentResourceFamilyBudget> decorationFamily_ =
      std::make_shared<components::DocumentResourceFamilyBudget>();
  std::size_t maximumCapturedDecorationBytes_ = RenderSnapshot::kMaximumCapturedDecorationBytes;
  std::size_t capturedDecorationBytes_ = 0;
  Registry& registry_;
  std::unordered_map<SourceEntityKey, Entity, SourceEntityKeyHash> mappedEntities_;
  std::map<FontDomainKey, size_t> textDomains_;
  size_t maximumCapturedFontBytes_ = RenderSnapshot::kMaximumCapturedFontBytes;
  size_t capturedFontBytes_ = 0;
  bool fontPayloadLimitExceeded_ = false;
#ifdef DONNER_TEXT_ENABLED
  std::map<FontSourceKey, SnapshotFont> capturedFonts_;
#endif
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
  // Clip paths hold no entity handle at all (see `ClipPathShape`), so only the mask chain
  // can reference the registry.
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
    if (span.decorations) {
      for (const auto& origin : span.decorations->origins()) {
        count += CountReferenceToRegistry(origin.fill, registry);
        count += CountReferenceToRegistry(origin.stroke, registry);
      }
    }
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
                        [&](const DrawImageCommand& value) {
                          return CountReferenceToRegistry(value.params.sourceEntity, registry);
                        },
                        [&](const DrawTextCommand& value) {
                          return CountReferenceToRegistry(value.text, registry);
                        },
                        [](const auto&) -> std::size_t { return 0; },
                    },
                    command);
}

#ifdef DONNER_TEXT_ENABLED
FontHandle LoadSnapshotFont(FontManager& manager, const SnapshotFont& font,
                            SnapshotReplayFontState::Domain& domain) {
  if (!font) {
    return FontHandle();
  }
  const auto [it, inserted] = domain.handles.try_emplace(font.get());
  if (inserted) {
    it->second =
        manager.loadFontData(std::span<const uint8_t>(font->bytes.data(), font->bytes.size()),
                             font->trust, font->rendering);
  }
  return it->second;
}

/// Replaces recorded prepared-run and fallback-glyph fonts with faces loaded into \p manager.
void LoadPreparedTextFonts(PreparedTextDraw& prepared, const DrawTextCommand& command,
                           FontManager& manager, SnapshotReplayFontState::Domain& domain) {
  auto glyphFont = command.preparedGlyphFonts.begin();
  size_t glyphOrdinal = 0;
  for (size_t i = 0; i < prepared.runs.size(); ++i) {
    prepared.runs[i].font = i < command.preparedFonts.size()
                                ? LoadSnapshotFont(manager, command.preparedFonts[i], domain)
                                : FontHandle();
    for (TextGlyph& glyph : prepared.runs[i].glyphs) {
      if (glyphFont != command.preparedGlyphFonts.end() && glyphFont->first == glyphOrdinal) {
        glyph.font = LoadSnapshotFont(manager, glyphFont->second, domain);
        if (!glyph.font) {
          // Without its own face, the glyph id would select an unrelated glyph of the run's face.
          glyph.glyphIndex = 0;
        }
        ++glyphFont;
      }
      ++glyphOrdinal;
    }
  }
}
#endif

void ReplayCommand(const RenderCommand& command, RendererInterface& renderer,
                   SnapshotReplayFontState& replayFonts) {
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
          [&](const PushMaskCommand& value) {
            if (value.colorInterpolation == ColorInterpolation::LinearRGB) {
              renderer.pushMask(value.maskBounds, value.maskType, value.colorInterpolation);
            } else {
              renderer.pushMask(value.maskBounds, value.maskType);
            }
          },
          [&](const TransitionMaskToContentCommand&) { renderer.transitionMaskToContent(); },
          [&](const PopMaskCommand&) { renderer.popMask(); },
          [&](const BeginPatternTileCommand& value) {
            (void)renderer.beginPatternTile(value.tileRect, value.targetFromPattern);
          },
          [&](const EndPatternTileCommand& value) { renderer.endPatternTile(value.forStroke); },
          [&](const SetPaintCommand& value) { renderer.setPaint(value.paint); },
          [&](const DrawPathCommand& value) {
            renderer.drawPath(PathShape{&value.path, value.fillRule}, value.stroke);
          },
          [&](const DrawRectCommand& value) { renderer.drawRect(value.rect, value.stroke); },
          [&](const DrawEllipseCommand& value) {
            renderer.drawEllipse(value.bounds, value.stroke);
          },
          [&](const DrawImageCommand& value) { renderer.drawImage(value.image, value.params); },
          [&](const DrawTextCommand& value) {
            // A refused domain still emits the command, but cannot borrow any other face or
            // loading namespace. It has no retained key and is isolated for this draw.
            auto refusedDomain =
                value.fontDomain ? nullptr : std::make_unique<SnapshotReplayFontState::Domain>();
            auto& domain =
                value.fontDomain ? replayFonts.domain(*value.fontDomain) : *refusedDomain;
            Registry& textRegistry = domain.registry;
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
#ifdef DONNER_TEXT_ENABLED
            if (params.preparedTextDraw) {
              auto prepared = std::make_shared<PreparedTextDraw>(*params.preparedTextDraw);
              LoadPreparedTextFonts(*prepared, value, fontManager, domain);
              params.preparedTextDraw = std::move(prepared);
            }
            components::ComputedTextComponent text = value.text;
            for (auto& span : text.spans) {
              if (span.decorations) {
                const auto previous = span.decorations;
                std::array<components::TextDecorationOrigin,
                           components::TextDecorationData::kMaximumOrigins>
                    origins;
                std::copy(previous->origins().begin(), previous->origins().end(), origins.begin());
                for (std::size_t index = 0; index < previous->origins().size(); ++index) {
                  auto& origin = origins[index];
                  if (origin.font) {
                    origin.font->font = LoadSnapshotFont(fontManager, origin.snapshotFont, domain);
                  }
                }
                if (!domain.decorationFamily) {
                  domain.decorationFamily =
                      std::make_shared<components::DocumentResourceFamilyBudget>(
                          previous->family()->limits());
                }
                span.decorations = components::TextDecorationData::Create(
                    domain.decorationFamily, std::span(origins).first(previous->origins().size()));
                if (!span.decorations) {
                  span.textDecoration = TextDecoration::None;
                }
              }
            }
            renderer.drawText(textRegistry, text, params);
#else
            renderer.drawText(textRegistry, value.text, params);
#endif
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

bool RenderSnapshot::fontPayloadLimitExceeded() const {
  return impl_->resourceMapper.fontPayloadLimitExceeded();
}

void RenderSnapshot::replay(RendererInterface& renderer) const {
  SnapshotReplayFontState replayFonts;
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
    ReplayCommand(command, renderer, replayFonts);
  }
}

void RenderSnapshot::setSourceRevision(std::uint64_t revision) {
  impl_->sourceRevision = revision;
}

RenderSnapshotRecorder::RenderSnapshotRecorder(RenderSnapshot& snapshot,
                                               RendererInterface& offscreenFactory,
                                               std::size_t maximumCapturedFontBytes,
                                               std::size_t maximumCapturedDecorationBytes)
    : snapshot_(snapshot), offscreenFactory_(offscreenFactory) {
  snapshot_.impl_->resourceMapper.setMaximumCapturedFontBytes(maximumCapturedFontBytes);
  snapshot_.impl_->resourceMapper.setMaximumCapturedDecorationBytes(maximumCapturedDecorationBytes);
}

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
  pushMask(maskBounds, MaskType::Luminance);
}

void RenderSnapshotRecorder::pushMask(const std::optional<Box2d>& maskBounds, MaskType maskType) {
  pushMask(maskBounds, maskType, ColorInterpolation::SRGB);
}

void RenderSnapshotRecorder::pushMask(const std::optional<Box2d>& maskBounds, MaskType maskType,
                                      ColorInterpolation colorInterpolation) {
  snapshot_.impl_->commands.push_back(PushMaskCommand{maskBounds, maskType, colorInterpolation});
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
  // Deliberate copy: the recorded command has to own geometry that stays valid after the
  // source registry is gone. The source entity is dropped for the same reason.
  snapshot_.impl_->commands.push_back(DrawPathCommand{path.pathOrEmpty(), path.fillRule, stroke});
}

void RenderSnapshotRecorder::drawRect(const Box2d& rect, const StrokeParams& stroke) {
  snapshot_.impl_->commands.push_back(DrawRectCommand{rect, stroke});
}

void RenderSnapshotRecorder::drawEllipse(const Box2d& bounds, const StrokeParams& stroke) {
  snapshot_.impl_->commands.push_back(DrawEllipseCommand{bounds, stroke});
}

void RenderSnapshotRecorder::drawImage(const ImageResource& image, const ImageParams& params) {
  ImageParams snapshotParams = params;
  // Same rule as `drawPath`: a replayed command must not carry a handle into the live document
  // registry, so backends replaying a snapshot convert the image per draw instead of caching it
  // on its source entity.
  snapshotParams.sourceEntity = EntityHandle();
  snapshot_.impl_->commands.push_back(DrawImageCommand{image, std::move(snapshotParams)});
}

void RenderSnapshotRecorder::drawText(Registry& sourceRegistry,
                                      const components::ComputedTextComponent& text,
                                      const TextParams& params) {
  DrawTextCommand command;
  command.fontDomain = snapshot_.impl_->resourceMapper.captureTextDomain(sourceRegistry);
  command.text = snapshot_.impl_->resourceMapper.mapComputedText(sourceRegistry, text,
                                                                 command.fontDomain.has_value());
  command.params = params;
  command.params.fontFaces = {};
  command.params.textRootEntity = entt::null;
  command.fontFaces.assign(params.fontFaces.begin(), params.fontFaces.end());
#ifdef DONNER_TEXT_ENABLED
  const auto captureFont = [&](FontHandle font) {
    return command.fontDomain ? snapshot_.impl_->resourceMapper.captureFont(sourceRegistry, font)
                              : SnapshotFont();
  };
  if (params.preparedTextDraw) {
    // Keep the placed glyphs, but replace every source-registry font handle on replay.
    auto prepared = std::make_shared<PreparedTextDraw>(*params.preparedTextDraw);
    command.preparedFonts.reserve(prepared->runs.size());
    size_t glyphOrdinal = 0;
    for (TextRun& run : prepared->runs) {
      command.preparedFonts.push_back(captureFont(run.font));
      run.font = FontHandle();
      for (TextGlyph& glyph : run.glyphs) {
        if (glyph.font) {
          command.preparedGlyphFonts.emplace_back(glyphOrdinal, captureFont(glyph.font));
          glyph.font = FontHandle();
        }
        ++glyphOrdinal;
      }
    }
    command.params.preparedTextDraw = std::move(prepared);
  }

#endif
  snapshot_.impl_->commands.push_back(std::move(command));
}

RendererBitmap RenderSnapshotRecorder::takeSnapshot() const {
  return RendererBitmap();
}

RendererFilterPreparationBudget* RenderSnapshotRecorder::filterPreparationBudget() {
  return offscreenFactory_.filterPreparationBudget();
}

std::unique_ptr<RendererInterface> RenderSnapshotRecorder::createOffscreenInstance() const {
  return offscreenFactory_.createOffscreenInstance();
}

}  // namespace donner::svg
