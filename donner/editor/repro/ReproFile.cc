#include "donner/editor/repro/ReproFile.h"

#include <cerrno>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>

namespace donner::editor::repro {

namespace {

// Minimal string quoter for JSON. Handles ASCII-safe escapes we need
// for filenames, ISO timestamps, and event-type tags — does NOT attempt
// to handle every Unicode escape. The recorder only emits short
// ASCII-ish strings so this is sufficient.
void WriteQuotedJsonString(std::ostream& os, std::string_view s) {
  os << '"';
  for (char c : s) {
    switch (c) {
      case '"': os << "\\\""; break;
      case '\\': os << "\\\\"; break;
      case '\b': os << "\\b"; break;
      case '\f': os << "\\f"; break;
      case '\n': os << "\\n"; break;
      case '\r': os << "\\r"; break;
      case '\t': os << "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned>(c));
          os << buf;
        } else {
          os << c;
        }
        break;
    }
  }
  os << '"';
}

const char* EventKindTag(ReproEvent::Kind kind) {
  switch (kind) {
    case ReproEvent::Kind::MouseDown: return "mdown";
    case ReproEvent::Kind::MouseUp: return "mup";
    case ReproEvent::Kind::KeyDown: return "kdown";
    case ReproEvent::Kind::KeyUp: return "kup";
    case ReproEvent::Kind::Char: return "chr";
    case ReproEvent::Kind::Wheel: return "wheel";
    case ReproEvent::Kind::Resize: return "resize";
    case ReproEvent::Kind::Focus: return "focus";
  }
  return "unknown";
}

std::optional<ReproEvent::Kind> ParseEventKind(std::string_view tag) {
  if (tag == "mdown") return ReproEvent::Kind::MouseDown;
  if (tag == "mup") return ReproEvent::Kind::MouseUp;
  if (tag == "kdown") return ReproEvent::Kind::KeyDown;
  if (tag == "kup") return ReproEvent::Kind::KeyUp;
  if (tag == "chr") return ReproEvent::Kind::Char;
  if (tag == "wheel") return ReproEvent::Kind::Wheel;
  if (tag == "resize") return ReproEvent::Kind::Resize;
  if (tag == "focus") return ReproEvent::Kind::Focus;
  return std::nullopt;
}

void WriteHit(std::ostream& os, const ReproHit& hit) {
  os << "\"hit\":{";
  bool first = true;
  const auto writeFieldSeparator = [&]() {
    if (!first) os << ',';
    first = false;
  };
  if (hit.empty) {
    writeFieldSeparator();
    os << "\"empty\":1";
  } else {
    writeFieldSeparator();
    os << "\"tag\":";
    WriteQuotedJsonString(os, hit.tag);
    if (!hit.id.empty()) {
      writeFieldSeparator();
      os << "\"id\":";
      WriteQuotedJsonString(os, hit.id);
    }
    if (hit.docOrderIndex >= 0) {
      writeFieldSeparator();
      os << "\"idx\":" << hit.docOrderIndex;
    }
  }
  os << '}';
}

void WriteEvent(std::ostream& os, const ReproEvent& ev) {
  os << "{\"k\":\"" << EventKindTag(ev.kind) << '"';
  switch (ev.kind) {
    case ReproEvent::Kind::MouseDown:
    case ReproEvent::Kind::MouseUp: os << ",\"b\":" << ev.mouseButton; break;
    case ReproEvent::Kind::KeyDown:
    case ReproEvent::Kind::KeyUp: os << ",\"key\":" << ev.key << ",\"m\":" << ev.modifiers; break;
    case ReproEvent::Kind::Char: os << ",\"c\":" << ev.codepoint; break;
    case ReproEvent::Kind::Wheel:
      os << ",\"dx\":" << ev.wheelDeltaX << ",\"dy\":" << ev.wheelDeltaY;
      break;
    case ReproEvent::Kind::Resize: os << ",\"w\":" << ev.width << ",\"h\":" << ev.height; break;
    case ReproEvent::Kind::Focus: os << ",\"on\":" << (ev.focusOn ? 1 : 0); break;
  }
  if (ev.hit.has_value()) {
    os << ',';
    WriteHit(os, *ev.hit);
  }
  os << '}';
}

void WriteExpectation(std::ostream& os, const ReproExpectation& expect) {
  os << "\"expect\":{"
     << "\"left_mouse_down_ordinal\":" << expect.leftMouseDownOrdinal
     << ",\"frame_offset_after_left_mouse_down\":" << expect.frameOffsetAfterLeftMouseDown
     << ",\"min_frame_index\":" << expect.minFrameIndex
     << ",\"max_frame_index\":" << expect.maxFrameIndex << ",\"target_selector\":";
  WriteQuotedJsonString(os, expect.targetSelector);
  os << ",\"crop_mode\":";
  WriteQuotedJsonString(os, expect.cropMode);
  if (expect.cropRect.has_value()) {
    const ReproExpectedCrop& crop = *expect.cropRect;
    os << ",\"crop\":{\"x\":" << crop.x << ",\"y\":" << crop.y << ",\"w\":" << crop.width
       << ",\"h\":" << crop.height << '}';
  }
  os << '}';
}

void WriteMetadataLine(std::ostream& os, const ReproMetadata& meta) {
  os << "{\"v\":" << kReproFileVersion << ",\"svg\":";
  WriteQuotedJsonString(os, meta.svgPath);
  os << ",\"wnd\":[" << meta.windowWidth << ',' << meta.windowHeight << ']'
     << ",\"scale\":" << meta.displayScale << ",\"exp\":" << (meta.experimentalMode ? 1 : 0);
  if (!meta.startedAtIso8601.empty()) {
    os << ",\"at\":";
    WriteQuotedJsonString(os, meta.startedAtIso8601);
  }
  if (meta.expect.has_value()) {
    os << ',';
    WriteExpectation(os, *meta.expect);
  }
  os << "}\n";
}

void WriteViewport(std::ostream& os, const ReproViewport& viewport) {
  os << "\"vp\":{" << "\"ox\":" << viewport.paneOriginX << ",\"oy\":" << viewport.paneOriginY
     << ",\"pw\":" << viewport.paneSizeW << ",\"ph\":" << viewport.paneSizeH
     << ",\"dpr\":" << viewport.devicePixelRatio << ",\"z\":" << viewport.zoom
     << ",\"pdx\":" << viewport.panDocX << ",\"pdy\":" << viewport.panDocY
     << ",\"psx\":" << viewport.panScreenX << ",\"psy\":" << viewport.panScreenY
     << ",\"vbx\":" << viewport.viewBoxX << ",\"vby\":" << viewport.viewBoxY
     << ",\"vbw\":" << viewport.viewBoxW << ",\"vbh\":" << viewport.viewBoxH << '}';
}

void WriteFrameLine(std::ostream& os, const ReproFrame& frame) {
  os << "{\"f\":" << frame.index << ",\"t\":" << frame.timestampSeconds
     << ",\"dt\":" << frame.deltaMs << ",\"mx\":" << frame.mouseX << ",\"my\":" << frame.mouseY
     << ",\"btn\":" << frame.mouseButtonMask << ",\"mod\":" << frame.modifiers;
  if (frame.mouseDocX.has_value() && frame.mouseDocY.has_value()) {
    os << ",\"mdx\":" << *frame.mouseDocX << ",\"mdy\":" << *frame.mouseDocY;
  }
  if (frame.viewport.has_value()) {
    os << ',';
    WriteViewport(os, *frame.viewport);
  }
  if (!frame.events.empty()) {
    os << ",\"e\":[";
    for (std::size_t i = 0; i < frame.events.size(); ++i) {
      if (i > 0) os << ',';
      WriteEvent(os, frame.events[i]);
    }
    os << ']';
  }
  os << "}\n";
}

// Dead-simple JSON scalar extractor for our controlled format. Looks
// for `"key":` after the caller's starting position, then parses the
// following token as a number / string / array-of-numbers. Not a
// general JSON parser — our writer emits a known, regular shape.
std::string_view FindKey(std::string_view line, std::string_view key) {
  std::string pattern;
  pattern.reserve(key.size() + 3);
  pattern += '"';
  pattern += key;
  pattern += "\":";
  const auto pos = line.find(pattern);
  if (pos == std::string_view::npos) return {};
  return line.substr(pos + pattern.size());
}

std::optional<double> ReadNumber(std::string_view& cursor) {
  // Skip leading whitespace.
  std::size_t i = 0;
  while (i < cursor.size() && (cursor[i] == ' ' || cursor[i] == '\t')) ++i;
  const std::size_t start = i;
  if (i < cursor.size() && (cursor[i] == '-' || cursor[i] == '+')) ++i;
  while (i < cursor.size() &&
         ((cursor[i] >= '0' && cursor[i] <= '9') || cursor[i] == '.' || cursor[i] == 'e' ||
          cursor[i] == 'E' || cursor[i] == '+' || cursor[i] == '-')) {
    ++i;
  }
  if (i == start) return std::nullopt;
  const std::string token(cursor.substr(start, i - start));
  char* endPtr = nullptr;
  errno = 0;
  const double value = std::strtod(token.c_str(), &endPtr);
  if (endPtr == token.c_str() || errno != 0) {
    return std::nullopt;
  }
  cursor.remove_prefix(i);
  return value;
}

std::optional<std::string> ReadString(std::string_view& cursor) {
  std::size_t i = 0;
  while (i < cursor.size() && (cursor[i] == ' ' || cursor[i] == '\t')) ++i;
  if (i >= cursor.size() || cursor[i] != '"') return std::nullopt;
  ++i;
  std::string out;
  while (i < cursor.size()) {
    const char c = cursor[i];
    if (c == '"') {
      cursor.remove_prefix(i + 1);
      return out;
    }
    if (c == '\\' && i + 1 < cursor.size()) {
      const char next = cursor[i + 1];
      switch (next) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        default: out += next; break;
      }
      i += 2;
      continue;
    }
    out += c;
    ++i;
  }
  return std::nullopt;
}

