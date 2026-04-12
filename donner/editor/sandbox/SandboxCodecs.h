#pragma once
/// @file
///
/// Encoders and decoders for Donner value types that cross the sandbox wire
/// boundary. Every `Encode*` writes its type into a `WireWriter`; every
/// `Decode*` reads from a `WireReader` and sets the reader's failure flag on
/// any problem (truncation, invalid variant tag, out-of-range enum, length
/// cap). Decoders never throw and never crash on adversarial input.
///
/// Scope for S2: paths, transforms, boxes, colors, stroke params, solid paint
/// servers, simple clips. Gradients, patterns, filters, masks, text, and
/// images are out of scope and the `SerializingRenderer` emits a
/// `kUnsupported` message instead of attempting to encode them.

#include <optional>
#include <vector>

#include "donner/base/Box.h"
#include "donner/base/FillRule.h"
#include "donner/base/Length.h"
#include "donner/base/Path.h"
#include "donner/base/Transform.h"
#include "donner/base/Vector2.h"
#include "donner/css/Color.h"
#include "donner/css/FontFace.h"
#include "donner/editor/sandbox/Wire.h"
#include "donner/svg/components/RenderingInstanceComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"
#include "donner/svg/core/Gradient.h"
#include "donner/svg/core/MixBlendMode.h"
#include "donner/svg/core/Stroke.h"
#include "donner/svg/properties/PaintServer.h"
#include "donner/svg/renderer/RendererInterface.h"
#include "donner/svg/renderer/StrokeParams.h"
#include "donner/svg/resources/ImageResource.h"

namespace donner::editor::sandbox {

/// @name Primitive Donner types
/// @{
void EncodeVector2d(WireWriter& w, const Vector2d& v);
[[nodiscard]] bool DecodeVector2d(WireReader& r, Vector2d& out);

void EncodeVector2i(WireWriter& w, const Vector2i& v);
[[nodiscard]] bool DecodeVector2i(WireReader& r, Vector2i& out);

void EncodeTransform2d(WireWriter& w, const Transform2d& t);
[[nodiscard]] bool DecodeTransform2d(WireReader& r, Transform2d& out);

void EncodeBox2d(WireWriter& w, const Box2d& b);
[[nodiscard]] bool DecodeBox2d(WireReader& r, Box2d& out);

void EncodeRgba(WireWriter& w, const css::RGBA& rgba);
[[nodiscard]] bool DecodeRgba(WireReader& r, css::RGBA& out);

/// Encodes a `css::Color`. HSLA and CurrentColor are not fully faithful in
/// S2 — HSLA is converted to RGBA, and CurrentColor is encoded with a
/// fallback RGBA so the replayed paint doesn't resolve to a surprise value.
void EncodeColor(WireWriter& w, const css::Color& color);
[[nodiscard]] bool DecodeColor(WireReader& r, css::Color& out);
/// @}

/// @name Enums (encoded as a single `u8` byte each)
/// @{
void EncodeFillRule(WireWriter& w, FillRule v);
[[nodiscard]] bool DecodeFillRule(WireReader& r, FillRule& out);

void EncodeMixBlendMode(WireWriter& w, svg::MixBlendMode v);
[[nodiscard]] bool DecodeMixBlendMode(WireReader& r, svg::MixBlendMode& out);

void EncodeStrokeLinecap(WireWriter& w, svg::StrokeLinecap v);
[[nodiscard]] bool DecodeStrokeLinecap(WireReader& r, svg::StrokeLinecap& out);

void EncodeStrokeLinejoin(WireWriter& w, svg::StrokeLinejoin v);
[[nodiscard]] bool DecodeStrokeLinejoin(WireReader& r, svg::StrokeLinejoin& out);
/// @}

/// @name Path and stroke state
/// @{

/// Encodes a `Path` as: `u32 commandCount`, then each command as
/// `u8 verb, u32 pointIndex`, then `u32 pointCount`, then `pointCount *
/// Vector2d`. `isInternal` is not preserved — S2 paths come straight from the
/// driver and the flag is only used for marker placement, which happens
/// before this encoder sees the path.
void EncodePath(WireWriter& w, const Path& path);
[[nodiscard]] bool DecodePath(WireReader& r, Path& out);

void EncodeStrokeParams(WireWriter& w, const svg::StrokeParams& s);
[[nodiscard]] bool DecodeStrokeParams(WireReader& r, svg::StrokeParams& out);

void EncodePathShape(WireWriter& w, const svg::PathShape& p);
[[nodiscard]] bool DecodePathShape(WireReader& r, svg::PathShape& out);
/// @}

/// @name Paint and clip
/// @{

/// Self-contained gradient payload — everything needed to reconstruct a
/// linear or radial gradient on the replay side without registry lookups.
/// This is the serializer's side of WIRE.5: it flattens a
/// `PaintResolvedReference` to a gradient entity into plain values that
/// cross the wire, then the replayer materializes them onto a fresh ECS
/// entity via `GradientReplayRegistry`.
struct WireGradient {
  enum class Kind : uint8_t { kLinear = 0, kRadial = 1 };
  Kind kind = Kind::kLinear;

  // Common fields (apply to both linear and radial).
  svg::GradientUnits units = svg::GradientUnits::Default;
  svg::GradientSpreadMethod spreadMethod = svg::GradientSpreadMethod::Default;
  std::vector<svg::GradientStop> stops;

  // Linear fields — used when kind == kLinear.
  Lengthd x1;
  Lengthd y1;
  Lengthd x2;
  Lengthd y2;

