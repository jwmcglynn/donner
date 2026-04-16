#include "donner/editor/sandbox/FrameInspector.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <string_view>

#include "donner/editor/sandbox/SandboxCodecs.h"

namespace donner::editor::sandbox {

namespace {

/// True if `op` opens a LIFO scope (push*, begin*).
bool IsPush(Opcode op) {
  switch (op) {
    case Opcode::kPushTransform:
    case Opcode::kPushClip:
    case Opcode::kPushIsolatedLayer: return true;
    default: return false;
  }
}

/// True if `op` closes a LIFO scope (pop*, end*).
bool IsPop(Opcode op) {
  switch (op) {
    case Opcode::kPopTransform:
    case Opcode::kPopClip:
    case Opcode::kPopIsolatedLayer: return true;
    default: return false;
  }
}

/// Peeks at the u32 opcode + u32 payload_length at the current cursor
/// without advancing it. Used by the decode pass, which needs to record
/// the absolute byte offset *before* reading the header.
bool PeekMessageHeader(std::span<const uint8_t> bytes, std::size_t pos, Opcode& outOp,
                       uint32_t& outLen) {
  if (pos + 8 > bytes.size()) return false;
  uint32_t op = 0;
  uint32_t len = 0;
  std::memcpy(&op, bytes.data() + pos, sizeof(uint32_t));
  std::memcpy(&len, bytes.data() + pos + sizeof(uint32_t), sizeof(uint32_t));
  outOp = static_cast<Opcode>(op);
  outLen = len;
  return true;
}

/// Builds a terse summary for the supported opcodes. Unknown or complex
/// payloads return the plain opcode name.
std::string SummarizeCommand(Opcode op, std::span<const uint8_t> payload) {
  WireReader r(payload);

  auto formatDouble = [](double v) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g", v);
    return std::string(buf);
  };

  auto formatBox = [&](const Box2d& b) {
    std::ostringstream os;
    os << "(" << formatDouble(b.topLeft.x) << "," << formatDouble(b.topLeft.y) << ","
       << formatDouble(b.bottomRight.x) << "," << formatDouble(b.bottomRight.y) << ")";
    return os.str();
  };

  switch (op) {
    case Opcode::kStreamHeader: {
      uint32_t magic = 0;
      uint32_t version = 0;
      if (r.readU32(magic) && r.readU32(version)) {
        std::ostringstream os;
        os << "streamHeader magic=0x" << std::hex << magic << std::dec << " version=" << version;
        return os.str();
      }
      return "streamHeader";
    }

    case Opcode::kBeginFrame: {
      svg::RenderViewport vp;
      if (DecodeRenderViewport(r, vp)) {
        std::ostringstream os;
        os << "beginFrame viewport=" << formatDouble(vp.size.x) << "x" << formatDouble(vp.size.y)
           << " dpr=" << formatDouble(vp.devicePixelRatio);
        return os.str();
      }
      return "beginFrame";
    }

    case Opcode::kEndFrame: return "endFrame";

    case Opcode::kSetTransform: {
      Transform2d t;
      if (DecodeTransform2d(r, t)) {
        std::ostringstream os;
        os << "setTransform tx=" << formatDouble(t.data[4]) << " ty=" << formatDouble(t.data[5]);
        return os.str();
      }
      return "setTransform";
    }

    case Opcode::kPushTransform: {
      Transform2d t;
      if (DecodeTransform2d(r, t)) {
        std::ostringstream os;
        os << "pushTransform tx=" << formatDouble(t.data[4]) << " ty=" << formatDouble(t.data[5]);
        return os.str();
      }
      return "pushTransform";
    }
    case Opcode::kPopTransform: return "popTransform";

    case Opcode::kPushClip: {
      svg::ResolvedClip clip;
      if (DecodeResolvedClip(r, clip)) {
        std::ostringstream os;
        os << "pushClip";
        if (clip.clipRect) os << " rect=" << formatBox(*clip.clipRect);
        if (!clip.clipPaths.empty()) os << " paths=" << clip.clipPaths.size();
        return os.str();
      }
      return "pushClip";
    }
    case Opcode::kPopClip: return "popClip";

    case Opcode::kPushIsolatedLayer: {
      double opacity = 0;
      svg::MixBlendMode mode = svg::MixBlendMode::Normal;
      if (r.readF64(opacity) && DecodeMixBlendMode(r, mode)) {
        std::ostringstream os;
        os << "pushIsolatedLayer opacity=" << formatDouble(opacity)
           << " mode=" << static_cast<int>(mode);
        return os.str();
      }
      return "pushIsolatedLayer";
    }
    case Opcode::kPopIsolatedLayer: return "popIsolatedLayer";

    case Opcode::kSetPaint: {
      svg::PaintParams paint;
      std::optional<WireGradient> fillGradient;
      std::optional<WireGradient> strokeGradient;
      if (DecodePaintParams(r, paint, &fillGradient, &strokeGradient)) {
        std::ostringstream os;
        os << "setPaint opacity=" << formatDouble(paint.opacity);
        // Fill summary — recognize solid + gradient variants.
        if (fillGradient) {
          os << " fill="
             << (fillGradient->kind == WireGradient::Kind::kLinear ? "linearGradient"
                                                                   : "radialGradient")
             << "(" << fillGradient->stops.size() << " stops)";
        } else if (std::holds_alternative<svg::PaintServer::Solid>(paint.fill)) {
          const auto& solid = std::get<svg::PaintServer::Solid>(paint.fill);
          if (std::holds_alternative<css::RGBA>(solid.color.value)) {
            const auto c = std::get<css::RGBA>(solid.color.value);
            char hex[16];
            std::snprintf(hex, sizeof(hex), " fill=#%02X%02X%02X", c.r, c.g, c.b);
            os << hex;
          }
        } else {
          os << " fill=none";
        }
        return os.str();
      }
      return "setPaint";
    }

    case Opcode::kDrawPath: {
      svg::PathShape shape;
      if (DecodePathShape(r, shape)) {
        std::ostringstream os;
        os << "drawPath verbs=" << shape.path.commands().size();
        return os.str();
      }
      return "drawPath";
    }

    case Opcode::kDrawRect: {
      Box2d rect;
      if (DecodeBox2d(r, rect)) {
        return "drawRect " + formatBox(rect);
      }
      return "drawRect";
    }

    case Opcode::kDrawEllipse: {
      Box2d bounds;
      if (DecodeBox2d(r, bounds)) {
        return "drawEllipse " + formatBox(bounds);
      }
      return "drawEllipse";
    }

    case Opcode::kDrawText: return "drawText";
    case Opcode::kPushMask: return "pushMask";
    case Opcode::kTransitionMaskToContent: return "transitionMaskToContent";
    case Opcode::kPopMask: return "popMask";
    case Opcode::kBeginPatternTile: return "beginPatternTile";
    case Opcode::kEndPatternTile: return "endPatternTile";
    case Opcode::kPushFilterLayer: return "pushFilterLayer";
    case Opcode::kPopFilterLayer: return "popFilterLayer";
    case Opcode::kDrawImage: return "drawImage";

    case Opcode::kUnsupported: {
      uint32_t kindRaw = 0;
      if (r.readU32(kindRaw)) {
        std::ostringstream os;
        os << "unsupported ("
           << FrameInspector::UnsupportedKindName(static_cast<UnsupportedKind>(kindRaw)) << ")";
        return os.str();
      }
      return "unsupported";
    }

    case Opcode::kInvalid: return "<invalid>";
  }
  return std::string(FrameInspector::OpcodeName(op));
}

}  // namespace