// Finds the matching closing brace for a `{` that `cursor` points
// immediately past, respecting string literals and nested objects.
bool ExtractBalancedObject(std::string_view& cursor, std::string_view& body) {
  int depth = 1;
  std::size_t i = 0;
  while (i < cursor.size() && depth > 0) {
    if (cursor[i] == '"') {
      ++i;
      while (i < cursor.size() && cursor[i] != '"') {
        if (cursor[i] == '\\' && i + 1 < cursor.size())
          i += 2;
        else
          ++i;
      }
      if (i < cursor.size()) ++i;
      continue;
    }
    if (cursor[i] == '{')
      ++depth;
    else if (cursor[i] == '}')
      --depth;
    ++i;
  }
  if (depth != 0) return false;
  body = cursor.substr(0, i - 1);
  cursor.remove_prefix(i);
  return true;
}

std::optional<ReproHit> ParseHitObject(std::string_view body) {
  ReproHit hit;

  auto emptyRest = FindKey(body, "empty");
  if (!emptyRest.empty()) {
    auto value = ReadNumber(emptyRest);
    if (!value) return std::nullopt;
    hit.empty = (*value != 0.0);
    if (hit.empty) return hit;
  }

  auto tagRest = FindKey(body, "tag");
  if (!tagRest.empty()) {
    auto tag = ReadString(tagRest);
    if (!tag.has_value()) return std::nullopt;
    hit.tag = std::move(*tag);
  }
  auto idRest = FindKey(body, "id");
  if (!idRest.empty()) {
    auto id = ReadString(idRest);
    if (!id.has_value()) return std::nullopt;
    hit.id = std::move(*id);
  }
  auto idxRest = FindKey(body, "idx");
  if (!idxRest.empty()) {
    auto index = ReadNumber(idxRest);
    if (!index) return std::nullopt;
    hit.docOrderIndex = static_cast<int>(*index);
  }
  return hit;
}

