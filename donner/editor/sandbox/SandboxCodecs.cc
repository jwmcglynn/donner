#include "donner/editor/sandbox/SandboxCodecs.h"

#include <cstdint>
#include <variant>
#include <vector>

#include "donner/svg/components/paint/GradientComponent.h"
#include "donner/svg/components/paint/LinearGradientComponent.h"
#include "donner/svg/components/paint/RadialGradientComponent.h"
#include "donner/svg/components/text/ComputedTextComponent.h"

namespace donner::editor::sandbox {

namespace {

// Upper bound on how many Command/points pairs we'll accept in a single Path.
// Matches the per-frame cap guidance in docs/design_docs/0023-editor_sandbox.md.
constexpr uint32_t kMaxPathCommands = 10'000'000;
constexpr uint32_t kMaxPathPoints = 20'000'000;
constexpr uint32_t kMaxDashArrayLen = 4096;
constexpr uint32_t kMaxClipPaths = 1024;

// Reconstructs a Path from a decoded verb stream via PathBuilder. Points are
// consumed from a cursor to keep verb/point counts cross-checked.
bool BuildPathFromVerbs(const std::vector<Path::Verb>& verbs,
                       const std::vector<Vector2d>& points, Path& out) {
  PathBuilder builder;
  std::size_t cursor = 0;
  const auto take = [&](std::size_t n, std::span<const Vector2d>& dst) -> bool {
    if (points.size() - cursor < n) return false;
    dst = std::span<const Vector2d>(points.data() + cursor, n);
    cursor += n;
    return true;
  };

  for (const Path::Verb verb : verbs) {
    std::span<const Vector2d> pts;
    switch (verb) {
      case Path::Verb::MoveTo:
        if (!take(1, pts)) return false;
        builder.moveTo(pts[0]);
        break;
      case Path::Verb::LineTo:
        if (!take(1, pts)) return false;
        builder.lineTo(pts[0]);
        break;
      case Path::Verb::QuadTo:
        if (!take(2, pts)) return false;
        builder.quadTo(pts[0], pts[1]);
        break;
      case Path::Verb::CurveTo:
        if (!take(3, pts)) return false;
        builder.curveTo(pts[0], pts[1], pts[2]);
        break;
      case Path::Verb::ClosePath:
        builder.closePath();
        break;
    }
  }

  if (cursor != points.size()) {
    // Trailing point data with no verb to consume it — treat as corruption.
    return false;
  }
  out = builder.build();
  return true;
}

std::size_t PointsPerVerb(Path::Verb verb) {
  switch (verb) {
    case Path::Verb::MoveTo:
    case Path::Verb::LineTo:
      return 1;
    case Path::Verb::QuadTo:
      return 2;
    case Path::Verb::CurveTo:
      return 3;
    case Path::Verb::ClosePath:
      return 0;
  }
  return 0;
}

}  // namespace

// -----------------------------------------------------------------------------
// Primitive Donner types
// -----------------------------------------------------------------------------

void EncodeVector2d(WireWriter& w, const Vector2d& v) {
  w.writeF64(v.x);
  w.writeF64(v.y);
}
bool DecodeVector2d(WireReader& r, Vector2d& out) {
  return r.readF64(out.x) && r.readF64(out.y);
}

void EncodeVector2i(WireWriter& w, const Vector2i& v) {
  w.writeI32(v.x);
  w.writeI32(v.y);
}
bool DecodeVector2i(WireReader& r, Vector2i& out) {
  return r.readI32(out.x) && r.readI32(out.y);
}

void EncodeTransform2d(WireWriter& w, const Transform2d& t) {
  for (int i = 0; i < 6; ++i) {
    w.writeF64(t.data[i]);
  }
}
bool DecodeTransform2d(WireReader& r, Transform2d& out) {
  for (int i = 0; i < 6; ++i) {
    if (!r.readF64(out.data[i])) return false;
  }
  return true;
}

void EncodeBox2d(WireWriter& w, const Box2d& b) {
  EncodeVector2d(w, b.topLeft);
  EncodeVector2d(w, b.bottomRight);
}
bool DecodeBox2d(WireReader& r, Box2d& out) {
  return DecodeVector2d(r, out.topLeft) && DecodeVector2d(r, out.bottomRight);
}

void EncodeRgba(WireWriter& w, const css::RGBA& rgba) {
  w.writeU8(rgba.r);
  w.writeU8(rgba.g);
  w.writeU8(rgba.b);
  w.writeU8(rgba.a);
}
bool DecodeRgba(WireReader& r, css::RGBA& out) {
  return r.readU8(out.r) && r.readU8(out.g) && r.readU8(out.b) && r.readU8(out.a);
}

void EncodeColor(WireWriter& w, const css::Color& color) {
  // S2: everything gets flattened to an RGBA variant on the wire. A `u8` tag
  // precedes the RGBA so future versions can reintroduce HSLA/CurrentColor
  // without breaking older readers.
  css::RGBA rgba = css::RGBA();
  if (std::holds_alternative<css::RGBA>(color.value)) {
    rgba = std::get<css::RGBA>(color.value);
  } else if (std::holds_alternative<css::HSLA>(color.value)) {
    rgba = std::get<css::HSLA>(color.value).toRGBA();
  }
  // CurrentColor is not resolvable here — fallback to fully-transparent, which
  // matches Donner's CSS-side semantic of "currentColor with no context".

  w.writeU8(1);  // tag: RGBA
  EncodeRgba(w, rgba);
}
bool DecodeColor(WireReader& r, css::Color& out) {
  uint8_t tag = 0;
  if (!r.readU8(tag)) return false;
  if (tag != 1) {
    r.fail();
    return false;
  }
  css::RGBA rgba;
  if (!DecodeRgba(r, rgba)) return false;
  out = css::Color(rgba);
  return true;
}

// -----------------------------------------------------------------------------
// Enums
// -----------------------------------------------------------------------------

void EncodeFillRule(WireWriter& w, FillRule v) { w.writeU8(static_cast<uint8_t>(v)); }
bool DecodeFillRule(WireReader& r, FillRule& out) {
  uint8_t v = 0;
  if (!r.readU8(v)) return false;
  if (v > static_cast<uint8_t>(FillRule::EvenOdd)) {
    r.fail();
    return false;
  }
  out = static_cast<FillRule>(v);
  return true;
}

void EncodeMixBlendMode(WireWriter& w, svg::MixBlendMode v) {
  w.writeU8(static_cast<uint8_t>(v));
}
bool DecodeMixBlendMode(WireReader& r, svg::MixBlendMode& out) {
  uint8_t v = 0;
  if (!r.readU8(v)) return false;
  if (v > static_cast<uint8_t>(svg::MixBlendMode::Luminosity)) {
    r.fail();
    return false;
  }
  out = static_cast<svg::MixBlendMode>(v);
  return true;
}

void EncodeStrokeLinecap(WireWriter& w, svg::StrokeLinecap v) {
  w.writeU8(static_cast<uint8_t>(v));
}
bool DecodeStrokeLinecap(WireReader& r, svg::StrokeLinecap& out) {
  uint8_t v = 0;
  if (!r.readU8(v)) return false;
  if (v > static_cast<uint8_t>(svg::StrokeLinecap::Square)) {
    r.fail();
    return false;
  }
  out = static_cast<svg::StrokeLinecap>(v);
  return true;
}

void EncodeStrokeLinejoin(WireWriter& w, svg::StrokeLinejoin v) {
  w.writeU8(static_cast<uint8_t>(v));
}
bool DecodeStrokeLinejoin(WireReader& r, svg::StrokeLinejoin& out) {
  uint8_t v = 0;
  if (!r.readU8(v)) return false;
  if (v > static_cast<uint8_t>(svg::StrokeLinejoin::Arcs)) {
    r.fail();
    return false;
  }
  out = static_cast<svg::StrokeLinejoin>(v);
  return true;
}

// -----------------------------------------------------------------------------
// Path and stroke
// -----------------------------------------------------------------------------

void EncodePath(WireWriter& w, const Path& path) {
  const auto commands = path.commands();
  const auto points = path.points();

  w.writeU32(static_cast<uint32_t>(commands.size()));
  for (const auto& cmd : commands) {
    w.writeU8(static_cast<uint8_t>(cmd.verb));
  }

  w.writeU32(static_cast<uint32_t>(points.size()));
  for (const auto& p : points) {
    EncodeVector2d(w, p);
  }
}

bool DecodePath(WireReader& r, Path& out) {
  uint32_t commandCount = 0;
  if (!r.readCount(commandCount, kMaxPathCommands)) return false;

  std::vector<Path::Verb> verbs;
  verbs.reserve(commandCount);
  std::size_t expectedPoints = 0;
  for (uint32_t i = 0; i < commandCount; ++i) {
    uint8_t v = 0;
    if (!r.readU8(v)) return false;
    if (v > static_cast<uint8_t>(Path::Verb::ClosePath)) {
      r.fail();
      return false;
    }
    const auto verb = static_cast<Path::Verb>(v);
    verbs.push_back(verb);
    expectedPoints += PointsPerVerb(verb);
  }

  uint32_t pointCount = 0;
  if (!r.readCount(pointCount, kMaxPathPoints)) return false;
  if (pointCount != expectedPoints) {
    r.fail();
    return false;
  }

  std::vector<Vector2d> points;
  points.reserve(pointCount);
  for (uint32_t i = 0; i < pointCount; ++i) {
    Vector2d p;
    if (!DecodeVector2d(r, p)) return false;
    points.push_back(p);
  }

  if (!BuildPathFromVerbs(verbs, points, out)) {
    r.fail();
    return false;
  }
  return true;
}

void EncodeStrokeParams(WireWriter& w, const svg::StrokeParams& s) {
  w.writeF64(s.strokeWidth);
  EncodeStrokeLinecap(w, s.lineCap);
  EncodeStrokeLinejoin(w, s.lineJoin);
  w.writeF64(s.miterLimit);

  w.writeU32(static_cast<uint32_t>(s.dashArray.size()));
  for (const double d : s.dashArray) {
    w.writeF64(d);
  }
  w.writeF64(s.dashOffset);
  w.writeF64(s.pathLength);
}

bool DecodeStrokeParams(WireReader& r, svg::StrokeParams& out) {
  if (!r.readF64(out.strokeWidth)) return false;
  if (!DecodeStrokeLinecap(r, out.lineCap)) return false;
  if (!DecodeStrokeLinejoin(r, out.lineJoin)) return false;
  if (!r.readF64(out.miterLimit)) return false;

  uint32_t count = 0;
  if (!r.readCount(count, kMaxDashArrayLen)) return false;
  out.dashArray.clear();
  out.dashArray.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    double v = 0;
    if (!r.readF64(v)) return false;
    out.dashArray.push_back(v);
  }
  if (!r.readF64(out.dashOffset)) return false;
  if (!r.readF64(out.pathLength)) return false;
  return true;
}