std::string_view FrameInspector::OpcodeName(Opcode op) {
  switch (op) {
    case Opcode::kInvalid: return "invalid";
    case Opcode::kStreamHeader: return "streamHeader";
    case Opcode::kBeginFrame: return "beginFrame";
    case Opcode::kEndFrame: return "endFrame";
    case Opcode::kSetTransform: return "setTransform";
    case Opcode::kPushTransform: return "pushTransform";
    case Opcode::kPopTransform: return "popTransform";
    case Opcode::kPushClip: return "pushClip";
    case Opcode::kPopClip: return "popClip";
    case Opcode::kPushIsolatedLayer: return "pushIsolatedLayer";
    case Opcode::kPopIsolatedLayer: return "popIsolatedLayer";
    case Opcode::kPushMask: return "pushMask";
    case Opcode::kTransitionMaskToContent: return "transitionMaskToContent";
    case Opcode::kPopMask: return "popMask";
    case Opcode::kBeginPatternTile: return "beginPatternTile";
    case Opcode::kEndPatternTile: return "endPatternTile";
    case Opcode::kPushFilterLayer: return "pushFilterLayer";
    case Opcode::kPopFilterLayer: return "popFilterLayer";
    case Opcode::kSetPaint: return "setPaint";
    case Opcode::kDrawPath: return "drawPath";
    case Opcode::kDrawRect: return "drawRect";
    case Opcode::kDrawEllipse: return "drawEllipse";
    case Opcode::kDrawImage: return "drawImage";
    case Opcode::kDrawText: return "drawText";
    case Opcode::kUnsupported: return "unsupported";
  }
  return "unknown";
}

std::string_view FrameInspector::UnsupportedKindName(UnsupportedKind kind) {
  switch (kind) {
    case UnsupportedKind::kPushFilterLayer: return "pushFilterLayer";
    case UnsupportedKind::kPopFilterLayer: return "popFilterLayer";
    case UnsupportedKind::kPushMask: return "pushMask";
    case UnsupportedKind::kTransitionMaskToContent: return "transitionMaskToContent";
    case UnsupportedKind::kPopMask: return "popMask";
    case UnsupportedKind::kBeginPatternTile: return "beginPatternTile";
    case UnsupportedKind::kEndPatternTile: return "endPatternTile";
    case UnsupportedKind::kDrawImage: return "drawImage";
    case UnsupportedKind::kDrawText: return "drawText";
    case UnsupportedKind::kPaintServerGradient: return "paintServerGradient";
    case UnsupportedKind::kPaintServerPattern: return "paintServerPattern";
    case UnsupportedKind::kPaintServerResolvedReference: return "paintServerResolvedReference";
    case UnsupportedKind::kClipMaskChain: return "clipMaskChain";
    case UnsupportedKind::kColorNonRgba: return "colorNonRgba";
  }
  return "unknownUnsupportedKind";
}