// Parses an event object starting at `cursor` pointing just past the `{`.
// Advances `cursor` past the matching `}`. Returns nullopt on malformed input.
std::optional<ReproEvent> ParseEventObject(std::string_view& cursor) {
  std::string_view body;
  if (!ExtractBalancedObject(cursor, body)) return std::nullopt;

  ReproEvent ev;
  auto rest = FindKey(body, "k");
  if (rest.empty()) return std::nullopt;
  auto kindStr = ReadString(rest);
  if (!kindStr.has_value()) return std::nullopt;
  auto kind = ParseEventKind(*kindStr);
  if (!kind.has_value()) return std::nullopt;
  ev.kind = *kind;

  const auto readIntField = [&](std::string_view key, int& out) {
    auto r = FindKey(body, key);
    if (r.empty()) return;
    auto v = ReadNumber(r);
    if (v) out = static_cast<int>(*v);
  };
  const auto readUintField = [&](std::string_view key, std::uint32_t& out) {
    auto r = FindKey(body, key);
    if (r.empty()) return;
    auto v = ReadNumber(r);
    if (v) out = static_cast<std::uint32_t>(*v);
  };
  const auto readFloatField = [&](std::string_view key, float& out) {
    auto r = FindKey(body, key);
    if (r.empty()) return;
    auto v = ReadNumber(r);
    if (v) out = static_cast<float>(*v);
  };
  const auto readBoolField = [&](std::string_view key, bool& out) {
    auto r = FindKey(body, key);
    if (r.empty()) return;
    auto v = ReadNumber(r);
    if (v) out = (*v != 0.0);
  };

  readIntField("b", ev.mouseButton);
  readIntField("key", ev.key);
  readIntField("m", ev.modifiers);
  readUintField("c", ev.codepoint);
  readFloatField("dx", ev.wheelDeltaX);
  readFloatField("dy", ev.wheelDeltaY);
  readIntField("w", ev.width);
  readIntField("h", ev.height);
  readBoolField("on", ev.focusOn);

  auto hitRest = FindKey(body, "hit");
  if (!hitRest.empty()) {
    std::size_t p = 0;
    while (p < hitRest.size() && (hitRest[p] == ' ' || hitRest[p] == '\t')) ++p;
    if (p >= hitRest.size() || hitRest[p] != '{') return std::nullopt;
    std::string_view hitCursor = hitRest.substr(p + 1);
    std::string_view hitBody;
    if (!ExtractBalancedObject(hitCursor, hitBody)) return std::nullopt;
    auto hit = ParseHitObject(hitBody);
    if (!hit.has_value()) return std::nullopt;
    ev.hit = std::move(*hit);
  }

  return ev;
}