void EncodePathShape(WireWriter& w, const svg::PathShape& p) {
  EncodePath(w, p.path);
  EncodeFillRule(w, p.fillRule);
  EncodeTransform2d(w, p.entityFromParent);
  w.writeI32(p.layer);
}

bool DecodePathShape(WireReader& r, svg::PathShape& out) {
  if (!DecodePath(r, out.path)) return false;
  if (!DecodeFillRule(r, out.fillRule)) return false;
  if (!DecodeTransform2d(r, out.entityFromParent)) return false;
  if (!r.readI32(out.layer)) return false;
  return true;
}

// -----------------------------------------------------------------------------
// Paint and clip
// -----------------------------------------------------------------------------

namespace {
// Tags for the ResolvedPaintServer variant on the wire.
constexpr uint8_t kPaintTagNone = 0;
constexpr uint8_t kPaintTagSolid = 1;
constexpr uint8_t kPaintTagStub = 2;      // unsupported alternative — decode as None
constexpr uint8_t kPaintTagGradient = 3;  // WIRE.5: linear/radial gradient

constexpr uint32_t kMaxGradientStops = 4096;
}  // namespace

void EncodeLengthd(WireWriter& w, const Lengthd& v) {
  w.writeF64(v.value);
  w.writeU8(static_cast<uint8_t>(v.unit));
}
bool DecodeLengthd(WireReader& r, Lengthd& out) {
  double value = 0;
  uint8_t unit = 0;
  if (!r.readF64(value)) return false;
  if (!r.readU8(unit)) return false;
  if (unit > static_cast<uint8_t>(LengthUnit::Vmax)) {
    r.fail();
    return false;
  }
  out = Lengthd(value, static_cast<LengthUnit>(unit));
  return true;
}

void EncodeWireGradient(WireWriter& w, const WireGradient& g) {
  w.writeU8(static_cast<uint8_t>(g.kind));
  w.writeU8(static_cast<uint8_t>(g.units));
  w.writeU8(static_cast<uint8_t>(g.spreadMethod));

  // Stops: count + each stop as (f32 offset, u32 rgba, f32 opacity).
  w.writeU32(static_cast<uint32_t>(g.stops.size()));
  for (const auto& stop : g.stops) {
    // GradientStop.offset is float, not double.
    const double offset64 = static_cast<double>(stop.offset);
    w.writeF64(offset64);
    css::RGBA rgba;
    if (std::holds_alternative<css::RGBA>(stop.color.value)) {
      rgba = std::get<css::RGBA>(stop.color.value);
    } else if (std::holds_alternative<css::HSLA>(stop.color.value)) {
      rgba = std::get<css::HSLA>(stop.color.value).toRGBA();
    }
    EncodeRgba(w, rgba);
    w.writeF64(static_cast<double>(stop.opacity));
  }

  // Geometry — both linear and radial fields are always present. Wasting a
  // few bytes is cheaper than branching the wire layout and makes the decoder
  // trivially bounds-checkable.
  EncodeLengthd(w, g.x1);
  EncodeLengthd(w, g.y1);
  EncodeLengthd(w, g.x2);
  EncodeLengthd(w, g.y2);

  EncodeLengthd(w, g.cx);
  EncodeLengthd(w, g.cy);
  EncodeLengthd(w, g.r);
  w.writeBool(g.fx.has_value());
  if (g.fx) EncodeLengthd(w, *g.fx);
  w.writeBool(g.fy.has_value());
  if (g.fy) EncodeLengthd(w, *g.fy);
  EncodeLengthd(w, g.fr);

  // Fallback color — matches the PaintResolvedReference structure.
  w.writeBool(g.fallback.has_value());
  if (g.fallback) EncodeColor(w, *g.fallback);
}

bool DecodeWireGradient(WireReader& r, WireGradient& out) {
  uint8_t kind = 0;
  if (!r.readU8(kind)) return false;
  if (kind > static_cast<uint8_t>(WireGradient::Kind::kRadial)) {
    r.fail();
    return false;
  }
  out.kind = static_cast<WireGradient::Kind>(kind);

  uint8_t units = 0;
  if (!r.readU8(units)) return false;
  if (units > static_cast<uint8_t>(svg::GradientUnits::ObjectBoundingBox)) {
    r.fail();
    return false;
  }
  out.units = static_cast<svg::GradientUnits>(units);

  uint8_t spread = 0;
  if (!r.readU8(spread)) return false;
  if (spread > static_cast<uint8_t>(svg::GradientSpreadMethod::Repeat)) {
    r.fail();
    return false;
  }
  out.spreadMethod = static_cast<svg::GradientSpreadMethod>(spread);

  uint32_t stopCount = 0;
  if (!r.readCount(stopCount, kMaxGradientStops)) return false;
  out.stops.clear();
  out.stops.reserve(stopCount);
  for (uint32_t i = 0; i < stopCount; ++i) {
    svg::GradientStop stop;
    double offset = 0;
    if (!r.readF64(offset)) return false;
    stop.offset = static_cast<float>(offset);
    css::RGBA rgba;
    if (!DecodeRgba(r, rgba)) return false;
    stop.color = css::Color(rgba);
    double opacity = 0;
    if (!r.readF64(opacity)) return false;
    stop.opacity = static_cast<float>(opacity);
    out.stops.push_back(std::move(stop));
  }

  if (!DecodeLengthd(r, out.x1)) return false;
  if (!DecodeLengthd(r, out.y1)) return false;
  if (!DecodeLengthd(r, out.x2)) return false;
  if (!DecodeLengthd(r, out.y2)) return false;

  if (!DecodeLengthd(r, out.cx)) return false;
  if (!DecodeLengthd(r, out.cy)) return false;
  if (!DecodeLengthd(r, out.r)) return false;
  bool hasFx = false;
  if (!r.readBool(hasFx)) return false;
  if (hasFx) {
    Lengthd fx;
    if (!DecodeLengthd(r, fx)) return false;
    out.fx = fx;
  } else {
    out.fx.reset();
  }
  bool hasFy = false;
  if (!r.readBool(hasFy)) return false;
  if (hasFy) {
    Lengthd fy;
    if (!DecodeLengthd(r, fy)) return false;
    out.fy = fy;
  } else {
    out.fy.reset();
  }
  if (!DecodeLengthd(r, out.fr)) return false;

  bool hasFallback = false;
  if (!r.readBool(hasFallback)) return false;
  if (hasFallback) {
    css::Color color(css::RGBA{});
    if (!DecodeColor(r, color)) return false;
    out.fallback = color;
  } else {
    out.fallback.reset();
  }
  return true;
}

namespace {

/// Attempts to flatten a PaintResolvedReference's gradient components into a
/// self-contained `WireGradient`. Returns `std::nullopt` if the reference
/// doesn't resolve to a gradient (e.g., it's a pattern). The caller is the
/// serializer side, which has live access to the sandbox-side registry, so
/// `handle.try_get<...>()` is safe.
std::optional<WireGradient> FlattenGradientReference(
    const svg::components::PaintResolvedReference& ref) {
  const auto& handle = ref.reference.handle;
  const auto* gradient =
      handle.try_get<svg::components::ComputedGradientComponent>();
  if (gradient == nullptr || !gradient->initialized) {
    return std::nullopt;
  }

  WireGradient out;
  out.units = gradient->gradientUnits;
  out.spreadMethod = gradient->spreadMethod;
  out.stops = gradient->stops;
  out.fallback = ref.fallback;

  if (const auto* linear =
          handle.try_get<svg::components::ComputedLinearGradientComponent>()) {
    out.kind = WireGradient::Kind::kLinear;
    out.x1 = linear->x1;
    out.y1 = linear->y1;
    out.x2 = linear->x2;
    out.y2 = linear->y2;
    return out;
  }
  if (const auto* radial =
          handle.try_get<svg::components::ComputedRadialGradientComponent>()) {
    out.kind = WireGradient::Kind::kRadial;
    out.cx = radial->cx;
    out.cy = radial->cy;
    out.r = radial->r;
    out.fx = radial->fx;
    out.fy = radial->fy;
    out.fr = radial->fr;
    return out;
  }
  return std::nullopt;
}

}  // namespace

void EncodeResolvedPaintServer(WireWriter& w,
                               const svg::components::ResolvedPaintServer& p) {
  if (std::holds_alternative<svg::PaintServer::None>(p)) {
    w.writeU8(kPaintTagNone);
    return;
  }
  if (std::holds_alternative<svg::PaintServer::Solid>(p)) {
    const auto& solid = std::get<svg::PaintServer::Solid>(p);
    w.writeU8(kPaintTagSolid);
    EncodeColor(w, solid.color);
    return;
  }
  if (std::holds_alternative<svg::components::PaintResolvedReference>(p)) {
    const auto& ref = std::get<svg::components::PaintResolvedReference>(p);
    if (auto gradient = FlattenGradientReference(ref)) {
      w.writeU8(kPaintTagGradient);
      EncodeWireGradient(w, *gradient);
      return;
    }
    // Fall through: unresolvable or pattern — emit a stub. The downstream
    // setPaint will decode to None; pattern rendering continues to work via
    // RendererTinySkia's patternFillPaint_ side channel.
  }
  w.writeU8(kPaintTagStub);
}

