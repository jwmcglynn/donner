#include "donner/editor/repro/ReproFile.h"

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

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
      case '"':
        os << "\\\"";
        break;
      case '\\':
        os << "\\\\";
        break;
      case '\b':
        os << "\\b";
        break;
      case '\f':
        os << "\\f";
        break;
      case '\n':
        os << "\\n";
        break;
      case '\r':
        os << "\\r";
        break;
      case '\t':
        os << "\\t";
        break;
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
    case ReproEvent::Kind::MouseDown:
      return "mdown";
    case ReproEvent::Kind::MouseUp:
      return "mup";
    case ReproEvent::Kind::KeyDown:
      return "kdown";
    case ReproEvent::Kind::KeyUp:
      return "kup";
    case ReproEvent::Kind::Char:
      return "chr";
    case ReproEvent::Kind::Wheel:
      return "wheel";
    case ReproEvent::Kind::Resize:
      return "resize";
    case ReproEvent::Kind::Focus:
      return "focus";
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

void WriteEvent(std::ostream& os, const ReproEvent& ev) {
  os << "{\"k\":\"" << EventKindTag(ev.kind) << '"';
  switch (ev.kind) {
    case ReproEvent::Kind::MouseDown:
    case ReproEvent::Kind::MouseUp:
      os << ",\"b\":" << ev.mouseButton;
      break;
    case ReproEvent::Kind::KeyDown:
    case ReproEvent::Kind::KeyUp:
      os << ",\"key\":" << ev.key << ",\"m\":" << ev.modifiers;
      break;
    case ReproEvent::Kind::Char:
      os << ",\"c\":" << ev.codepoint;
      break;
    case ReproEvent::Kind::Wheel:
      os << ",\"dx\":" << ev.wheelDeltaX << ",\"dy\":" << ev.wheelDeltaY;
      break;
    case ReproEvent::Kind::Resize:
      os << ",\"w\":" << ev.width << ",\"h\":" << ev.height;
      break;
    case ReproEvent::Kind::Focus:
      os << ",\"on\":" << (ev.focusOn ? 1 : 0);
      break;
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
  os << "}\n";
}

void WriteFrameLine(std::ostream& os, const ReproFrame& frame) {
  os << "{\"f\":" << frame.index << ",\"t\":" << frame.timestampSeconds << ",\"dt\":" << frame.deltaMs
     << ",\"mx\":" << frame.mouseX << ",\"my\":" << frame.mouseY
     << ",\"btn\":" << frame.mouseButtonMask << ",\"mod\":" << frame.modifiers;
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
        case '"':
          out += '"';
          break;
        case '\\':
          out += '\\';
          break;
        case 'n':
          out += '\n';
          break;
        case 't':
          out += '\t';
          break;
        case 'r':
          out += '\r';
          break;
        default:
          out += next;
          break;
      }
      i += 2;
      continue;
    }
    out += c;
    ++i;
  }
  return std::nullopt;
}

// Parses an event object starting at `cursor` pointing just past the `{`.
// Advances `cursor` past the matching `}`. Returns nullopt on malformed input.
std::optional<ReproEvent> ParseEventObject(std::string_view& cursor) {
  // Find the closing brace respecting nested braces.
  int depth = 1;
  std::size_t i = 0;
  while (i < cursor.size() && depth > 0) {
    if (cursor[i] == '"') {
      ++i;
      while (i < cursor.size() && cursor[i] != '"') {
        if (cursor[i] == '\\' && i + 1 < cursor.size()) i += 2;
        else ++i;
      }
      if (i < cursor.size()) ++i;
      continue;
    }
    if (cursor[i] == '{') ++depth;
    else if (cursor[i] == '}') --depth;
    ++i;
  }
  if (depth != 0) return std::nullopt;
  std::string_view body = cursor.substr(0, i - 1);
  cursor.remove_prefix(i);

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

  return ev;
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
  while (std::getline(is, line)) {
    if (line.empty()) continue;
    const std::string_view view(line);
    if (!gotMeta) {
      // Metadata: `{"v":N,"svg":"...","wnd":[W,H],"scale":S,"exp":0|1,...}`.
      int version = 0;
      auto r = FindKey(view, "v");
      if (r.empty()) {
        std::fprintf(stderr, "ReproFile: first line missing `v` field\n");
        return std::nullopt;
      }
      auto vn = ReadNumber(r);
      if (!vn) return std::nullopt;
      version = static_cast<int>(*vn);
      if (version != kReproFileVersion) {
        std::fprintf(stderr, "ReproFile: version %d, expected %d\n", version, kReproFileVersion);
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