std::optional<ReproViewport> ParseViewportObject(std::string_view body) {
  ReproViewport viewport;
  const auto readField = [&](std::string_view key, double& out) {
    auto r = FindKey(body, key);
    if (r.empty()) return false;
    auto v = ReadNumber(r);
    if (!v) return false;
    out = *v;
    return true;
  };

  if (!readField("ox", viewport.paneOriginX)) return std::nullopt;
  if (!readField("oy", viewport.paneOriginY)) return std::nullopt;
  if (!readField("pw", viewport.paneSizeW)) return std::nullopt;
  if (!readField("ph", viewport.paneSizeH)) return std::nullopt;
  if (!readField("dpr", viewport.devicePixelRatio)) return std::nullopt;
  if (!readField("z", viewport.zoom)) return std::nullopt;
  if (!readField("pdx", viewport.panDocX)) return std::nullopt;
  if (!readField("pdy", viewport.panDocY)) return std::nullopt;
  if (!readField("psx", viewport.panScreenX)) return std::nullopt;
  if (!readField("psy", viewport.panScreenY)) return std::nullopt;
  if (!readField("vbx", viewport.viewBoxX)) return std::nullopt;
  if (!readField("vby", viewport.viewBoxY)) return std::nullopt;
  if (!readField("vbw", viewport.viewBoxW)) return std::nullopt;
  if (!readField("vbh", viewport.viewBoxH)) return std::nullopt;
  return viewport;
}