bool DecodeResolvedPaintServer(WireReader& r,
                               svg::components::ResolvedPaintServer& out,
                               std::optional<WireGradient>* outPendingGradient) {
  uint8_t tag = 0;
  if (!r.readU8(tag)) return false;
  switch (tag) {
    case kPaintTagNone:
      out = svg::PaintServer::None{};
      return true;
    case kPaintTagSolid: {
      css::Color color(css::RGBA{});
      if (!DecodeColor(r, color)) return false;
      out = svg::PaintServer::Solid(color);
      return true;
    }
    case kPaintTagStub:
      // Decode as transparent-solid rather than None. This matters for
      // patterns: RendererTinySkia::makeFillPaint() returns early on None
      // (line 2105) before checking patternFillPaint_ (line 2112). A
      // transparent-solid passes the None check, letting the pattern side
      // channel take effect. For non-pattern stubs, transparent-solid is
      // visually identical to None on a transparent-initialized canvas.
      out = svg::PaintServer::Solid(css::Color(css::RGBA(0, 0, 0, 0)));
      return true;
    case kPaintTagGradient: {
      WireGradient gradient;
      if (!DecodeWireGradient(r, gradient)) return false;
      if (outPendingGradient != nullptr) {
        *outPendingGradient = std::move(gradient);
      }
      // The actual `PaintResolvedReference` is materialized by the replayer
      // before the decoded PaintParams is forwarded to the backend. Until
      // then, we hold a placeholder `None` so the variant stays valid.
      out = svg::PaintServer::None{};
      return true;
    }
    default:
      r.fail();
      return false;
  }
}

void EncodePaintParams(WireWriter& w, const svg::PaintParams& p) {
  w.writeF64(p.opacity);
  EncodeResolvedPaintServer(w, p.fill);
  EncodeResolvedPaintServer(w, p.stroke);
  w.writeF64(p.fillOpacity);
  w.writeF64(p.strokeOpacity);
  EncodeColor(w, p.currentColor);
  EncodeBox2d(w, p.viewBox);
  EncodeStrokeParams(w, p.strokeParams);
}

bool DecodePaintParams(WireReader& r, svg::PaintParams& out,
                       std::optional<WireGradient>* outFillGradient,
                       std::optional<WireGradient>* outStrokeGradient) {
  if (!r.readF64(out.opacity)) return false;
  if (!DecodeResolvedPaintServer(r, out.fill, outFillGradient)) return false;
  if (!DecodeResolvedPaintServer(r, out.stroke, outStrokeGradient)) return false;
  if (!r.readF64(out.fillOpacity)) return false;
  if (!r.readF64(out.strokeOpacity)) return false;
  if (!DecodeColor(r, out.currentColor)) return false;
  if (!DecodeBox2d(r, out.viewBox)) return false;
  if (!DecodeStrokeParams(r, out.strokeParams)) return false;
  return true;
}

void EncodeResolvedClip(WireWriter& w, const svg::ResolvedClip& c) {
  w.writeBool(c.clipRect.has_value());
  if (c.clipRect) {
    EncodeBox2d(w, *c.clipRect);
  }

  w.writeU32(static_cast<uint32_t>(c.clipPaths.size()));
  for (const auto& shape : c.clipPaths) {
    EncodePathShape(w, shape);
  }

  EncodeTransform2d(w, c.clipPathUnitsTransform);
  // ResolvedMask: always encoded as absent. Masks hit the unsupported path
  // before ever reaching this encoder in the SerializingRenderer; if one
  // slips through we deliberately drop it instead of corrupting the stream.
  w.writeBool(false);
}

bool DecodeResolvedClip(WireReader& r, svg::ResolvedClip& out) {
  bool hasRect = false;
  if (!r.readBool(hasRect)) return false;
  if (hasRect) {
    Box2d rect;
    if (!DecodeBox2d(r, rect)) return false;
    out.clipRect = rect;
  } else {
    out.clipRect.reset();
  }

  uint32_t pathCount = 0;
  if (!r.readCount(pathCount, kMaxClipPaths)) return false;
  out.clipPaths.clear();
  out.clipPaths.reserve(pathCount);
  for (uint32_t i = 0; i < pathCount; ++i) {
    svg::PathShape shape;
    if (!DecodePathShape(r, shape)) return false;
    out.clipPaths.push_back(std::move(shape));
  }

  if (!DecodeTransform2d(r, out.clipPathUnitsTransform)) return false;

  bool hasMask = false;
  if (!r.readBool(hasMask)) return false;
  // Mask payload is never written, so hasMask==true on the wire is a bug.
  if (hasMask) {
    r.fail();
    return false;
  }
  out.mask.reset();
  return true;
}

// -----------------------------------------------------------------------------
// Misc
// -----------------------------------------------------------------------------

void EncodeRenderViewport(WireWriter& w, const svg::RenderViewport& v) {
  EncodeVector2d(w, v.size);
  w.writeF64(v.devicePixelRatio);
}
bool DecodeRenderViewport(WireReader& r, svg::RenderViewport& out) {
  if (!DecodeVector2d(r, out.size)) return false;
  if (!r.readF64(out.devicePixelRatio)) return false;
  return true;
}

void EncodeImageParams(WireWriter& w, const svg::ImageParams& p) {
  EncodeBox2d(w, p.targetRect);
  w.writeF64(p.opacity);
  w.writeBool(p.imageRenderingPixelated);
}
bool DecodeImageParams(WireReader& r, svg::ImageParams& out) {
  if (!DecodeBox2d(r, out.targetRect)) return false;
  if (!r.readF64(out.opacity)) return false;
  if (!r.readBool(out.imageRenderingPixelated)) return false;
  return true;
}

namespace {
// 512 MB hard cap on inline image data — any real SVG image is tiny
// compared to this. Anything larger is a protocol violation.
constexpr uint32_t kMaxImageBytes = 512u * 1024u * 1024u;
}  // namespace

void EncodeImageResource(WireWriter& w, const svg::ImageResource& img) {
  w.writeI32(img.width);
  w.writeI32(img.height);
  w.writeU32(static_cast<uint32_t>(img.data.size()));
  w.writeBytes(std::span<const uint8_t>(img.data));
}

bool DecodeImageResource(WireReader& r, svg::ImageResource& out) {
  if (!r.readI32(out.width)) return false;
  if (!r.readI32(out.height)) return false;
  if (out.width < 0 || out.height < 0) {
    r.fail();
    return false;
  }
  uint32_t byteLen = 0;
  if (!r.readCount(byteLen, kMaxImageBytes)) return false;
  out.data.resize(byteLen);
  if (byteLen > 0 && !r.readBytes(std::span<uint8_t>(out.data))) return false;
  return true;
}

// ---------------------------------------------------------------------------
// Text: TextParams + ComputedTextComponent
// ---------------------------------------------------------------------------

