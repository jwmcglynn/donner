#include "donner/editor/sandbox/ReplayingRenderer.h"

#include <optional>
#include <utility>

#include "donner/base/EcsRegistry.h"
#include "donner/css/FontFace.h"
#include "donner/editor/sandbox/SandboxCodecs.h"
#include "donner/svg/graph/Reference.h"
#include "donner/svg/resources/ImageResource.h"

#ifdef DONNER_TEXT_ENABLED
#include "donner/svg/resources/FontManager.h"
#include "donner/svg/text/TextEngine.h"
#endif

namespace donner::editor::sandbox {

// Holds the replay-side ECS state needed to materialize gradient references
// from wire messages. One instance per ReplayingRenderer; fresh entities are
// created on demand inside `DecodePaintParams`, and the registry is cleared
// between frames via `resetFrame()`.
struct ReplayingRenderer::Impl {
  Registry registry;

  /// Backing storage for decoded font faces. Kept alive so that
  /// `TextParams::fontFaces` (a span) remains valid through the
  /// `drawText` call.
  std::vector<css::FontFace> lastFontFaces;

  void resetFrame() { registry.clear(); }

  /// Converts a decoded `WireGradient` into a real `PaintResolvedReference`
  /// suitable for handing to `RendererInterface::setPaint`. The returned
  /// variant references a fresh entity in `registry` that carries exactly
  /// the components `RendererTinySkia::makeFillPaint` / `makeStrokePaint`
  /// look up during paint resolution.
  svg::components::ResolvedPaintServer MaterializeGradient(const WireGradient& g) {
    return svg::MaterializeResolvedGradient(registry, g);
  }
};

ReplayingRenderer::ReplayingRenderer(svg::RendererInterface& target)
    : impl_(std::make_unique<Impl>()), target_(target) {}

ReplayingRenderer::~ReplayingRenderer() = default;

namespace {

bool ReadHeader(WireReader& r) {
  Opcode opcode = Opcode::kInvalid;
  uint32_t payloadLength = 0;
  if (!r.readMessageHeader(opcode, payloadLength)) return false;
  if (opcode != Opcode::kStreamHeader || payloadLength != 8) return false;

  uint32_t magic = 0;
  uint32_t version = 0;
  if (!r.readU32(magic) || !r.readU32(version)) return false;
  if (magic != kWireMagic || version != kWireVersion) return false;
  return true;
}

}  // namespace

ReplayStatus ReplayingRenderer::pumpFrame(std::span<const uint8_t> wire, ReplayReport& report) {
  report = ReplayReport{};
  WireReader r(wire);

  if (!ReadHeader(r)) {
    return ReplayStatus::kHeaderMismatch;
  }

  bool sawEndFrame = false;

  while (r.remaining() > 0 && !sawEndFrame) {
    Opcode opcode = Opcode::kInvalid;
    uint32_t payloadLength = 0;
    if (!r.readMessageHeader(opcode, payloadLength)) {
      return ReplayStatus::kMalformed;
    }

    const std::size_t payloadStart = r.position();
    if (payloadLength > r.remaining()) {
      return ReplayStatus::kMalformed;
    }

    report.lastOpcode = opcode;
    ++report.messagesProcessed;

    const DispatchOutcome outcome = handleMessage(r, opcode);

    switch (outcome) {
      case DispatchOutcome::kHandled: break;
      case DispatchOutcome::kUnsupported: ++report.unsupportedCount; break;
      case DispatchOutcome::kDecodeError: return ReplayStatus::kMalformed;
      case DispatchOutcome::kUnknownOpcode: {
        // Skip the remaining payload and report the unknown opcode.
        const std::size_t consumed = r.position() - payloadStart;
        if (consumed > payloadLength) return ReplayStatus::kMalformed;
        if (!r.skip(payloadLength - consumed)) return ReplayStatus::kMalformed;
        return ReplayStatus::kUnknownOpcode;
      }
    }

    // Verify the handler consumed exactly `payloadLength` bytes. A mismatch
    // means encoder and decoder disagree on field layout — fail hard rather
    // than silently render the wrong thing.
    const std::size_t consumed = r.position() - payloadStart;
    if (consumed != payloadLength) {
      return ReplayStatus::kMalformed;
    }

    if (opcode == Opcode::kEndFrame) {
      sawEndFrame = true;
    }
  }

  if (!sawEndFrame) {
    return ReplayStatus::kEndOfStream;
  }
  return report.unsupportedCount == 0 ? ReplayStatus::kOk : ReplayStatus::kEncounteredUnsupported;
}

ReplayingRenderer::DispatchOutcome ReplayingRenderer::handleMessage(WireReader& r, Opcode opcode) {
  switch (opcode) {
    case Opcode::kBeginFrame: {
      svg::RenderViewport viewport;
      if (!DecodeRenderViewport(r, viewport)) return DispatchOutcome::kDecodeError;
      // Fresh registry per frame so gradient entities don't pile up over a
      // long editor session. Materialization happens on each kSetPaint.
      impl_->resetFrame();
      target_.beginFrame(viewport);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kEndFrame: target_.endFrame(); return DispatchOutcome::kHandled;

    case Opcode::kSetTransform: {
      Transform2d t;
      if (!DecodeTransform2d(r, t)) return DispatchOutcome::kDecodeError;
      target_.setTransform(t);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kPushTransform: {
      Transform2d t;
      if (!DecodeTransform2d(r, t)) return DispatchOutcome::kDecodeError;
      target_.pushTransform(t);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kPopTransform: target_.popTransform(); return DispatchOutcome::kHandled;

    case Opcode::kPushClip: {
      svg::ResolvedClip clip;
      if (!DecodeResolvedClip(r, clip)) return DispatchOutcome::kDecodeError;
      target_.pushClip(clip);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kPopClip: target_.popClip(); return DispatchOutcome::kHandled;

    case Opcode::kPushIsolatedLayer: {
      double opacity = 0;
      svg::MixBlendMode blendMode = svg::MixBlendMode::Normal;
      if (!r.readF64(opacity)) return DispatchOutcome::kDecodeError;
      if (!DecodeMixBlendMode(r, blendMode)) return DispatchOutcome::kDecodeError;
      target_.pushIsolatedLayer(opacity, blendMode);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kPopIsolatedLayer: target_.popIsolatedLayer(); return DispatchOutcome::kHandled;

    case Opcode::kSetPaint: {
      svg::PaintParams paint;
      std::optional<WireGradient> fillGradient;
      std::optional<WireGradient> strokeGradient;
      if (!DecodePaintParams(r, paint, &fillGradient, &strokeGradient)) {
        return DispatchOutcome::kDecodeError;
      }
      // Materialize any gradient paint servers into the replayer's private
      // registry, then patch the PaintParams before forwarding. The backend
      // sees a normal PaintResolvedReference — it doesn't know or care that
      // the referenced entity only exists for the duration of this frame.
      if (fillGradient) {
        paint.fill = impl_->MaterializeGradient(*fillGradient);
      }
      if (strokeGradient) {
        paint.stroke = impl_->MaterializeGradient(*strokeGradient);
      }
      target_.setPaint(paint);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kDrawPath: {
      svg::PathShape shape;
      svg::StrokeParams stroke;
      if (!DecodePathShape(r, shape)) return DispatchOutcome::kDecodeError;
      if (!DecodeStrokeParams(r, stroke)) return DispatchOutcome::kDecodeError;
      target_.drawPath(shape, stroke);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kDrawRect: {
      Box2d rect;
      svg::StrokeParams stroke;
      if (!DecodeBox2d(r, rect)) return DispatchOutcome::kDecodeError;
      if (!DecodeStrokeParams(r, stroke)) return DispatchOutcome::kDecodeError;
      target_.drawRect(rect, stroke);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kDrawEllipse: {
      Box2d bounds;
      svg::StrokeParams stroke;
      if (!DecodeBox2d(r, bounds)) return DispatchOutcome::kDecodeError;
      if (!DecodeStrokeParams(r, stroke)) return DispatchOutcome::kDecodeError;
      target_.drawEllipse(bounds, stroke);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kPushMask: {
      bool hasBounds = false;
      if (!r.readBool(hasBounds)) return DispatchOutcome::kDecodeError;
      std::optional<Box2d> bounds;
      if (hasBounds) {
        Box2d b;
        if (!DecodeBox2d(r, b)) return DispatchOutcome::kDecodeError;
        bounds = b;
      }
      target_.pushMask(bounds);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kTransitionMaskToContent:
      target_.transitionMaskToContent();
      return DispatchOutcome::kHandled;

    case Opcode::kPopMask: target_.popMask(); return DispatchOutcome::kHandled;

    case Opcode::kBeginPatternTile: {
      Box2d tileRect;
      Transform2d targetFromPattern;
      if (!DecodeBox2d(r, tileRect)) return DispatchOutcome::kDecodeError;
      if (!DecodeTransform2d(r, targetFromPattern)) {
        return DispatchOutcome::kDecodeError;
      }
      target_.beginPatternTile(tileRect, targetFromPattern);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kEndPatternTile: {
      bool forStroke = false;
      if (!r.readBool(forStroke)) return DispatchOutcome::kDecodeError;
      target_.endPatternTile(forStroke);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kPushFilterLayer: {
      svg::components::FilterGraph filterGraph;
      if (!DecodeFilterGraph(r, filterGraph)) return DispatchOutcome::kDecodeError;
      bool hasRegion = false;
      std::optional<Box2d> filterRegion;
      if (!r.readBool(hasRegion)) return DispatchOutcome::kDecodeError;
      if (hasRegion) {
        Box2d box;
        if (!DecodeBox2d(r, box)) return DispatchOutcome::kDecodeError;
        filterRegion = box;
      }
      target_.pushFilterLayer(filterGraph, filterRegion);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kPopFilterLayer: target_.popFilterLayer(); return DispatchOutcome::kHandled;

    case Opcode::kDrawImage: {
      svg::ImageResource image;
      svg::ImageParams params;
      if (!DecodeImageResource(r, image)) return DispatchOutcome::kDecodeError;
      if (!DecodeImageParams(r, params)) return DispatchOutcome::kDecodeError;
      target_.drawImage(image, params);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kDrawText: {
      svg::components::ComputedTextComponent textComp;
      svg::TextParams params;
      if (!DecodeComputedTextComponent(r, textComp)) return DispatchOutcome::kDecodeError;
      if (!DecodeTextParams(r, params, &impl_->lastFontFaces)) return DispatchOutcome::kDecodeError;
      // Point the span at stable storage that outlives the drawText call.
      params.fontFaces = impl_->lastFontFaces;
#ifdef DONNER_TEXT_ENABLED
      // Lazily initialize FontManager + TextEngine on the replayer's private
      // registry so the backend's drawText can resolve fonts and lay out
      // glyphs. FontManager must be created first — TextEngine depends on it.
      if (!impl_->registry.ctx().contains<svg::TextEngine>()) {
        auto& fontManager = impl_->registry.ctx().emplace<svg::FontManager>(impl_->registry);
        impl_->registry.ctx().emplace<svg::TextEngine>(fontManager, impl_->registry);
      }
#endif
      target_.drawText(impl_->registry, textComp, params);
      return DispatchOutcome::kHandled;
    }

    case Opcode::kUnsupported: {
      uint32_t kind = 0;
      if (!r.readU32(kind)) return DispatchOutcome::kDecodeError;
      (void)kind;  // Diagnostic only; replay silently skips the draw call.
      return DispatchOutcome::kUnsupported;
    }

    case Opcode::kStreamHeader:
      // A second header mid-stream is a protocol error.
      return DispatchOutcome::kDecodeError;

    case Opcode::kInvalid:
    default: return DispatchOutcome::kUnknownOpcode;
  }
}

}  // namespace donner::editor::sandbox