std::optional<ReproExpectedCrop> ParseExpectedCropObject(std::string_view body) {
  ReproExpectedCrop crop;
  const auto readField = [&](std::string_view key, int& out) {
    auto r = FindKey(body, key);
    if (r.empty()) return false;
    auto v = ReadNumber(r);
    if (!v) return false;
    out = static_cast<int>(*v);
    return true;
  };

  if (!readField("x", crop.x)) return std::nullopt;
  if (!readField("y", crop.y)) return std::nullopt;
  if (!readField("w", crop.width)) return std::nullopt;
  if (!readField("h", crop.height)) return std::nullopt;
  return crop;
}

std::optional<ReproExpectation> ParseExpectationObject(std::string_view body) {
  ReproExpectation expect;
  const auto readIntField = [&](std::string_view key, int& out) {
    auto r = FindKey(body, key);
    if (r.empty()) return false;
    auto v = ReadNumber(r);
    if (!v) return false;
    out = static_cast<int>(*v);
    return true;
  };
  const auto readStringField = [&](std::string_view key, std::string& out) {
    auto r = FindKey(body, key);
    if (r.empty()) return false;
    auto value = ReadString(r);
    if (!value.has_value()) return false;
    out = std::move(*value);
    return true;
  };

  if (!readIntField("left_mouse_down_ordinal", expect.leftMouseDownOrdinal)) {
    return std::nullopt;
  }
  if (!readIntField("frame_offset_after_left_mouse_down", expect.frameOffsetAfterLeftMouseDown)) {
    return std::nullopt;
  }
  if (!readIntField("min_frame_index", expect.minFrameIndex)) return std::nullopt;
  if (!readIntField("max_frame_index", expect.maxFrameIndex)) return std::nullopt;
  if (!readStringField("target_selector", expect.targetSelector)) return std::nullopt;
  if (!readStringField("crop_mode", expect.cropMode)) return std::nullopt;

  auto cropRest = FindKey(body, "crop");
  if (!cropRest.empty()) {
    std::size_t p = 0;
    while (p < cropRest.size() && (cropRest[p] == ' ' || cropRest[p] == '\t')) ++p;
    if (p >= cropRest.size() || cropRest[p] != '{') return std::nullopt;
    std::string_view cropCursor = cropRest.substr(p + 1);
    std::string_view cropBody;
    if (!ExtractBalancedObject(cropCursor, cropBody)) return std::nullopt;
    auto crop = ParseExpectedCropObject(cropBody);
    if (!crop.has_value()) return std::nullopt;
    expect.cropRect = *crop;
  }

  return expect;
}