namespace {
constexpr uint32_t kMaxTextSpans = 10'000;
constexpr uint32_t kMaxPositionList = 100'000;
constexpr uint32_t kMaxFontFamilies = 64;
constexpr uint32_t kMaxBaselineShifts = 256;
constexpr uint32_t kMaxFontFaces = 256;
constexpr uint32_t kMaxFontFaceSources = 16;
constexpr uint32_t kMaxFontDataBytes = 50u * 1024u * 1024u;  // 50 MB
constexpr uint32_t kMaxTechHints = 32;

void EncodeOptionalLengthd(WireWriter& w, const std::optional<Lengthd>& v) {
  w.writeBool(v.has_value());
  if (v) EncodeLengthd(w, *v);
}
bool DecodeOptionalLengthd(WireReader& r, std::optional<Lengthd>& out) {
  bool present = false;
  if (!r.readBool(present)) return false;
  if (present) {
    Lengthd v;
    if (!DecodeLengthd(r, v)) return false;
    out = v;
  } else {
    out.reset();
  }
  return true;
}

template <typename T>
void EncodeSmallVecOptLengthd(WireWriter& w, const T& vec) {
  w.writeU32(static_cast<uint32_t>(vec.size()));
  for (const auto& v : vec) {
    EncodeOptionalLengthd(w, v);
  }
}
template <typename T>
bool DecodeSmallVecOptLengthd(WireReader& r, T& out, uint32_t maxCount) {
  uint32_t count = 0;
  if (!r.readCount(count, maxCount)) return false;
  out.clear();
  for (uint32_t i = 0; i < count; ++i) {
    std::optional<Lengthd> v;
    if (!DecodeOptionalLengthd(r, v)) return false;
    out.push_back(std::move(v));
  }
  return true;
}

void EncodeTextSpan(WireWriter& w, const svg::components::ComputedTextComponent::TextSpan& s) {
  // Text content
  w.writeString(std::string_view(s.text));
  w.writeU32(static_cast<uint32_t>(s.start));
  w.writeU32(static_cast<uint32_t>(s.end));

  // Positioning lists
  EncodeSmallVecOptLengthd(w, s.xList);
  EncodeSmallVecOptLengthd(w, s.yList);
  EncodeSmallVecOptLengthd(w, s.dxList);
  EncodeSmallVecOptLengthd(w, s.dyList);

  w.writeU32(static_cast<uint32_t>(s.rotateList.size()));
  for (const double v : s.rotateList) {
    w.writeF64(v);
  }

  // Paint servers (reuse existing codec)
  EncodeResolvedPaintServer(w, s.resolvedFill);
  EncodeResolvedPaintServer(w, s.resolvedStroke);

  // Scalar style fields
  w.writeF64(s.opacity);
  w.writeF64(s.fillOpacity);
  w.writeF64(s.strokeOpacity);
  EncodeLengthd(w, s.fontSize);
  w.writeI32(s.fontWeight);
  w.writeU8(static_cast<uint8_t>(s.fontStyle));
  w.writeU8(static_cast<uint8_t>(s.fontStretch));
  w.writeU8(static_cast<uint8_t>(s.fontVariant));
  w.writeF64(s.strokeWidth);
  w.writeF64(s.strokeMiterLimit);
  EncodeStrokeLinecap(w, s.strokeLinecap);
  EncodeStrokeLinejoin(w, s.strokeLinejoin);

  // Path (optional)
  w.writeBool(s.pathSpline.has_value());
  if (s.pathSpline) EncodePath(w, *s.pathSpline);

  // Baseline shifts
  w.writeU32(static_cast<uint32_t>(s.ancestorBaselineShifts.size()));
  for (const auto& shift : s.ancestorBaselineShifts) {
    using K = svg::components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;
    w.writeU8(static_cast<uint8_t>(shift.keyword));
    EncodeLengthd(w, shift.shift);
    w.writeF64(shift.fontSizePx);
  }
  w.writeU8(static_cast<uint8_t>(s.baselineShiftKeyword));
  EncodeLengthd(w, s.baselineShift);

  // Enum fields
  w.writeU8(static_cast<uint8_t>(s.textAnchor));
  w.writeU8(static_cast<uint8_t>(s.visibility));
  w.writeU8(static_cast<uint8_t>(s.textDecoration));
  w.writeU8(static_cast<uint8_t>(s.alignmentBaseline));

  // Spacing
  w.writeF64(s.letterSpacingPx);
  w.writeF64(s.wordSpacingPx);
  EncodeOptionalLengthd(w, s.textLength);
  w.writeU8(static_cast<uint8_t>(s.lengthAdjust));

  // Booleans
  w.writeBool(s.startsNewChunk);
  w.writeBool(s.hidden);
  w.writeBool(s.textPathFailed);

  w.writeF64(s.pathStartOffset);

  // Decoration paint/styling
  EncodeResolvedPaintServer(w, s.resolvedDecorationFill);
  EncodeResolvedPaintServer(w, s.resolvedDecorationStroke);
  w.writeF64(s.decorationFillOpacity);
  w.writeF64(s.decorationStrokeOpacity);
  w.writeF64(s.decorationStrokeWidth);
  w.writeF64(static_cast<double>(s.decorationFontSizePx));
  w.writeI32(s.decorationDeclarationCount);
}

bool DecodeTextSpan(WireReader& r,
                    svg::components::ComputedTextComponent::TextSpan& s) {
  std::string text;
  if (!r.readString(text)) return false;
  s.text = RcString(text);
  uint32_t start = 0, end = 0;
  if (!r.readU32(start) || !r.readU32(end)) return false;
  s.start = start;
  s.end = end;

  if (!DecodeSmallVecOptLengthd(r, s.xList, kMaxPositionList)) return false;
  if (!DecodeSmallVecOptLengthd(r, s.yList, kMaxPositionList)) return false;
  if (!DecodeSmallVecOptLengthd(r, s.dxList, kMaxPositionList)) return false;
  if (!DecodeSmallVecOptLengthd(r, s.dyList, kMaxPositionList)) return false;

  uint32_t rotCount = 0;
  if (!r.readCount(rotCount, kMaxPositionList)) return false;
  s.rotateList.clear();
  for (uint32_t i = 0; i < rotCount; ++i) {
    double v = 0;
    if (!r.readF64(v)) return false;
    s.rotateList.push_back(v);
  }

  if (!DecodeResolvedPaintServer(r, s.resolvedFill)) return false;
  if (!DecodeResolvedPaintServer(r, s.resolvedStroke)) return false;

  if (!r.readF64(s.opacity)) return false;
  if (!r.readF64(s.fillOpacity)) return false;
  if (!r.readF64(s.strokeOpacity)) return false;
  if (!DecodeLengthd(r, s.fontSize)) return false;
  if (!r.readI32(s.fontWeight)) return false;
  uint8_t u = 0;
  if (!r.readU8(u)) return false; s.fontStyle = static_cast<svg::FontStyle>(u);
  if (!r.readU8(u)) return false; s.fontStretch = static_cast<svg::FontStretch>(u);
  if (!r.readU8(u)) return false; s.fontVariant = static_cast<svg::FontVariant>(u);
  if (!r.readF64(s.strokeWidth)) return false;
  if (!r.readF64(s.strokeMiterLimit)) return false;
  if (!DecodeStrokeLinecap(r, s.strokeLinecap)) return false;
  if (!DecodeStrokeLinejoin(r, s.strokeLinejoin)) return false;

  bool hasPath = false;
  if (!r.readBool(hasPath)) return false;
  if (hasPath) {
    Path p;
    if (!DecodePath(r, p)) return false;
    s.pathSpline = std::move(p);
  } else {
    s.pathSpline.reset();
  }

  uint32_t shiftCount = 0;
  if (!r.readCount(shiftCount, kMaxBaselineShifts)) return false;
  s.ancestorBaselineShifts.clear();
  using AncestorShift = svg::components::ComputedTextComponent::TextSpan::AncestorShift;
  using K = svg::components::ComputedTextComponent::TextSpan::BaselineShiftKeyword;
  for (uint32_t i = 0; i < shiftCount; ++i) {
    AncestorShift shift;
    if (!r.readU8(u)) return false;
    shift.keyword = static_cast<K>(u);
    if (!DecodeLengthd(r, shift.shift)) return false;
    if (!r.readF64(shift.fontSizePx)) return false;
    s.ancestorBaselineShifts.push_back(shift);
  }
  if (!r.readU8(u)) return false; s.baselineShiftKeyword = static_cast<K>(u);
  if (!DecodeLengthd(r, s.baselineShift)) return false;

  if (!r.readU8(u)) return false; s.textAnchor = static_cast<svg::TextAnchor>(u);
  if (!r.readU8(u)) return false; s.visibility = static_cast<svg::Visibility>(u);
  if (!r.readU8(u)) return false; s.textDecoration = static_cast<svg::TextDecoration>(u);
  if (!r.readU8(u)) return false; s.alignmentBaseline = static_cast<svg::DominantBaseline>(u);

  if (!r.readF64(s.letterSpacingPx)) return false;
  if (!r.readF64(s.wordSpacingPx)) return false;
  if (!DecodeOptionalLengthd(r, s.textLength)) return false;
  if (!r.readU8(u)) return false; s.lengthAdjust = static_cast<svg::LengthAdjust>(u);

  if (!r.readBool(s.startsNewChunk)) return false;
  if (!r.readBool(s.hidden)) return false;
  if (!r.readBool(s.textPathFailed)) return false;

  if (!r.readF64(s.pathStartOffset)) return false;

  if (!DecodeResolvedPaintServer(r, s.resolvedDecorationFill)) return false;
  if (!DecodeResolvedPaintServer(r, s.resolvedDecorationStroke)) return false;
  if (!r.readF64(s.decorationFillOpacity)) return false;
  if (!r.readF64(s.decorationStrokeOpacity)) return false;
  if (!r.readF64(s.decorationStrokeWidth)) return false;
  double decFontSize = 0;
  if (!r.readF64(decFontSize)) return false;
  s.decorationFontSizePx = static_cast<float>(decFontSize);
  if (!r.readI32(s.decorationDeclarationCount)) return false;

  // Entities are not transferred across the wire boundary. The replayer
  // doesn't have the original document's registry, so entity handles are
  // meaningless. Set to null — the renderer will re-derive as needed.
  s.sourceEntity = entt::null;
  s.textPathSourceEntity = entt::null;
  return true;
}

void EncodeFontFaceSource(WireWriter& w, const css::FontFaceSource& src) {
  w.writeU8(static_cast<uint8_t>(src.kind));
  if (src.kind == css::FontFaceSource::Kind::Data) {
    // Data payload: write the byte vector inline.
    const auto* dataPtr =
        std::get_if<std::shared_ptr<const std::vector<uint8_t>>>(&src.payload);
    if (dataPtr && *dataPtr) {
      const auto& bytes = **dataPtr;
      w.writeU32(static_cast<uint32_t>(bytes.size()));
      w.writeBytes(std::span<const uint8_t>(bytes));
    } else {
      w.writeU32(0);
    }
  } else {
    // Local or Url: write the string payload.
    const auto* strPtr = std::get_if<RcString>(&src.payload);
    if (strPtr) {
      w.writeString(std::string_view(*strPtr));
    } else {
      w.writeString(std::string_view{});
    }
  }
  w.writeString(std::string_view(src.formatHint));
  w.writeU32(static_cast<uint32_t>(src.techHints.size()));
  for (const auto& hint : src.techHints) {
    w.writeString(std::string_view(hint));
  }
}

bool DecodeFontFaceSource(WireReader& r, css::FontFaceSource& out) {
  uint8_t kindU8 = 0;
  if (!r.readU8(kindU8)) return false;
  if (kindU8 > static_cast<uint8_t>(css::FontFaceSource::Kind::Data)) {
    return false;
  }
  out.kind = static_cast<css::FontFaceSource::Kind>(kindU8);

  if (out.kind == css::FontFaceSource::Kind::Data) {
    uint32_t dataLen = 0;
    if (!r.readCount(dataLen, kMaxFontDataBytes)) return false;
    auto dataVec = std::make_shared<std::vector<uint8_t>>(dataLen);
    if (dataLen > 0) {
      if (!r.readBytes(std::span<uint8_t>(dataVec->data(), dataLen))) return false;
    }
    out.payload = std::move(dataVec);
  } else {
    std::string str;
    if (!r.readString(str)) return false;
    out.payload = RcString(str);
  }

  std::string formatHint;
  if (!r.readString(formatHint)) return false;
  out.formatHint = RcString(formatHint);

  uint32_t techCount = 0;
  if (!r.readCount(techCount, kMaxTechHints)) return false;
  out.techHints.clear();
  out.techHints.reserve(techCount);
  for (uint32_t i = 0; i < techCount; ++i) {
    std::string hint;
    if (!r.readString(hint)) return false;
    out.techHints.push_back(RcString(hint));
  }
  return true;
}

}  // namespace