InspectionResult FrameInspector::Decode(std::span<const uint8_t> wire) {
  InspectionResult out;
  std::size_t pos = 0;
  uint32_t index = 0;
  int32_t depth = 0;

  while (pos < wire.size()) {
    Opcode op = Opcode::kInvalid;
    uint32_t payloadLen = 0;
    if (!PeekMessageHeader(wire, pos, op, payloadLen)) {
      out.error = "truncated message header at offset " + std::to_string(pos);
      out.finalDepth = depth;
      return out;
    }
    if (payloadLen > kMaxPayloadBytes || pos + 8 + payloadLen > wire.size()) {
      out.error = "payload length " + std::to_string(payloadLen) + " overruns buffer at offset " +
                  std::to_string(pos);
      out.finalDepth = depth;
      return out;
    }

    DecodedCommand cmd;
    cmd.index = index++;
    cmd.opcode = op;
    cmd.depth = std::max<int32_t>(0, depth);
    cmd.byteOffset = pos;
    cmd.byteLength = 8 + payloadLen;

    const auto payload = wire.subspan(pos + 8, payloadLen);
    cmd.summary = SummarizeCommand(op, payload);
    out.commands.push_back(std::move(cmd));

    // Depth tracking happens *after* recording the row so the push/pop
    // commands themselves appear at the parent's depth — matching how most
    // tree UIs render the handles for an expandable node.
    if (IsPush(op)) ++depth;
    if (IsPop(op)) --depth;

    pos += 8 + payloadLen;
  }

  out.streamValid = true;
  out.finalDepth = depth;
  return out;
}

ReplayStatus FrameInspector::ReplayPrefix(std::span<const uint8_t> wire, std::size_t commandCount,
                                          svg::RendererInterface& target) {
  // Full-replay fast path: the caller asked for everything. Just forward.
  if (commandCount == std::numeric_limits<std::size_t>::max()) {
    ReplayingRenderer replay(target);
    ReplayReport report;
    return replay.pumpFrame(wire, report);
  }

  // Walk the stream looking for the byte offset just past the Nth command
  // (where command 0 is `kBeginFrame` — the stream header is always
  // consumed but not counted, matching the `Decode()` convention minus the
  // header row).
  std::size_t pos = 0;
  std::size_t cmdsSeen = 0;
  bool sawHeader = false;
  bool sawEndFrame = false;

  while (pos < wire.size()) {
    Opcode op = Opcode::kInvalid;
    uint32_t payloadLen = 0;
    if (!PeekMessageHeader(wire, pos, op, payloadLen)) break;
    if (pos + 8 + payloadLen > wire.size()) break;

    const std::size_t nextPos = pos + 8 + payloadLen;

    if (!sawHeader) {
      if (op != Opcode::kStreamHeader) return ReplayStatus::kHeaderMismatch;
      sawHeader = true;
      pos = nextPos;
      continue;
    }

    if (cmdsSeen >= commandCount) break;
    ++cmdsSeen;
    pos = nextPos;
    if (op == Opcode::kEndFrame) {
      sawEndFrame = true;
      break;
    }
  }

  if (!sawHeader) return ReplayStatus::kHeaderMismatch;

  // Replay the prefix we just located. ReplayingRenderer re-validates the
  // header and payload cross-checks on its own, so this is the only place
  // that needs to know about prefix boundaries.
  const auto prefix = wire.subspan(0, pos);
  ReplayingRenderer replay(target);
  ReplayReport report;
  const ReplayStatus status = replay.pumpFrame(prefix, report);

  if (sawEndFrame) {
    // Whole (possibly lossy) frame — passthrough.
    return status;
  }

  // We cut the stream short. `kEndOfStream` is expected here; synthesize
  // the missing endFrame so the target has a valid frame to snapshot.
  if (status == ReplayStatus::kEndOfStream || status == ReplayStatus::kOk ||
      status == ReplayStatus::kEncounteredUnsupported) {
    target.endFrame();
    return ReplayStatus::kOk;
  }
  return status;
}

std::string FrameInspector::Dump(std::span<const uint8_t> wire) {
  const auto result = Decode(wire);
  std::ostringstream os;
  os << "# " << result.commands.size() << " command(s), finalDepth=" << result.finalDepth << "\n";
  for (const auto& cmd : result.commands) {
    os << "[" << cmd.index << "] ";
    for (int i = 0; i < cmd.depth; ++i) os << "  ";
    os << cmd.summary << "\n";
  }
  if (!result.streamValid) {
    os << "! decode stopped: " << result.error << "\n";
  }
  return os.str();
}

}  // namespace donner::editor::sandbox