std::optional<ReproFrame> ParseFrameLine(std::string_view line) {
  ReproFrame frame;
  auto readIntField = [&](std::string_view key, auto& out) {
    auto r = FindKey(line, key);
    if (r.empty()) return false;
    auto v = ReadNumber(r);
    if (!v) return false;
    out = static_cast<std::remove_reference_t<decltype(out)>>(*v);
    return true;
  };
  auto readDoubleField = [&](std::string_view key, double& out) {
    auto r = FindKey(line, key);
    if (r.empty()) return false;
    auto v = ReadNumber(r);
    if (!v) return false;
    out = *v;
    return true;
  };
  if (!readIntField("f", frame.index)) return std::nullopt;
  if (!readDoubleField("t", frame.timestampSeconds)) return std::nullopt;
  if (!readDoubleField("dt", frame.deltaMs)) return std::nullopt;
  if (!readDoubleField("mx", frame.mouseX)) return std::nullopt;
  if (!readDoubleField("my", frame.mouseY)) return std::nullopt;
  int btn = 0;
  int mod = 0;
  if (!readIntField("btn", btn)) return std::nullopt;
  if (!readIntField("mod", mod)) return std::nullopt;
  frame.mouseButtonMask = btn;
  frame.modifiers = mod;

  double mouseDocX = 0.0;
  double mouseDocY = 0.0;
  const bool hasMouseDocX = readDoubleField("mdx", mouseDocX);
  const bool hasMouseDocY = readDoubleField("mdy", mouseDocY);
  if (hasMouseDocX != hasMouseDocY) return std::nullopt;
  if (hasMouseDocX) {
    frame.mouseDocX = mouseDocX;
    frame.mouseDocY = mouseDocY;
  }

  auto viewportRest = FindKey(line, "vp");
  if (!viewportRest.empty()) {
    std::size_t p = 0;
    while (p < viewportRest.size() && (viewportRest[p] == ' ' || viewportRest[p] == '\t')) ++p;
    if (p >= viewportRest.size() || viewportRest[p] != '{') return std::nullopt;
    std::string_view viewportCursor = viewportRest.substr(p + 1);
    std::string_view viewportBody;
    if (!ExtractBalancedObject(viewportCursor, viewportBody)) return std::nullopt;
    auto viewport = ParseViewportObject(viewportBody);
    if (!viewport.has_value()) {
      std::fprintf(stderr, "ReproFile: malformed `vp` block in frame %" PRIu64 "\n",
                   static_cast<std::uint64_t>(frame.index));
      return std::nullopt;
    }
    frame.viewport = std::move(*viewport);
  }

  auto eventsStart = FindKey(line, "e");
  if (!eventsStart.empty()) {
    // Find the opening '[' then parse objects separated by commas until ']'.
    std::size_t p = 0;
    while (p < eventsStart.size() && eventsStart[p] != '[') ++p;
    if (p >= eventsStart.size()) return std::nullopt;
    std::string_view cursor = eventsStart.substr(p + 1);
    while (!cursor.empty()) {
      std::size_t q = 0;
      while (q < cursor.size() && (cursor[q] == ' ' || cursor[q] == ',' || cursor[q] == '\t')) ++q;
      if (q >= cursor.size()) break;
      if (cursor[q] == ']') break;
      if (cursor[q] != '{') return std::nullopt;
      cursor.remove_prefix(q + 1);
      auto ev = ParseEventObject(cursor);
      if (!ev.has_value()) return std::nullopt;
      frame.events.push_back(*ev);
    }
  }
  return frame;
}

}  // namespace

bool WriteReproFile(const std::filesystem::path& path, const ReproFile& file) {
  const auto tmp = path.string() + ".tmp";
  {
    std::ofstream os(tmp, std::ios::binary);
    if (!os) {
      std::fprintf(stderr, "ReproFile: failed to open %s for write\n", tmp.c_str());
      return false;
    }
    WriteMetadataLine(os, file.metadata);
    for (const auto& frame : file.frames) {
      WriteFrameLine(os, frame);
    }
    if (!os) {
      std::fprintf(stderr, "ReproFile: failed to write to %s\n", tmp.c_str());
      return false;
    }
  }
  std::error_code ec;
  std::filesystem::rename(tmp, path, ec);
  if (ec) {
    std::fprintf(stderr, "ReproFile: failed to rename %s → %s: %s\n", tmp.c_str(),
                 path.string().c_str(), ec.message().c_str());
    std::filesystem::remove(tmp, ec);
    return false;
  }
  return true;
}