void EncodeFontFace(WireWriter& w, const css::FontFace& face) {
  w.writeString(std::string_view(face.familyName));
  w.writeI32(face.fontWeight);
  w.writeI32(face.fontStyle);
  w.writeI32(face.fontStretch);
  w.writeU32(static_cast<uint32_t>(face.sources.size()));
  for (const auto& src : face.sources) {
    EncodeFontFaceSource(w, src);
  }
}

bool DecodeFontFace(WireReader& r, css::FontFace& out) {
  std::string familyName;
  if (!r.readString(familyName)) return false;
  out.familyName = RcString(familyName);
  if (!r.readI32(out.fontWeight)) return false;
  if (!r.readI32(out.fontStyle)) return false;
  if (!r.readI32(out.fontStretch)) return false;
  uint32_t srcCount = 0;
  if (!r.readCount(srcCount, kMaxFontFaceSources)) return false;
  out.sources.clear();
  out.sources.reserve(srcCount);
  for (uint32_t i = 0; i < srcCount; ++i) {
    css::FontFaceSource src;
    if (!DecodeFontFaceSource(r, src)) return false;
    out.sources.push_back(std::move(src));
  }
  return true;
}

void EncodeTextParams(WireWriter& w, const svg::TextParams& p) {
  w.writeF64(p.opacity);
  EncodeColor(w, p.fillColor);
  EncodeColor(w, p.strokeColor);
  EncodeStrokeParams(w, p.strokeParams);

  w.writeU32(static_cast<uint32_t>(p.fontFamilies.size()));
  for (const auto& fam : p.fontFamilies) {
    w.writeString(std::string_view(fam));
  }

  EncodeLengthd(w, p.fontSize);
  EncodeBox2d(w, p.viewBox);

  // FontMetrics
  w.writeF64(p.fontMetrics.fontSize);
  w.writeF64(p.fontMetrics.rootFontSize);
  w.writeF64(p.fontMetrics.exUnitInEm);
  w.writeF64(p.fontMetrics.chUnitInEm);
  w.writeBool(p.fontMetrics.viewportSize.has_value());
  if (p.fontMetrics.viewportSize) EncodeVector2d(w, *p.fontMetrics.viewportSize);

  w.writeU8(static_cast<uint8_t>(p.textAnchor));
  w.writeU8(static_cast<uint8_t>(p.textDecoration));
  w.writeU8(static_cast<uint8_t>(p.dominantBaseline));
  w.writeU8(static_cast<uint8_t>(p.writingMode));
  w.writeF64(p.letterSpacingPx);
  w.writeF64(p.wordSpacingPx);
  EncodeOptionalLengthd(w, p.textLength);
  w.writeU8(static_cast<uint8_t>(p.lengthAdjust));

  // Encode fontFaces with full inline font data.
  w.writeU32(static_cast<uint32_t>(p.fontFaces.size()));
  for (const auto& face : p.fontFaces) {
    EncodeFontFace(w, face);
  }
  // textRootEntity is a local entity handle — meaningless on the replay side.
}

bool DecodeTextParams(WireReader& r, svg::TextParams& out,
                      std::vector<css::FontFace>* outFontFaces) {
  if (!r.readF64(out.opacity)) return false;
  if (!DecodeColor(r, out.fillColor)) return false;
  if (!DecodeColor(r, out.strokeColor)) return false;
  if (!DecodeStrokeParams(r, out.strokeParams)) return false;

  uint32_t famCount = 0;
  if (!r.readCount(famCount, kMaxFontFamilies)) return false;
  out.fontFamilies.clear();
  for (uint32_t i = 0; i < famCount; ++i) {
    std::string fam;
    if (!r.readString(fam)) return false;
    out.fontFamilies.push_back(RcString(fam));
  }

  if (!DecodeLengthd(r, out.fontSize)) return false;
  if (!DecodeBox2d(r, out.viewBox)) return false;

  if (!r.readF64(out.fontMetrics.fontSize)) return false;
  if (!r.readF64(out.fontMetrics.rootFontSize)) return false;
  if (!r.readF64(out.fontMetrics.exUnitInEm)) return false;
  if (!r.readF64(out.fontMetrics.chUnitInEm)) return false;
  bool hasViewport = false;
  if (!r.readBool(hasViewport)) return false;
  if (hasViewport) {
    Vector2d vp;
    if (!DecodeVector2d(r, vp)) return false;
    out.fontMetrics.viewportSize = vp;
  } else {
    out.fontMetrics.viewportSize.reset();
  }

  uint8_t u = 0;
  if (!r.readU8(u)) return false; out.textAnchor = static_cast<svg::TextAnchor>(u);
  if (!r.readU8(u)) return false; out.textDecoration = static_cast<svg::TextDecoration>(u);
  if (!r.readU8(u)) return false; out.dominantBaseline = static_cast<svg::DominantBaseline>(u);
  if (!r.readU8(u)) return false; out.writingMode = static_cast<svg::WritingMode>(u);
  if (!r.readF64(out.letterSpacingPx)) return false;
  if (!r.readF64(out.wordSpacingPx)) return false;
  if (!DecodeOptionalLengthd(r, out.textLength)) return false;
  if (!r.readU8(u)) return false; out.lengthAdjust = static_cast<svg::LengthAdjust>(u);

  uint32_t fontFaceCount = 0;
  if (!r.readCount(fontFaceCount, kMaxFontFaces)) return false;
  if (outFontFaces) {
    outFontFaces->clear();
    outFontFaces->reserve(fontFaceCount);
    for (uint32_t i = 0; i < fontFaceCount; ++i) {
      css::FontFace face;
      if (!DecodeFontFace(r, face)) return false;
      outFontFaces->push_back(std::move(face));
    }
    out.fontFaces = *outFontFaces;
  } else {
    // Caller doesn't need font faces — still must consume the bytes.
    for (uint32_t i = 0; i < fontFaceCount; ++i) {
      css::FontFace face;
      if (!DecodeFontFace(r, face)) return false;
    }
    out.fontFaces = {};
  }
  out.textRootEntity = entt::null;
  return true;
}

void EncodeComputedTextComponent(
    WireWriter& w, const svg::components::ComputedTextComponent& text) {
  w.writeU32(static_cast<uint32_t>(text.spans.size()));
  for (const auto& span : text.spans) {
    EncodeTextSpan(w, span);
  }
}

bool DecodeComputedTextComponent(
    WireReader& r, svg::components::ComputedTextComponent& out) {
  uint32_t count = 0;
  if (!r.readCount(count, kMaxTextSpans)) return false;
  out.spans.clear();
  for (uint32_t i = 0; i < count; ++i) {
    svg::components::ComputedTextComponent::TextSpan span;
    if (!DecodeTextSpan(r, span)) return false;
    out.spans.push_back(std::move(span));
  }
  return true;
}

// -----------------------------------------------------------------------------
// Filter primitive encoding helpers
// -----------------------------------------------------------------------------

namespace {

// Primitive variant tag — stable across versions.
enum class FilterPrimitiveTag : uint8_t {
  kGaussianBlur = 0,
  kFlood = 1,
  kOffset = 2,
  kMerge = 3,
  kBlend = 4,
  kComposite = 5,
  kColorMatrix = 6,
  kDropShadow = 7,
  kComponentTransfer = 8,
  kConvolveMatrix = 9,
  kMorphology = 10,
  kTile = 11,
  kTurbulence = 12,
  kImage = 13,
  kDisplacementMap = 14,
  kDiffuseLighting = 15,
  kSpecularLighting = 16,
};

// Per-field caps for filter-related variable-length fields.
constexpr uint32_t kMaxFilterNodes = 1024;
constexpr uint32_t kMaxFilterInputs = 256;
constexpr uint32_t kMaxColorMatrixValues = 256;
constexpr uint32_t kMaxTransferTableValues = 4096;
constexpr uint32_t kMaxConvolveKernelValues = 10000;
constexpr uint32_t kMaxImageDataBytes = 64u * 1024u * 1024u;

// FilterInput tags
constexpr uint8_t kFilterInputPrevious = 0;
constexpr uint8_t kFilterInputStandard = 1;
constexpr uint8_t kFilterInputNamed = 2;

void EncodeFilterInput(WireWriter& w,
                       const svg::components::FilterInput& input) {
  std::visit(
      [&](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, svg::components::FilterInput::Previous>) {
          w.writeU8(kFilterInputPrevious);
        } else if constexpr (std::is_same_v<T, svg::components::FilterStandardInput>) {
          w.writeU8(kFilterInputStandard);
          w.writeU8(static_cast<uint8_t>(arg));
        } else if constexpr (std::is_same_v<T, svg::components::FilterInput::Named>) {
          w.writeU8(kFilterInputNamed);
          w.writeString(std::string_view(arg.name));
        }
      },
      input.value);
}

bool DecodeFilterInput(WireReader& r, svg::components::FilterInput& out) {
  uint8_t tag = 0;
  if (!r.readU8(tag)) return false;
  switch (tag) {
    case kFilterInputPrevious:
      out.value = svg::components::FilterInput::Previous{};
      return true;
    case kFilterInputStandard: {
      uint8_t v = 0;
      if (!r.readU8(v)) return false;
      if (v > static_cast<uint8_t>(svg::components::FilterStandardInput::StrokePaint)) {
        r.fail();
        return false;
      }
      out.value = static_cast<svg::components::FilterStandardInput>(v);
      return true;
    }
    case kFilterInputNamed: {
      std::string name;
      if (!r.readString(name)) return false;
      out.value = svg::components::FilterInput::Named{RcString(name)};
      return true;
    }
    default:
      r.fail();
      return false;
  }
}