  // Radial fields — used when kind == kRadial.
  Lengthd cx;
  Lengthd cy;
  Lengthd r;
  std::optional<Lengthd> fx;
  std::optional<Lengthd> fy;
  Lengthd fr;

  // Fallback color carried from the original PaintResolvedReference.
  std::optional<css::Color> fallback;
};

/// Encodes a `Lengthd` as `f64 value, u8 unit`.
void EncodeLengthd(WireWriter& w, const Lengthd& v);
[[nodiscard]] bool DecodeLengthd(WireReader& r, Lengthd& out);

/// Encodes a `WireGradient` in full. The gradient kind tag is written first
/// so a truncated stream can still be recognized as malformed early.
void EncodeWireGradient(WireWriter& w, const WireGradient& g);
[[nodiscard]] bool DecodeWireGradient(WireReader& r, WireGradient& out);

/// Encodes a `ResolvedPaintServer` variant. Supports:
///   - `PaintServer::None`
///   - `PaintServer::Solid`
///   - `PaintResolvedReference` carrying a **linear or radial gradient**
///     (flattened to a `WireGradient`).
/// Pattern paint servers and other unresolved references still fall through
/// as stubs that decode to `None`. That's a separate milestone — see
/// `docs/design_docs/editor_sandbox.md`.
void EncodeResolvedPaintServer(WireWriter& w, const svg::components::ResolvedPaintServer& p);

/// Decodes a `ResolvedPaintServer`. When the wire carries a gradient tag
/// the fully-decoded `WireGradient` is stashed in `*outPendingGradient`
/// (if non-null) and `out` is set to `PaintServer::None`. The caller is
/// expected to materialize the gradient via a replay-side ECS registry
/// before forwarding `out` to the backend. If `outPendingGradient` is null,
/// gradients silently decode as `None`.
[[nodiscard]] bool DecodeResolvedPaintServer(
    WireReader& r, svg::components::ResolvedPaintServer& out,
    std::optional<WireGradient>* outPendingGradient = nullptr);

void EncodePaintParams(WireWriter& w, const svg::PaintParams& p);

/// Decodes a `PaintParams`. Fill and stroke gradients (if any) are stashed
/// in the corresponding out-parameters rather than being flattened into the
/// result's `ResolvedPaintServer` variants — see `DecodeResolvedPaintServer`.
[[nodiscard]] bool DecodePaintParams(
    WireReader& r, svg::PaintParams& out,
    std::optional<WireGradient>* outFillGradient = nullptr,
    std::optional<WireGradient>* outStrokeGradient = nullptr);

/// Encodes the subset of `ResolvedClip` we support in S2: rect + paths +
/// unit-transform. The optional mask is always encoded as "absent" — masks
/// cross the wire as `kUnsupported` from higher up in the caller chain.
void EncodeResolvedClip(WireWriter& w, const svg::ResolvedClip& c);
[[nodiscard]] bool DecodeResolvedClip(WireReader& r, svg::ResolvedClip& out);
/// @}

/// @name Misc
/// @{
void EncodeRenderViewport(WireWriter& w, const svg::RenderViewport& v);
[[nodiscard]] bool DecodeRenderViewport(WireReader& r, svg::RenderViewport& out);

void EncodeImageParams(WireWriter& w, const svg::ImageParams& p);
[[nodiscard]] bool DecodeImageParams(WireReader& r, svg::ImageParams& out);

/// Encodes an `ImageResource` as: `u32 width, u32 height, u32 byteLength,
/// u8[byteLength]`. Bytes are stored RGBA-straight (the same format
/// `RendererInterface::drawImage` expects).
void EncodeImageResource(WireWriter& w, const svg::ImageResource& img);
[[nodiscard]] bool DecodeImageResource(WireReader& r, svg::ImageResource& out);

/// Encodes a single `css::FontFace` (family name, weight/style/stretch,
/// and the source list including inline font data blobs).
void EncodeFontFace(WireWriter& w, const css::FontFace& face);

/// Decodes a single `css::FontFace`. Returns false on any parse failure.
[[nodiscard]] bool DecodeFontFace(WireReader& r, css::FontFace& out);

/// Encodes a `TextParams` struct including `fontFaces` with full inline
/// font data. Up to 256 font faces are supported.
void EncodeTextParams(WireWriter& w, const svg::TextParams& p);

/// Decodes a `TextParams`. The decoded font faces are stored into
/// `outFontFaces` (if non-null) so the caller can keep the backing
/// storage alive for the `fontFaces` span. When `outFontFaces` is
/// provided and non-empty after decode, `out.fontFaces` is set to
/// reference it.
[[nodiscard]] bool DecodeTextParams(WireReader& r, svg::TextParams& out,
                                    std::vector<css::FontFace>* outFontFaces = nullptr);

/// Encodes a `ComputedTextComponent` (vector of TextSpan, each carrying
/// text content, positioning lists, paint servers, font style, etc.).
void EncodeComputedTextComponent(WireWriter& w,
                                 const svg::components::ComputedTextComponent& text);
[[nodiscard]] bool DecodeComputedTextComponent(
    WireReader& r, svg::components::ComputedTextComponent& out);

/// Encodes a `FilterGraph` as metadata only: 0 nodes (transparent
/// pass-through), colorInterpolationFilters, primitiveUnits,
/// elementBoundingBox, filterRegion, userToPixelScale. The full primitive
/// chain is deferred to a follow-up milestone — the pass-through stub
/// preserves layout without producing visual effects.
void EncodeFilterGraph(WireWriter& w, const svg::components::FilterGraph& g);
[[nodiscard]] bool DecodeFilterGraph(WireReader& r, svg::components::FilterGraph& out);
/// @}

}  // namespace donner::editor::sandbox