std::optional<ReproFile> ReadReproFile(const std::filesystem::path& path) {
  std::ifstream is(path, std::ios::binary);
  if (!is) {
    std::fprintf(stderr, "ReproFile: could not open %s for read\n", path.string().c_str());
    return std::nullopt;
  }
  ReproFile file;
  std::string line;
  bool gotMeta = false;
  int version = 0;
  while (std::getline(is, line)) {
    if (line.empty()) continue;
    const std::string_view view(line);
    if (!gotMeta) {
      // Metadata: `{"v":N,"svg":"...","wnd":[W,H],"scale":S,"exp":0|1,...}`.
      auto r = FindKey(view, "v");
      if (r.empty()) {
        std::fprintf(stderr, "ReproFile: first line missing `v` field\n");
        return std::nullopt;
      }
      auto vn = ReadNumber(r);
      if (!vn) return std::nullopt;
      version = static_cast<int>(*vn);
      if (version != 1 && version != kReproFileVersion) {
        std::fprintf(stderr, "ReproFile: version %d, expected 1 or %d\n", version,
                     kReproFileVersion);
        return std::nullopt;
      }
      ReproMetadata meta;
      auto svgRest = FindKey(view, "svg");
      if (!svgRest.empty()) {
        auto s = ReadString(svgRest);
        if (s) meta.svgPath = std::move(*s);
      }
      // wnd parsed as two numbers between `[` `]`.
      auto wndRest = FindKey(view, "wnd");
      if (!wndRest.empty()) {
        std::size_t p = 0;
        while (p < wndRest.size() && wndRest[p] != '[') ++p;
        if (p < wndRest.size()) {
          std::string_view cursor = wndRest.substr(p + 1);
          auto w = ReadNumber(cursor);
          while (!cursor.empty() && (cursor[0] == ',' || cursor[0] == ' ')) cursor.remove_prefix(1);
          auto h = ReadNumber(cursor);
          if (w && h) {
            meta.windowWidth = static_cast<int>(*w);
            meta.windowHeight = static_cast<int>(*h);
          }
        }
      }
      auto scaleRest = FindKey(view, "scale");
      if (!scaleRest.empty()) {
        auto v = ReadNumber(scaleRest);
        if (v) meta.displayScale = *v;
      }
      auto expRest = FindKey(view, "exp");
      if (!expRest.empty()) {
        auto v = ReadNumber(expRest);
        if (v) meta.experimentalMode = (*v != 0.0);
      }
      auto atRest = FindKey(view, "at");
      if (!atRest.empty()) {
        auto s = ReadString(atRest);
        if (s) meta.startedAtIso8601 = std::move(*s);
      }
      auto expectRest = FindKey(view, "expect");
      if (!expectRest.empty()) {
        std::size_t p = 0;
        while (p < expectRest.size() && (expectRest[p] == ' ' || expectRest[p] == '\t')) ++p;
        if (p >= expectRest.size() || expectRest[p] != '{') return std::nullopt;
        std::string_view expectCursor = expectRest.substr(p + 1);
        std::string_view expectBody;
        if (!ExtractBalancedObject(expectCursor, expectBody)) return std::nullopt;
        auto expect = ParseExpectationObject(expectBody);
        if (!expect.has_value()) {
          std::fprintf(stderr, "ReproFile: malformed `expect` metadata block\n");
          return std::nullopt;
        }
        meta.expect = std::move(*expect);
      }
      file.metadata = std::move(meta);
      gotMeta = true;
      continue;
    }
    auto frame = ParseFrameLine(view);
    if (!frame.has_value()) {
      std::fprintf(stderr, "ReproFile: malformed frame line: %s\n", line.c_str());
      return std::nullopt;
    }
    file.frames.push_back(*frame);
  }
  if (!gotMeta) {
    std::fprintf(stderr, "ReproFile: empty file\n");
    return std::nullopt;
  }
  return file;
}

}  // namespace donner::editor::repro