void EncodeOptionalLenghdFilter(WireWriter& w, const std::optional<Lengthd>& v) {
  w.writeBool(v.has_value());
  if (v) EncodeLengthd(w, *v);
}

bool DecodeOptionalLenghdFilter(WireReader& r, std::optional<Lengthd>& out) {
  bool has = false;
  if (!r.readBool(has)) return false;
  if (has) {
    Lengthd v;
    if (!DecodeLengthd(r, v)) return false;
    out = v;
  } else {
    out.reset();
  }
  return true;
}

void EncodeLightSource(WireWriter& w,
                       const svg::components::filter_primitive::LightSource& ls) {
  w.writeU8(static_cast<uint8_t>(ls.type));
  w.writeF64(ls.azimuth);
  w.writeF64(ls.elevation);
  w.writeF64(ls.x);
  w.writeF64(ls.y);
  w.writeF64(ls.z);
  w.writeF64(ls.pointsAtX);
  w.writeF64(ls.pointsAtY);
  w.writeF64(ls.pointsAtZ);
  w.writeF64(ls.spotExponent);
  w.writeBool(ls.limitingConeAngle.has_value());
  if (ls.limitingConeAngle) w.writeF64(*ls.limitingConeAngle);
}

bool DecodeLightSource(WireReader& r,
                       svg::components::filter_primitive::LightSource& out) {
  uint8_t type = 0;
  if (!r.readU8(type)) return false;
  if (type > static_cast<uint8_t>(
                 svg::components::filter_primitive::LightSource::Type::Spot)) {
    r.fail();
    return false;
  }
  out.type = static_cast<svg::components::filter_primitive::LightSource::Type>(type);
  if (!r.readF64(out.azimuth)) return false;
  if (!r.readF64(out.elevation)) return false;
  if (!r.readF64(out.x)) return false;
  if (!r.readF64(out.y)) return false;
  if (!r.readF64(out.z)) return false;
  if (!r.readF64(out.pointsAtX)) return false;
  if (!r.readF64(out.pointsAtY)) return false;
  if (!r.readF64(out.pointsAtZ)) return false;
  if (!r.readF64(out.spotExponent)) return false;
  bool hasCone = false;
  if (!r.readBool(hasCone)) return false;
  if (hasCone) {
    double v = 0;
    if (!r.readF64(v)) return false;
    out.limitingConeAngle = v;
  } else {
    out.limitingConeAngle.reset();
  }
  return true;
}

void EncodeTransferFunc(
    WireWriter& w,
    const svg::components::filter_primitive::ComponentTransfer::Func& f) {
  w.writeU8(static_cast<uint8_t>(f.type));
  w.writeU32(static_cast<uint32_t>(f.tableValues.size()));
  for (double v : f.tableValues) w.writeF64(v);
  w.writeF64(f.slope);
  w.writeF64(f.intercept);
  w.writeF64(f.amplitude);
  w.writeF64(f.exponent);
  w.writeF64(f.offset);
}

bool DecodeTransferFunc(
    WireReader& r,
    svg::components::filter_primitive::ComponentTransfer::Func& out) {
  uint8_t type = 0;
  if (!r.readU8(type)) return false;
  if (type > static_cast<uint8_t>(
                 svg::components::filter_primitive::ComponentTransfer::FuncType::Gamma)) {
    r.fail();
    return false;
  }
  out.type =
      static_cast<svg::components::filter_primitive::ComponentTransfer::FuncType>(type);
  uint32_t count = 0;
  if (!r.readCount(count, kMaxTransferTableValues)) return false;
  out.tableValues.clear();
  out.tableValues.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    double v = 0;
    if (!r.readF64(v)) return false;
    out.tableValues.push_back(v);
  }
  if (!r.readF64(out.slope)) return false;
  if (!r.readF64(out.intercept)) return false;
  if (!r.readF64(out.amplitude)) return false;
  if (!r.readF64(out.exponent)) return false;
  if (!r.readF64(out.offset)) return false;
  return true;
}

// ---- Per-primitive encoders ----

void EncodeFilterPrimitive(
    WireWriter& w,
    const svg::components::filter_primitive::GaussianBlur& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kGaussianBlur));
  w.writeF64(p.stdDeviationX);
  w.writeF64(p.stdDeviationY);
  w.writeU8(static_cast<uint8_t>(p.edgeMode));
}

void EncodeFilterPrimitive(
    WireWriter& w, const svg::components::filter_primitive::Flood& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kFlood));
  EncodeColor(w, p.floodColor);
  w.writeF64(p.floodOpacity);
}

void EncodeFilterPrimitive(
    WireWriter& w, const svg::components::filter_primitive::Offset& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kOffset));
  w.writeF64(p.dx);
  w.writeF64(p.dy);
}

void EncodeFilterPrimitive(
    WireWriter& w, const svg::components::filter_primitive::Merge&) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kMerge));
  // Merge has no fields beyond its inputs (encoded in the FilterNode).
}

void EncodeFilterPrimitive(
    WireWriter& w, const svg::components::filter_primitive::Blend& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kBlend));
  w.writeU8(static_cast<uint8_t>(p.mode));
}

void EncodeFilterPrimitive(
    WireWriter& w, const svg::components::filter_primitive::Composite& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kComposite));
  w.writeU8(static_cast<uint8_t>(p.op));
  w.writeF64(p.k1);
  w.writeF64(p.k2);
  w.writeF64(p.k3);
  w.writeF64(p.k4);
}

void EncodeFilterPrimitive(
    WireWriter& w, const svg::components::filter_primitive::ColorMatrix& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kColorMatrix));
  w.writeU8(static_cast<uint8_t>(p.type));
  w.writeU32(static_cast<uint32_t>(p.values.size()));
  for (double v : p.values) w.writeF64(v);
}

void EncodeFilterPrimitive(
    WireWriter& w, const svg::components::filter_primitive::DropShadow& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kDropShadow));
  w.writeF64(p.dx);
  w.writeF64(p.dy);
  w.writeF64(p.stdDeviationX);
  w.writeF64(p.stdDeviationY);
  EncodeColor(w, p.floodColor);
  w.writeF64(p.floodOpacity);
}

void EncodeFilterPrimitive(
    WireWriter& w,
    const svg::components::filter_primitive::ComponentTransfer& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kComponentTransfer));
  EncodeTransferFunc(w, p.funcR);
  EncodeTransferFunc(w, p.funcG);
  EncodeTransferFunc(w, p.funcB);
  EncodeTransferFunc(w, p.funcA);
}

void EncodeFilterPrimitive(
    WireWriter& w,
    const svg::components::filter_primitive::ConvolveMatrix& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kConvolveMatrix));
  w.writeI32(p.orderX);
  w.writeI32(p.orderY);
  w.writeU32(static_cast<uint32_t>(p.kernelMatrix.size()));
  for (double v : p.kernelMatrix) w.writeF64(v);
  w.writeBool(p.divisor.has_value());
  if (p.divisor) w.writeF64(*p.divisor);
  w.writeF64(p.bias);
  w.writeBool(p.targetX.has_value());
  if (p.targetX) w.writeI32(*p.targetX);
  w.writeBool(p.targetY.has_value());
  if (p.targetY) w.writeI32(*p.targetY);
  w.writeU8(static_cast<uint8_t>(p.edgeMode));
  w.writeBool(p.preserveAlpha);
}

void EncodeFilterPrimitive(
    WireWriter& w, const svg::components::filter_primitive::Morphology& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kMorphology));
  w.writeU8(static_cast<uint8_t>(p.op));
  w.writeF64(p.radiusX);
  w.writeF64(p.radiusY);
}

void EncodeFilterPrimitive(
    WireWriter& w, const svg::components::filter_primitive::Tile&) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kTile));
}

void EncodeFilterPrimitive(
    WireWriter& w, const svg::components::filter_primitive::Turbulence& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kTurbulence));
  w.writeU8(static_cast<uint8_t>(p.type));
  w.writeF64(p.baseFrequencyX);
  w.writeF64(p.baseFrequencyY);
  w.writeI32(p.numOctaves);
  w.writeF64(p.seed);
  w.writeBool(p.stitchTiles);
}

void EncodeFilterPrimitive(
    WireWriter& w, const svg::components::filter_primitive::Image& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kImage));
  w.writeString(std::string_view(p.href));
  w.writeU8(static_cast<uint8_t>(p.preserveAspectRatio.align));
  w.writeU8(static_cast<uint8_t>(p.preserveAspectRatio.meetOrSlice));
  w.writeU32(static_cast<uint32_t>(p.imageData.size()));
  w.writeBytes(p.imageData);
  w.writeI32(p.imageWidth);
  w.writeI32(p.imageHeight);
  w.writeString(std::string_view(p.fragmentId));
  w.writeBool(p.isFragmentReference);
  EncodeVector2d(w, p.fragmentRegionTopLeft);
}

void EncodeFilterPrimitive(
    WireWriter& w,
    const svg::components::filter_primitive::DisplacementMap& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kDisplacementMap));
  w.writeF64(p.scale);
  w.writeU8(static_cast<uint8_t>(p.xChannelSelector));
  w.writeU8(static_cast<uint8_t>(p.yChannelSelector));
}

void EncodeFilterPrimitive(
    WireWriter& w,
    const svg::components::filter_primitive::DiffuseLighting& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kDiffuseLighting));
  w.writeF64(p.surfaceScale);
  w.writeF64(p.diffuseConstant);
  EncodeColor(w, p.lightingColor);
  w.writeBool(p.light.has_value());
  if (p.light) EncodeLightSource(w, *p.light);
}

void EncodeFilterPrimitive(
    WireWriter& w,
    const svg::components::filter_primitive::SpecularLighting& p) {
  w.writeU8(static_cast<uint8_t>(FilterPrimitiveTag::kSpecularLighting));
  w.writeF64(p.surfaceScale);
  w.writeF64(p.specularConstant);
  w.writeF64(p.specularExponent);
  EncodeColor(w, p.lightingColor);
  w.writeBool(p.light.has_value());
  if (p.light) EncodeLightSource(w, *p.light);
}

// ---- Per-primitive decoders (dispatched by tag) ----

bool DecodeFilterPrimitiveGaussianBlur(
    WireReader& r, svg::components::FilterPrimitive& out) {
  svg::components::filter_primitive::GaussianBlur p;
  if (!r.readF64(p.stdDeviationX)) return false;
  if (!r.readF64(p.stdDeviationY)) return false;
  uint8_t em = 0;
  if (!r.readU8(em)) return false;
  if (em > static_cast<uint8_t>(
               svg::components::filter_primitive::GaussianBlur::EdgeMode::Wrap)) {
    r.fail();
    return false;
  }
  p.edgeMode =
      static_cast<svg::components::filter_primitive::GaussianBlur::EdgeMode>(em);
  out = p;
  return true;
}

bool DecodeFilterPrimitiveFlood(WireReader& r,
                                svg::components::FilterPrimitive& out) {
  svg::components::filter_primitive::Flood p;
  if (!DecodeColor(r, p.floodColor)) return false;
  if (!r.readF64(p.floodOpacity)) return false;
  out = p;
  return true;
}

bool DecodeFilterPrimitiveOffset(WireReader& r,
                                 svg::components::FilterPrimitive& out) {
  svg::components::filter_primitive::Offset p;
  if (!r.readF64(p.dx)) return false;
  if (!r.readF64(p.dy)) return false;
  out = p;
  return true;
}

bool DecodeFilterPrimitiveMerge(WireReader& r,
                                svg::components::FilterPrimitive& out) {
  out = svg::components::filter_primitive::Merge{};
  return true;
}

bool DecodeFilterPrimitiveBlend(WireReader& r,
                                svg::components::FilterPrimitive& out) {
  uint8_t mode = 0;
  if (!r.readU8(mode)) return false;
  if (mode > static_cast<uint8_t>(
                 svg::components::filter_primitive::Blend::Mode::Luminosity)) {
    r.fail();
    return false;
  }
  svg::components::filter_primitive::Blend p;
  p.mode = static_cast<svg::components::filter_primitive::Blend::Mode>(mode);
  out = p;
  return true;
}

bool DecodeFilterPrimitiveComposite(WireReader& r,
                                    svg::components::FilterPrimitive& out) {
  uint8_t op = 0;
  if (!r.readU8(op)) return false;
  if (op > static_cast<uint8_t>(
               svg::components::filter_primitive::Composite::Operator::Arithmetic)) {
    r.fail();
    return false;
  }
  svg::components::filter_primitive::Composite p;
  p.op = static_cast<svg::components::filter_primitive::Composite::Operator>(op);
  if (!r.readF64(p.k1)) return false;
  if (!r.readF64(p.k2)) return false;
  if (!r.readF64(p.k3)) return false;
  if (!r.readF64(p.k4)) return false;
  out = p;
  return true;
}

bool DecodeFilterPrimitiveColorMatrix(WireReader& r,
                                      svg::components::FilterPrimitive& out) {
  uint8_t type = 0;
  if (!r.readU8(type)) return false;
  if (type > static_cast<uint8_t>(
                 svg::components::filter_primitive::ColorMatrix::Type::LuminanceToAlpha)) {
    r.fail();
    return false;
  }
  svg::components::filter_primitive::ColorMatrix p;
  p.type = static_cast<svg::components::filter_primitive::ColorMatrix::Type>(type);
  uint32_t count = 0;
  if (!r.readCount(count, kMaxColorMatrixValues)) return false;
  p.values.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    double v = 0;
    if (!r.readF64(v)) return false;
    p.values.push_back(v);
  }
  out = p;
  return true;
}

bool DecodeFilterPrimitiveDropShadow(WireReader& r,
                                     svg::components::FilterPrimitive& out) {
  svg::components::filter_primitive::DropShadow p;
  if (!r.readF64(p.dx)) return false;
  if (!r.readF64(p.dy)) return false;
  if (!r.readF64(p.stdDeviationX)) return false;
  if (!r.readF64(p.stdDeviationY)) return false;
  if (!DecodeColor(r, p.floodColor)) return false;
  if (!r.readF64(p.floodOpacity)) return false;
  out = p;
  return true;
}

bool DecodeFilterPrimitiveComponentTransfer(
    WireReader& r, svg::components::FilterPrimitive& out) {
  svg::components::filter_primitive::ComponentTransfer p;
  if (!DecodeTransferFunc(r, p.funcR)) return false;
  if (!DecodeTransferFunc(r, p.funcG)) return false;
  if (!DecodeTransferFunc(r, p.funcB)) return false;
  if (!DecodeTransferFunc(r, p.funcA)) return false;
  out = p;
  return true;
}

bool DecodeFilterPrimitiveConvolveMatrix(
    WireReader& r, svg::components::FilterPrimitive& out) {
  svg::components::filter_primitive::ConvolveMatrix p;
  if (!r.readI32(p.orderX)) return false;
  if (!r.readI32(p.orderY)) return false;
  uint32_t kernelCount = 0;
  if (!r.readCount(kernelCount, kMaxConvolveKernelValues)) return false;
  p.kernelMatrix.reserve(kernelCount);
  for (uint32_t i = 0; i < kernelCount; ++i) {
    double v = 0;
    if (!r.readF64(v)) return false;
    p.kernelMatrix.push_back(v);
  }
  bool hasDivisor = false;
  if (!r.readBool(hasDivisor)) return false;
  if (hasDivisor) {
    double v = 0;
    if (!r.readF64(v)) return false;
    p.divisor = v;
  }
  if (!r.readF64(p.bias)) return false;
  bool hasTargetX = false;
  if (!r.readBool(hasTargetX)) return false;
  if (hasTargetX) {
    int32_t v = 0;
    if (!r.readI32(v)) return false;
    p.targetX = v;
  }
  bool hasTargetY = false;
  if (!r.readBool(hasTargetY)) return false;
  if (hasTargetY) {
    int32_t v = 0;
    if (!r.readI32(v)) return false;
    p.targetY = v;
  }
  uint8_t em = 0;
  if (!r.readU8(em)) return false;
  if (em > static_cast<uint8_t>(
               svg::components::filter_primitive::ConvolveMatrix::EdgeMode::None)) {
    r.fail();
    return false;
  }
  p.edgeMode =
      static_cast<svg::components::filter_primitive::ConvolveMatrix::EdgeMode>(em);
  if (!r.readBool(p.preserveAlpha)) return false;
  out = p;
  return true;
}

bool DecodeFilterPrimitiveMorphology(WireReader& r,
                                     svg::components::FilterPrimitive& out) {
  uint8_t op = 0;
  if (!r.readU8(op)) return false;
  if (op > static_cast<uint8_t>(
               svg::components::filter_primitive::Morphology::Operator::Dilate)) {
    r.fail();
    return false;
  }
  svg::components::filter_primitive::Morphology p;
  p.op = static_cast<svg::components::filter_primitive::Morphology::Operator>(op);
  if (!r.readF64(p.radiusX)) return false;
  if (!r.readF64(p.radiusY)) return false;
  out = p;
  return true;
}

bool DecodeFilterPrimitiveTile(WireReader& r,
                               svg::components::FilterPrimitive& out) {
  out = svg::components::filter_primitive::Tile{};
  return true;
}

bool DecodeFilterPrimitiveTurbulence(WireReader& r,
                                     svg::components::FilterPrimitive& out) {
  uint8_t type = 0;
  if (!r.readU8(type)) return false;
  if (type > static_cast<uint8_t>(
                 svg::components::filter_primitive::Turbulence::Type::Turbulence)) {
    r.fail();
    return false;
  }
  svg::components::filter_primitive::Turbulence p;
  p.type = static_cast<svg::components::filter_primitive::Turbulence::Type>(type);
  if (!r.readF64(p.baseFrequencyX)) return false;
  if (!r.readF64(p.baseFrequencyY)) return false;
  if (!r.readI32(p.numOctaves)) return false;
  if (!r.readF64(p.seed)) return false;
  if (!r.readBool(p.stitchTiles)) return false;
  out = p;
  return true;
}

bool DecodeFilterPrimitiveImage(WireReader& r,
                                svg::components::FilterPrimitive& out) {
  svg::components::filter_primitive::Image p;
  std::string href;
  if (!r.readString(href)) return false;
  p.href = RcString(href);
  uint8_t align = 0, mos = 0;
  if (!r.readU8(align)) return false;
  if (align > static_cast<uint8_t>(svg::PreserveAspectRatio::Align::XMaxYMax)) {
    r.fail();
    return false;
  }
  if (!r.readU8(mos)) return false;
  if (mos > static_cast<uint8_t>(svg::PreserveAspectRatio::MeetOrSlice::Slice)) {
    r.fail();
    return false;
  }
  p.preserveAspectRatio.align =
      static_cast<svg::PreserveAspectRatio::Align>(align);
  p.preserveAspectRatio.meetOrSlice =
      static_cast<svg::PreserveAspectRatio::MeetOrSlice>(mos);
  uint32_t dataLen = 0;
  if (!r.readCount(dataLen, kMaxImageDataBytes)) return false;
  p.imageData.resize(dataLen);
  if (dataLen > 0 && !r.readBytes(p.imageData)) return false;
  if (!r.readI32(p.imageWidth)) return false;
  if (!r.readI32(p.imageHeight)) return false;
  std::string fragmentId;
  if (!r.readString(fragmentId)) return false;
  p.fragmentId = RcString(fragmentId);
  if (!r.readBool(p.isFragmentReference)) return false;
  if (!DecodeVector2d(r, p.fragmentRegionTopLeft)) return false;
  out = p;
  return true;
}

bool DecodeFilterPrimitiveDisplacementMap(
    WireReader& r, svg::components::FilterPrimitive& out) {
  svg::components::filter_primitive::DisplacementMap p;
  if (!r.readF64(p.scale)) return false;
  uint8_t xCh = 0, yCh = 0;
  if (!r.readU8(xCh)) return false;
  if (xCh > static_cast<uint8_t>(
                svg::components::filter_primitive::DisplacementMap::Channel::A)) {
    r.fail();
    return false;
  }
  if (!r.readU8(yCh)) return false;
  if (yCh > static_cast<uint8_t>(
                svg::components::filter_primitive::DisplacementMap::Channel::A)) {
    r.fail();
    return false;
  }
  p.xChannelSelector =
      static_cast<svg::components::filter_primitive::DisplacementMap::Channel>(xCh);
  p.yChannelSelector =
      static_cast<svg::components::filter_primitive::DisplacementMap::Channel>(yCh);
  out = p;
  return true;
}

bool DecodeFilterPrimitiveDiffuseLighting(
    WireReader& r, svg::components::FilterPrimitive& out) {
  svg::components::filter_primitive::DiffuseLighting p;
  if (!r.readF64(p.surfaceScale)) return false;
  if (!r.readF64(p.diffuseConstant)) return false;
  if (!DecodeColor(r, p.lightingColor)) return false;
  bool hasLight = false;
  if (!r.readBool(hasLight)) return false;
  if (hasLight) {
    svg::components::filter_primitive::LightSource ls;
    if (!DecodeLightSource(r, ls)) return false;
    p.light = ls;
  }
  out = p;
  return true;
}

bool DecodeFilterPrimitiveSpecularLighting(
    WireReader& r, svg::components::FilterPrimitive& out) {
  svg::components::filter_primitive::SpecularLighting p;
  if (!r.readF64(p.surfaceScale)) return false;
  if (!r.readF64(p.specularConstant)) return false;
  if (!r.readF64(p.specularExponent)) return false;
  if (!DecodeColor(r, p.lightingColor)) return false;
  bool hasLight = false;
  if (!r.readBool(hasLight)) return false;
  if (hasLight) {
    svg::components::filter_primitive::LightSource ls;
    if (!DecodeLightSource(r, ls)) return false;
    p.light = ls;
  }
  out = p;
  return true;
}

bool DecodeFilterPrimitiveByTag(WireReader& r, uint8_t tag,
                                svg::components::FilterPrimitive& out) {
  switch (static_cast<FilterPrimitiveTag>(tag)) {
    case FilterPrimitiveTag::kGaussianBlur:
      return DecodeFilterPrimitiveGaussianBlur(r, out);
    case FilterPrimitiveTag::kFlood:
      return DecodeFilterPrimitiveFlood(r, out);
    case FilterPrimitiveTag::kOffset:
      return DecodeFilterPrimitiveOffset(r, out);
    case FilterPrimitiveTag::kMerge:
      return DecodeFilterPrimitiveMerge(r, out);
    case FilterPrimitiveTag::kBlend:
      return DecodeFilterPrimitiveBlend(r, out);
    case FilterPrimitiveTag::kComposite:
      return DecodeFilterPrimitiveComposite(r, out);
    case FilterPrimitiveTag::kColorMatrix:
      return DecodeFilterPrimitiveColorMatrix(r, out);
    case FilterPrimitiveTag::kDropShadow:
      return DecodeFilterPrimitiveDropShadow(r, out);
    case FilterPrimitiveTag::kComponentTransfer:
      return DecodeFilterPrimitiveComponentTransfer(r, out);
    case FilterPrimitiveTag::kConvolveMatrix:
      return DecodeFilterPrimitiveConvolveMatrix(r, out);
    case FilterPrimitiveTag::kMorphology:
      return DecodeFilterPrimitiveMorphology(r, out);
    case FilterPrimitiveTag::kTile:
      return DecodeFilterPrimitiveTile(r, out);
    case FilterPrimitiveTag::kTurbulence:
      return DecodeFilterPrimitiveTurbulence(r, out);
    case FilterPrimitiveTag::kImage:
      return DecodeFilterPrimitiveImage(r, out);
    case FilterPrimitiveTag::kDisplacementMap:
      return DecodeFilterPrimitiveDisplacementMap(r, out);
    case FilterPrimitiveTag::kDiffuseLighting:
      return DecodeFilterPrimitiveDiffuseLighting(r, out);
    case FilterPrimitiveTag::kSpecularLighting:
      return DecodeFilterPrimitiveSpecularLighting(r, out);
    default:
      r.fail();
      return false;
  }
}

void EncodeFilterNode(WireWriter& w,
                      const svg::components::FilterNode& node) {
  // Primitive (tag + fields).
  std::visit([&](const auto& p) { EncodeFilterPrimitive(w, p); },
             node.primitive);

  // Inputs.
  w.writeU32(static_cast<uint32_t>(node.inputs.size()));
  for (const auto& input : node.inputs) {
    EncodeFilterInput(w, input);
  }

  // Named result.
  w.writeBool(node.result.has_value());
  if (node.result) w.writeString(std::string_view(*node.result));

  // Subregion bounds.
  EncodeOptionalLenghdFilter(w, node.x);
  EncodeOptionalLenghdFilter(w, node.y);
  EncodeOptionalLenghdFilter(w, node.width);
  EncodeOptionalLenghdFilter(w, node.height);

  // Per-primitive color-interpolation-filters override.
  w.writeBool(node.colorInterpolationFilters.has_value());
  if (node.colorInterpolationFilters) {
    w.writeU8(static_cast<uint8_t>(*node.colorInterpolationFilters));
  }
}

bool DecodeFilterNode(WireReader& r, svg::components::FilterNode& out) {
  // Primitive tag + fields.
  uint8_t tag = 0;
  if (!r.readU8(tag)) return false;
  if (!DecodeFilterPrimitiveByTag(r, tag, out.primitive)) return false;

  // Inputs.
  uint32_t inputCount = 0;
  if (!r.readCount(inputCount, kMaxFilterInputs)) return false;
  out.inputs.clear();
  out.inputs.reserve(inputCount);
  for (uint32_t i = 0; i < inputCount; ++i) {
    svg::components::FilterInput input;
    if (!DecodeFilterInput(r, input)) return false;
    out.inputs.push_back(std::move(input));
  }

  // Named result.
  bool hasResult = false;
  if (!r.readBool(hasResult)) return false;
  if (hasResult) {
    std::string result;
    if (!r.readString(result)) return false;
    out.result = RcString(result);
  } else {
    out.result.reset();
  }

  // Subregion bounds.
  if (!DecodeOptionalLenghdFilter(r, out.x)) return false;
  if (!DecodeOptionalLenghdFilter(r, out.y)) return false;
  if (!DecodeOptionalLenghdFilter(r, out.width)) return false;
  if (!DecodeOptionalLenghdFilter(r, out.height)) return false;

  // Per-primitive color-interpolation-filters override.
  bool hasCif = false;
  if (!r.readBool(hasCif)) return false;
  if (hasCif) {
    uint8_t cif = 0;
    if (!r.readU8(cif)) return false;
    out.colorInterpolationFilters =
        static_cast<svg::ColorInterpolationFilters>(cif);
  } else {
    out.colorInterpolationFilters.reset();
  }
  return true;
}

}  // namespace

void EncodeFilterGraph(WireWriter& w, const svg::components::FilterGraph& g) {
  // Encode the full primitive chain.
  w.writeU32(static_cast<uint32_t>(g.nodes.size()));
  for (const auto& node : g.nodes) {
    EncodeFilterNode(w, node);
  }
  w.writeU8(static_cast<uint8_t>(g.colorInterpolationFilters));
  w.writeU8(static_cast<uint8_t>(g.primitiveUnits));
  w.writeBool(g.elementBoundingBox.has_value());
  if (g.elementBoundingBox) EncodeBox2d(w, *g.elementBoundingBox);
  w.writeBool(g.filterRegion.has_value());
  if (g.filterRegion) EncodeBox2d(w, *g.filterRegion);
  EncodeVector2d(w, g.userToPixelScale);
}

bool DecodeFilterGraph(WireReader& r, svg::components::FilterGraph& out) {
  uint32_t nodeCount = 0;
  if (!r.readCount(nodeCount, kMaxFilterNodes)) return false;
  out.nodes.clear();
  out.nodes.reserve(nodeCount);
  for (uint32_t i = 0; i < nodeCount; ++i) {
    svg::components::FilterNode node;
    node.primitive = svg::components::filter_primitive::GaussianBlur{};
    if (!DecodeFilterNode(r, node)) return false;
    out.nodes.push_back(std::move(node));
  }
  uint8_t cif = 0;
  if (!r.readU8(cif)) return false;
  out.colorInterpolationFilters =
      static_cast<svg::ColorInterpolationFilters>(cif);
  uint8_t pu = 0;
  if (!r.readU8(pu)) return false;
  out.primitiveUnits = static_cast<svg::PrimitiveUnits>(pu);
  bool hasEbb = false;
  if (!r.readBool(hasEbb)) return false;
  if (hasEbb) {
    Box2d box;
    if (!DecodeBox2d(r, box)) return false;
    out.elementBoundingBox = box;
  } else {
    out.elementBoundingBox.reset();
  }
  bool hasFr = false;
  if (!r.readBool(hasFr)) return false;
  if (hasFr) {
    Box2d box;
    if (!DecodeBox2d(r, box)) return false;
    out.filterRegion = box;
  } else {
    out.filterRegion.reset();
  }
  if (!DecodeVector2d(r, out.userToPixelScale)) return false;
  return true;
}

}  // namespace donner::editor::sandbox
