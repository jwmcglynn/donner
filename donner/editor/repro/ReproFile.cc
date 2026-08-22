#include "donner/editor/repro/ReproFile.h"

#include <algorithm>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>

#include "donner/base/FileUtils.h"
#include "donner/base/Utf8.h"
#include "donner/editor/ImGuiIncludes.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/resources/SandboxedFileResourceLoader.h"

namespace donner::editor::repro {

namespace {

constexpr double kMaximumReproScalarMagnitude = 1.0e9;
constexpr int kDefaultReproWindowWidth = 1600;
constexpr int kDefaultReproWindowHeight = 900;
constexpr size_t kMaximumReproNumberTokenBytes = 128;
constexpr size_t kMaximumReproShortStringBytes = 256;
constexpr size_t kMaximumReproIdentifierBytes = 4 * 1024;

class ReproParseBudget {
public:
  bool chargeScan(size_t bytes) {
    if (bytes > kMaximumReproScanWorkBytes - scanBytes_) {
      rejected_ = true;
      return false;
    }
    scanBytes_ += bytes;
    return true;
  }

  bool retainString(size_t bytes) {
    if (bytes > kMaximumReproRetainedStringBytes - stringBytes_) {
      rejected_ = true;
      return false;
    }
    stringBytes_ += bytes;
    return true;
  }

  size_t remainingStringBytes() const { return kMaximumReproRetainedStringBytes - stringBytes_; }

  void reject() { rejected_ = true; }
  bool rejected() const { return rejected_; }

private:
  size_t scanBytes_ = 0;
  size_t stringBytes_ = 0;
  bool rejected_ = false;
};

bool IsSafePixelSurface(double width, double height, double scale) {
  if (!(width >= 0.0) || !(height >= 0.0) || !(scale > 0.0) ||
      scale > kMaximumReproDevicePixelRatio) {
    return false;
  }
  const double pixelWidth = width * scale;
  const double pixelHeight = height * scale;
  return std::isfinite(pixelWidth) && std::isfinite(pixelHeight) &&
         pixelWidth <= kMaximumReproPixelDimension && pixelHeight <= kMaximumReproPixelDimension &&
         pixelWidth * pixelHeight <= static_cast<double>(kMaximumReproPixels);
}

bool IsSafeImGuiKey(int key) {
  return (key >= ImGuiKey_NamedKey_BEGIN && key < ImGuiKey_NamedKey_END) || key == ImGuiMod_Ctrl ||
         key == ImGuiMod_Shift || key == ImGuiMod_Alt || key == ImGuiMod_Super;
}

// Minimal string quoter for JSON. Handles ASCII-safe escapes we need
// for filenames, ISO timestamps, and event-type tags - does NOT attempt
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

const char* ActionKindTag(ReproAction::Kind kind) {
  switch (kind) {
    case ReproAction::Kind::SetActiveTool: return "active_tool";
    case ReproAction::Kind::SetStyleProperty: return "style";
    case ReproAction::Kind::CommitPenPath: return "commit_pen_path";
  }
  return "unknown";
}

std::optional<ReproAction::Kind> ParseActionKind(std::string_view tag) {
  if (tag == "active_tool") return ReproAction::Kind::SetActiveTool;
  if (tag == "style") return ReproAction::Kind::SetStyleProperty;
  if (tag == "commit_pen_path") return ReproAction::Kind::CommitPenPath;
  return std::nullopt;
}

const char* ProofKindTag(ReproExpectationProofKind kind) {
  switch (kind) {
    case ReproExpectationProofKind::PresentedPixels: return "presented-pixels";
    case ReproExpectationProofKind::ActiveDragAlignment: return "active-drag-alignment";
    case ReproExpectationProofKind::Selection: return "selection";
    case ReproExpectationProofKind::WorkerLiveness: return "worker-liveness";
  }
  return "presented-pixels";
}

std::optional<ReproExpectationProofKind> ParseProofKind(std::string_view tag) {
  if (tag == "presented-pixels") return ReproExpectationProofKind::PresentedPixels;
  if (tag == "active-drag-alignment") return ReproExpectationProofKind::ActiveDragAlignment;
  if (tag == "selection") return ReproExpectationProofKind::Selection;
  if (tag == "worker-liveness") return ReproExpectationProofKind::WorkerLiveness;
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

void WriteAction(std::ostream& os, const ReproAction& action) {
  os << "{\"k\":\"" << ActionKindTag(action.kind) << '"';
  switch (action.kind) {
    case ReproAction::Kind::SetActiveTool:
      os << ",\"tool\":";
      WriteQuotedJsonString(os, action.tool);
      break;
    case ReproAction::Kind::SetStyleProperty:
      os << ",\"p\":";
      WriteQuotedJsonString(os, action.propertyName);
      os << ",\"v\":";
      WriteQuotedJsonString(os, action.propertyValue);
      break;
    case ReproAction::Kind::CommitPenPath: break;
  }
  os << '}';
}

void WriteExpectation(std::ostream& os, const ReproExpectation& expect) {
  os << "\"expect\":{"
     << "\"proof_kind\":";
  WriteQuotedJsonString(os, ProofKindTag(expect.proofKind));
  os << ",\"left_mouse_down_ordinal\":" << expect.leftMouseDownOrdinal
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
  if (expect.activeFrameIndex.has_value()) {
    os << ",\"active_frame_index\":" << *expect.activeFrameIndex;
  }
  if (expect.comparisonFrameIndex.has_value()) {
    os << ",\"comparison_frame_index\":" << *expect.comparisonFrameIndex;
  }
  if (expect.expectedSelectionLabel.has_value()) {
    os << ",\"expected_selection_label\":";
    WriteQuotedJsonString(os, *expect.expectedSelectionLabel);
  }
  if (expect.statusStartFrameIndex.has_value()) {
    os << ",\"status_start_frame_index\":" << *expect.statusStartFrameIndex;
  }
  if (expect.statusMaxFrameIndex.has_value()) {
    os << ",\"status_max_frame_index\":" << *expect.statusMaxFrameIndex;
  }
  if (expect.forbiddenStatusSubstring.has_value()) {
    os << ",\"forbidden_status_substring\":";
    WriteQuotedJsonString(os, *expect.forbiddenStatusSubstring);
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
  if (!meta.svgBasename.empty()) {
    os << ",\"svg_base\":";
    WriteQuotedJsonString(os, meta.svgBasename);
  }
  if (!meta.svgContentHash.empty()) {
    os << ",\"svg_hash\":";
    WriteQuotedJsonString(os, meta.svgContentHash);
  }
  if (meta.svgSource.has_value()) {
    os << ",\"svg_src\":";
    WriteQuotedJsonString(os, *meta.svgSource);
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
  if (!frame.actions.empty()) {
    os << ",\"a\":[";
    for (std::size_t i = 0; i < frame.actions.size(); ++i) {
      if (i > 0) os << ',';
      WriteAction(os, frame.actions[i]);
    }
    os << ']';
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

// Finds a field at the current object's top level without interpreting key-like text in strings.
std::string_view FindKey(std::string_view object, std::string_view key, ReproParseBudget& budget) {
  size_t first = 0;
  while (first < object.size() && (object[first] == ' ' || object[first] == '\t')) {
    ++first;
  }
  const size_t targetDepth = first < object.size() && object[first] == '{' ? 1 : 0;

  size_t depth = 0;
  size_t i = 0;
  while (i < object.size()) {
    const char ch = object[i];
    if (ch == '"') {
      const size_t stringStart = ++i;
      bool escaped = false;
      while (i < object.size()) {
        if (object[i] == '\\') {
          escaped = true;
          i += i + 1 < object.size() ? 2 : 1;
        } else if (object[i] == '"') {
          break;
        } else {
          ++i;
        }
      }
      if (i >= object.size()) {
        budget.chargeScan(i);
        return {};
      }

      const size_t stringEnd = i;
      size_t valueStart = i + 1;
      while (valueStart < object.size() &&
             (object[valueStart] == ' ' || object[valueStart] == '\t')) {
        ++valueStart;
      }
      if (depth == targetDepth && !escaped &&
          object.substr(stringStart, stringEnd - stringStart) == key &&
          valueStart < object.size() && object[valueStart] == ':') {
        if (!budget.chargeScan(valueStart + 1)) return {};
        return object.substr(valueStart + 1);
      }
      i = stringEnd + 1;
      continue;
    }

    if (ch == '{' || ch == '[') {
      ++depth;
    } else if (ch == '}' || ch == ']') {
      if (depth == 0) {
        budget.chargeScan(i + 1);
        return {};
      }
      --depth;
    }
    ++i;
  }
  budget.chargeScan(i);
  return {};
}

std::optional<double> ReadNumber(std::string_view& cursor, ReproParseBudget& budget) {
  // Skip leading whitespace.
  std::size_t i = 0;
  while (i < cursor.size() && (cursor[i] == ' ' || cursor[i] == '\t')) ++i;
  const std::size_t start = i;
  if (i < cursor.size() && (cursor[i] == '-' || cursor[i] == '+')) ++i;
  while (i < cursor.size() &&
         ((cursor[i] >= '0' && cursor[i] <= '9') || cursor[i] == '.' || cursor[i] == 'e' ||
          cursor[i] == 'E' || cursor[i] == '+' || cursor[i] == '-')) {
    ++i;
    if (i - start > kMaximumReproNumberTokenBytes) {
      budget.chargeScan(i);
      budget.reject();
      return std::nullopt;
    }
  }
  if (i == start) return std::nullopt;
  if (!budget.chargeScan(i)) return std::nullopt;
  const std::string token(cursor.substr(start, i - start));
  char* endPtr = nullptr;
  errno = 0;
  const double value = std::strtod(token.c_str(), &endPtr);
  if (endPtr != token.c_str() + token.size() || errno != 0 || !std::isfinite(value)) {
    return std::nullopt;
  }
  cursor.remove_prefix(i);
  return value;
}

std::optional<double> ReadBoundedNumber(std::string_view& cursor, ReproParseBudget& budget) {
  auto value = ReadNumber(cursor, budget);
  if (!value || std::abs(*value) > kMaximumReproScalarMagnitude) {
    return std::nullopt;
  }
  return value;
}

template <typename T>
std::optional<T> ReadInteger(std::string_view& cursor, ReproParseBudget& budget) {
  static_assert(std::is_integral_v<T> && !std::is_same_v<T, bool>);
  auto value = ReadNumber(cursor, budget);
  if (!value || std::trunc(*value) != *value) {
    return std::nullopt;
  }

  // Comparing against an exclusive power-of-two upper bound avoids rounding
  // std::numeric_limits<uint64_t>::max() up to 2^64 when converted to double.
  const double exclusiveLimit = std::ldexp(1.0, std::numeric_limits<T>::digits);
  if constexpr (std::is_signed_v<T>) {
    if (*value < -exclusiveLimit || *value >= exclusiveLimit) {
      return std::nullopt;
    }
  } else if (*value < 0.0 || *value >= exclusiveLimit) {
    return std::nullopt;
  }
  return static_cast<T>(*value);
}

std::optional<float> ReadFloat(std::string_view& cursor, ReproParseBudget& budget) {
  auto value = ReadBoundedNumber(cursor, budget);
  if (!value || std::abs(*value) > static_cast<double>(std::numeric_limits<float>::max())) {
    return std::nullopt;
  }
  return static_cast<float>(*value);
}

std::optional<std::string> ReadString(std::string_view& cursor, size_t maximumBytes,
                                      ReproParseBudget& budget) {
  const auto hexValue = [](char c) -> int {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
  };
  const auto appendUtf8 = [](std::string& out, std::uint32_t codepoint, size_t maximumSize) {
    size_t encodedBytes = 1;
    if (codepoint > 0x7Fu) encodedBytes = codepoint <= 0x7FFu ? 2 : codepoint <= 0xFFFFu ? 3 : 4;
    if (encodedBytes > maximumSize - out.size()) return false;
    if (codepoint <= 0x7Fu) {
      out += static_cast<char>(codepoint);
    } else if (codepoint <= 0x7FFu) {
      out += static_cast<char>(0xC0u | (codepoint >> 6));
      out += static_cast<char>(0x80u | (codepoint & 0x3Fu));
    } else if (codepoint <= 0xFFFFu) {
      out += static_cast<char>(0xE0u | (codepoint >> 12));
      out += static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
      out += static_cast<char>(0x80u | (codepoint & 0x3Fu));
    } else {
      out += static_cast<char>(0xF0u | (codepoint >> 18));
      out += static_cast<char>(0x80u | ((codepoint >> 12) & 0x3Fu));
      out += static_cast<char>(0x80u | ((codepoint >> 6) & 0x3Fu));
      out += static_cast<char>(0x80u | (codepoint & 0x3Fu));
    }
    return true;
  };
  maximumBytes = std::min(maximumBytes, budget.remainingStringBytes());
  std::size_t i = 0;
  while (i < cursor.size() && (cursor[i] == ' ' || cursor[i] == '\t')) ++i;
  if (i >= cursor.size() || cursor[i] != '"') return std::nullopt;
  ++i;
  std::string out;
  bool containsNonAscii = false;
  while (i < cursor.size()) {
    const char c = cursor[i];
    if (c == '"') {
      if (!budget.chargeScan(i + 1) || !budget.chargeScan(out.size())) {
        return std::nullopt;
      }
      // ASCII is already valid UTF-8. Avoid a second full decode pass over common large source
      // strings after the bounded JSON scan has established that every byte is ASCII.
      if (containsNonAscii && !Utf8::IsValidString(out)) {
        budget.reject();
        return std::nullopt;
      }
      if (!budget.retainString(out.size())) return std::nullopt;
      cursor.remove_prefix(i + 1);
      return out;
    }
    if (c == '\\' && i + 1 < cursor.size()) {
      const char next = cursor[i + 1];
      switch (next) {
        case '"':
        case '\\':
        case '/':
        case 'b':
        case 'f':
        case 'n':
        case 't':
        case 'r': {
          if (out.size() == maximumBytes) {
            budget.reject();
            return std::nullopt;
          }
          switch (next) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            default: break;
          }
          break;
        }
        case 'u': {
          if (i + 5 >= cursor.size()) return std::nullopt;
          std::uint32_t codepoint = 0;
          for (std::size_t j = 0; j < 4; ++j) {
            const int value = hexValue(cursor[i + 2 + j]);
            if (value < 0) return std::nullopt;
            codepoint = (codepoint << 4) | static_cast<std::uint32_t>(value);
          }
          size_t encodedInputBytes = 6;
          if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
            if (i + 11 >= cursor.size() || cursor[i + 6] != '\\' || cursor[i + 7] != 'u') {
              budget.reject();
              return std::nullopt;
            }
            std::uint32_t lowSurrogate = 0;
            for (std::size_t j = 0; j < 4; ++j) {
              const int value = hexValue(cursor[i + 8 + j]);
              if (value < 0) return std::nullopt;
              lowSurrogate = (lowSurrogate << 4) | static_cast<std::uint32_t>(value);
            }
            if (lowSurrogate < 0xDC00u || lowSurrogate > 0xDFFFu) {
              budget.reject();
              return std::nullopt;
            }
            codepoint = 0x10000u + ((codepoint - 0xD800u) << 10) + (lowSurrogate - 0xDC00u);
            encodedInputBytes = 12;
          } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
            budget.reject();
            return std::nullopt;
          }
          if (!appendUtf8(out, codepoint, maximumBytes)) {
            budget.reject();
            return std::nullopt;
          }
          i += encodedInputBytes;
          continue;
        }
        default:
          if (out.size() == maximumBytes) {
            budget.reject();
            return std::nullopt;
          }
          containsNonAscii |= static_cast<unsigned char>(next) >= 0x80;
          out += next;
          break;
      }
      i += 2;
      continue;
    }
    if (static_cast<unsigned char>(c) < 0x20) {
      budget.reject();
      return std::nullopt;
    }
    if (out.size() == maximumBytes) {
      budget.reject();
      return std::nullopt;
    }
    containsNonAscii |= static_cast<unsigned char>(c) >= 0x80;
    out += c;
    ++i;
  }
  budget.chargeScan(i);
  return std::nullopt;
}

// Finds the matching closing brace for a `{` that `cursor` points
// immediately past, respecting string literals and nested objects.
bool ExtractBalancedObject(std::string_view& cursor, std::string_view& body,
                           ReproParseBudget& budget) {
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
  if (!budget.chargeScan(i) || depth != 0) return false;
  body = cursor.substr(0, i - 1);
  cursor.remove_prefix(i);
  return true;
}

std::optional<ReproHit> ParseHitObject(std::string_view body, ReproParseBudget& budget) {
  ReproHit hit;

  auto emptyRest = FindKey(body, "empty", budget);
  if (!emptyRest.empty()) {
    auto value = ReadInteger<int>(emptyRest, budget);
    if (!value || (*value != 0 && *value != 1)) return std::nullopt;
    hit.empty = *value != 0;
    if (hit.empty) return hit;
  }

  auto tagRest = FindKey(body, "tag", budget);
  if (!tagRest.empty()) {
    auto tag = ReadString(tagRest, kMaximumReproShortStringBytes, budget);
    if (!tag.has_value()) return std::nullopt;
    hit.tag = std::move(*tag);
  }
  auto idRest = FindKey(body, "id", budget);
  if (!idRest.empty()) {
    auto id = ReadString(idRest, kMaximumReproIdentifierBytes, budget);
    if (!id.has_value()) return std::nullopt;
    hit.id = std::move(*id);
  }
  auto idxRest = FindKey(body, "idx", budget);
  if (!idxRest.empty()) {
    auto index = ReadInteger<int>(idxRest, budget);
    if (!index) return std::nullopt;
    hit.docOrderIndex = *index;
  }
  return hit;
}

// Parses an event object starting at `cursor` pointing just past the `{`.
// Advances `cursor` past the matching `}`. Returns nullopt on malformed input.
std::optional<ReproEvent> ParseEventObject(std::string_view& cursor, ReproParseBudget& budget) {
  std::string_view body;
  if (!ExtractBalancedObject(cursor, body, budget)) return std::nullopt;

  ReproEvent ev;
  auto rest = FindKey(body, "k", budget);
  if (rest.empty()) return std::nullopt;
  auto kindStr = ReadString(rest, kMaximumReproShortStringBytes, budget);
  if (!kindStr.has_value()) return std::nullopt;
  auto kind = ParseEventKind(*kindStr);
  if (!kind.has_value()) return std::nullopt;
  ev.kind = *kind;

  const auto readIntField = [&](std::string_view key, int& out) {
    auto r = FindKey(body, key, budget);
    if (r.empty()) return;
    auto v = ReadInteger<int>(r, budget);
    if (!v) return;
    out = *v;
  };
  const auto readUintField = [&](std::string_view key, std::uint32_t& out) {
    auto r = FindKey(body, key, budget);
    if (r.empty()) return;
    auto v = ReadInteger<std::uint32_t>(r, budget);
    if (!v) return;
    out = *v;
  };
  const auto readFloatField = [&](std::string_view key, float& out) {
    auto r = FindKey(body, key, budget);
    if (r.empty()) return;
    auto v = ReadFloat(r, budget);
    if (!v) return;
    out = *v;
  };
  const auto readBoolField = [&](std::string_view key, bool& out) {
    auto r = FindKey(body, key, budget);
    if (r.empty()) return;
    auto v = ReadInteger<int>(r, budget);
    if (!v || (*v != 0 && *v != 1)) return;
    out = *v != 0;
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
  if ((ev.kind == ReproEvent::Kind::KeyDown || ev.kind == ReproEvent::Kind::KeyUp) &&
      !IsSafeImGuiKey(ev.key)) {
    return std::nullopt;
  }
  if (ev.width < 0 || ev.width > kMaximumReproDimension || ev.height < 0 ||
      ev.height > kMaximumReproDimension ||
      (ev.kind == ReproEvent::Kind::Resize && !IsSafePixelSurface(ev.width, ev.height, 1.0))) {
    ev.width = 0;
    ev.height = 0;
  }

  auto hitRest = FindKey(body, "hit", budget);
  if (!hitRest.empty()) {
    std::size_t p = 0;
    while (p < hitRest.size() && (hitRest[p] == ' ' || hitRest[p] == '\t')) ++p;
    if (p >= hitRest.size() || hitRest[p] != '{') return std::nullopt;
    std::string_view hitCursor = hitRest.substr(p + 1);
    std::string_view hitBody;
    if (!ExtractBalancedObject(hitCursor, hitBody, budget)) return std::nullopt;
    auto hit = ParseHitObject(hitBody, budget);
    if (!hit.has_value()) return std::nullopt;
    ev.hit = std::move(*hit);
  }

  return ev;
}

// Parses an action object starting at `cursor` pointing just past the `{`.
// Advances `cursor` past the matching `}`. Returns nullopt on malformed input.
std::optional<ReproAction> ParseActionObject(std::string_view& cursor, ReproParseBudget& budget) {
  std::string_view body;
  if (!ExtractBalancedObject(cursor, body, budget)) return std::nullopt;

  ReproAction action;
  auto rest = FindKey(body, "k", budget);
  if (rest.empty()) return std::nullopt;
  auto kindStr = ReadString(rest, kMaximumReproShortStringBytes, budget);
  if (!kindStr.has_value()) return std::nullopt;
  auto kind = ParseActionKind(*kindStr);
  if (!kind.has_value()) return std::nullopt;
  action.kind = *kind;

  const auto readStringField = [&](std::string_view key, size_t maximumBytes, std::string& out) {
    auto r = FindKey(body, key, budget);
    if (r.empty()) return false;
    auto value = ReadString(r, maximumBytes, budget);
    if (!value.has_value()) return false;
    out = std::move(*value);
    return true;
  };

  switch (action.kind) {
    case ReproAction::Kind::SetActiveTool:
      if (!readStringField("tool", kMaximumReproShortStringBytes, action.tool)) {
        return std::nullopt;
      }
      break;
    case ReproAction::Kind::SetStyleProperty:
      if (!readStringField("p", kMaximumReproShortStringBytes, action.propertyName)) {
        return std::nullopt;
      }
      if (!readStringField("v", kMaximumReproActionPropertyValueBytes, action.propertyValue)) {
        return std::nullopt;
      }
      break;
    case ReproAction::Kind::CommitPenPath: break;
  }

  return action;
}

std::optional<ReproViewport> ParseViewportObject(std::string_view body, ReproParseBudget& budget) {
  ReproViewport viewport;
  const auto readField = [&](std::string_view key, double& out) {
    auto r = FindKey(body, key, budget);
    if (r.empty()) return false;
    auto v = ReadBoundedNumber(r, budget);
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
  if (std::abs(viewport.paneOriginX) > kMaximumReproDimension ||
      std::abs(viewport.paneOriginY) > kMaximumReproDimension || viewport.paneSizeW < 0.0 ||
      viewport.paneSizeW > kMaximumReproDimension || viewport.paneSizeH < 0.0 ||
      viewport.paneSizeH > kMaximumReproDimension ||
      !IsSafePixelSurface(viewport.paneSizeW, viewport.paneSizeH, viewport.devicePixelRatio) ||
      viewport.zoom < 0.1 || viewport.zoom > 32.0) {
    return std::nullopt;
  }
  return viewport;
}

std::optional<ReproExpectedCrop> ParseExpectedCropObject(std::string_view body,
                                                         ReproParseBudget& budget) {
  ReproExpectedCrop crop;
  const auto readField = [&](std::string_view key, int& out) {
    auto r = FindKey(body, key, budget);
    if (r.empty()) return false;
    auto v = ReadInteger<int>(r, budget);
    if (!v) return false;
    out = *v;
    return true;
  };

  if (!readField("x", crop.x)) return std::nullopt;
  if (!readField("y", crop.y)) return std::nullopt;
  if (!readField("w", crop.width)) return std::nullopt;
  if (!readField("h", crop.height)) return std::nullopt;
  if (std::abs(static_cast<double>(crop.x)) > kMaximumReproScalarMagnitude ||
      std::abs(static_cast<double>(crop.y)) > kMaximumReproScalarMagnitude || crop.width < 0 ||
      crop.width > kMaximumReproDimension || crop.height < 0 ||
      crop.height > kMaximumReproDimension) {
    return std::nullopt;
  }
  return crop;
}

std::optional<ReproExpectation> ParseExpectationObject(std::string_view body,
                                                       ReproParseBudget& budget) {
  ReproExpectation expect;
  const auto readIntField = [&](std::string_view key, int& out) {
    auto r = FindKey(body, key, budget);
    if (r.empty()) return false;
    auto v = ReadInteger<int>(r, budget);
    if (!v) return false;
    out = *v;
    return true;
  };
  const auto readStringField = [&](std::string_view key, size_t maximumBytes, std::string& out) {
    auto r = FindKey(body, key, budget);
    if (r.empty()) return false;
    auto value = ReadString(r, maximumBytes, budget);
    if (!value.has_value()) return false;
    out = std::move(*value);
    return true;
  };
  const auto readOptionalIntField = [&](std::string_view key, std::optional<int>& out) {
    auto r = FindKey(body, key, budget);
    if (r.empty()) return true;
    auto v = ReadInteger<int>(r, budget);
    if (!v) return false;
    out = *v;
    return true;
  };
  const auto readOptionalStringField = [&](std::string_view key, size_t maximumBytes,
                                           std::optional<std::string>& out) {
    auto r = FindKey(body, key, budget);
    if (r.empty()) return true;
    auto value = ReadString(r, maximumBytes, budget);
    if (!value.has_value()) return false;
    out = std::move(*value);
    return true;
  };

  auto proofKindRest = FindKey(body, "proof_kind", budget);
  if (!proofKindRest.empty()) {
    auto proofKindString = ReadString(proofKindRest, kMaximumReproShortStringBytes, budget);
    if (!proofKindString.has_value()) return std::nullopt;
    auto proofKind = ParseProofKind(*proofKindString);
    if (!proofKind.has_value()) return std::nullopt;
    expect.proofKind = *proofKind;
  }

  if (!readIntField("left_mouse_down_ordinal", expect.leftMouseDownOrdinal)) {
    return std::nullopt;
  }
  if (!readIntField("frame_offset_after_left_mouse_down", expect.frameOffsetAfterLeftMouseDown)) {
    return std::nullopt;
  }
  if (!readIntField("min_frame_index", expect.minFrameIndex)) return std::nullopt;
  if (!readIntField("max_frame_index", expect.maxFrameIndex)) return std::nullopt;
  if (!readStringField("target_selector", kMaximumReproIdentifierBytes, expect.targetSelector)) {
    return std::nullopt;
  }
  if (!readStringField("crop_mode", kMaximumReproShortStringBytes, expect.cropMode)) {
    return std::nullopt;
  }

  auto cropRest = FindKey(body, "crop", budget);
  if (!cropRest.empty()) {
    std::size_t p = 0;
    while (p < cropRest.size() && (cropRest[p] == ' ' || cropRest[p] == '\t')) ++p;
    if (p >= cropRest.size() || cropRest[p] != '{') return std::nullopt;
    std::string_view cropCursor = cropRest.substr(p + 1);
    std::string_view cropBody;
    if (!ExtractBalancedObject(cropCursor, cropBody, budget)) return std::nullopt;
    auto crop = ParseExpectedCropObject(cropBody, budget);
    if (!crop.has_value()) return std::nullopt;
    expect.cropRect = *crop;
  }

  if (!readOptionalIntField("active_frame_index", expect.activeFrameIndex)) {
    return std::nullopt;
  }
  if (!readOptionalIntField("comparison_frame_index", expect.comparisonFrameIndex)) {
    return std::nullopt;
  }
  if (!readOptionalStringField("expected_selection_label", kMaximumReproIdentifierBytes,
                               expect.expectedSelectionLabel)) {
    return std::nullopt;
  }
  if (!readOptionalIntField("status_start_frame_index", expect.statusStartFrameIndex)) {
    return std::nullopt;
  }
  if (!readOptionalIntField("status_max_frame_index", expect.statusMaxFrameIndex)) {
    return std::nullopt;
  }
  if (!readOptionalStringField("forbidden_status_substring", kMaximumReproIdentifierBytes,
                               expect.forbiddenStatusSubstring)) {
    return std::nullopt;
  }
  if (expect.leftMouseDownOrdinal < 0 || expect.frameOffsetAfterLeftMouseDown < 0 ||
      expect.minFrameIndex < 0 || expect.maxFrameIndex < 0 ||
      expect.minFrameIndex > static_cast<int>(kMaximumReproFrames) ||
      expect.maxFrameIndex > static_cast<int>(kMaximumReproFrames)) {
    return std::nullopt;
  }

  return expect;
}

std::optional<ReproFrame> ParseFrameLine(std::string_view line, ReproParseBudget& budget) {
  ReproFrame frame;
  auto readIntField = [&](std::string_view key, auto& out) {
    auto r = FindKey(line, key, budget);
    if (r.empty()) return false;
    using FieldType = std::remove_reference_t<decltype(out)>;
    auto v = ReadInteger<FieldType>(r, budget);
    if (!v) return false;
    out = *v;
    return true;
  };
  auto readDoubleField = [&](std::string_view key, double& out) {
    auto r = FindKey(line, key, budget);
    if (r.empty()) return false;
    auto v = ReadBoundedNumber(r, budget);
    if (!v) return false;
    out = *v;
    return true;
  };
  if (!readIntField("f", frame.index)) return std::nullopt;
  if (frame.index < 0 || frame.index > static_cast<int>(kMaximumReproFrames)) return std::nullopt;
  if (!readDoubleField("t", frame.timestampSeconds)) return std::nullopt;
  if (!readDoubleField("dt", frame.deltaMs)) return std::nullopt;
  if (frame.timestampSeconds < 0.0 || frame.timestampSeconds > kMaximumReproDurationSeconds ||
      frame.deltaMs < 0.0 || frame.deltaMs > kMaximumReproDeltaMilliseconds) {
    return std::nullopt;
  }
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

  auto viewportRest = FindKey(line, "vp", budget);
  if (!viewportRest.empty()) {
    std::size_t p = 0;
    while (p < viewportRest.size() && (viewportRest[p] == ' ' || viewportRest[p] == '\t')) ++p;
    if (p >= viewportRest.size() || viewportRest[p] != '{') return std::nullopt;
    std::string_view viewportCursor = viewportRest.substr(p + 1);
    std::string_view viewportBody;
    if (!ExtractBalancedObject(viewportCursor, viewportBody, budget)) return std::nullopt;
    auto viewport = ParseViewportObject(viewportBody, budget);
    if (!viewport.has_value()) {
      std::fprintf(stderr, "ReproFile: malformed `vp` block in frame %" PRIu64 "\n",
                   static_cast<std::uint64_t>(frame.index));
      return std::nullopt;
    }
    frame.viewport = std::move(*viewport);
  }

  auto actionsStart = FindKey(line, "a", budget);
  if (!actionsStart.empty()) {
    // Find the opening '[' then parse objects separated by commas until ']'.
    std::size_t p = 0;
    while (p < actionsStart.size() && actionsStart[p] != '[') ++p;
    if (p >= actionsStart.size()) return std::nullopt;
    std::string_view cursor = actionsStart.substr(p + 1);
    while (!cursor.empty()) {
      std::size_t q = 0;
      while (q < cursor.size() && (cursor[q] == ' ' || cursor[q] == ',' || cursor[q] == '\t')) {
        ++q;
      }
      if (q >= cursor.size()) break;
      if (cursor[q] == ']') break;
      if (cursor[q] != '{') return std::nullopt;
      cursor.remove_prefix(q + 1);
      auto action = ParseActionObject(cursor, budget);
      if (!action.has_value()) return std::nullopt;
      if (frame.actions.size() >= kMaximumReproItemsPerFrame) return std::nullopt;
      frame.actions.push_back(*action);
    }
  }

  auto eventsStart = FindKey(line, "e", budget);
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
      auto ev = ParseEventObject(cursor, budget);
      if (!ev.has_value()) return std::nullopt;
      if (frame.events.size() >= kMaximumReproItemsPerFrame) return std::nullopt;
      frame.events.push_back(*ev);
    }
  }
  return frame;
}

}  // namespace

std::string ReproSvgDisplayName(const ReproMetadata& metadata) {
  const std::string_view candidate = !metadata.svgBasename.empty()
                                         ? std::string_view(metadata.svgBasename)
                                         : std::string_view(metadata.svgPath);
  if (candidate.empty() || candidate.size() > kMaximumReproSvgPathBytes ||
      candidate.find('\0') != std::string_view::npos || !Utf8::IsValidString(candidate)) {
    return "embedded.svg";
  }

  const std::size_t separator = candidate.find_last_of("/\\");
  const std::string_view basename =
      separator == std::string_view::npos ? candidate : candidate.substr(separator + 1);
  if (basename.empty() || basename == "." || basename == "..") {
    return "embedded.svg";
  }
  return std::string(basename);
}

bool IsSafeReproSvgPath(std::string_view pathText) {
  if (pathText.empty() || pathText.size() > kMaximumReproSvgPathBytes ||
      pathText.find('\0') != std::string_view::npos || !Utf8::IsValidString(pathText)) {
    return false;
  }

  std::size_t componentCount = 0;
  bool inComponent = false;
  for (const char ch : pathText) {
    if (ch == '/' || ch == '\\') {
      inComponent = false;
    } else if (!inComponent) {
      inComponent = true;
      if (++componentCount > kMaximumReproSvgPathComponents) {
        return false;
      }
    }
  }

  const std::filesystem::path path(pathText);
  if (path.is_absolute() || path.has_root_name() || path.has_root_directory()) {
    return false;
  }
  for (const std::filesystem::path& component : path) {
    if (component == "..") {
      return false;
    }
  }
  return path.lexically_normal() != ".";
}

std::optional<ReproSvgFile> ReadReproSvgFile(const std::filesystem::path& rnrPath,
                                             std::string_view recordedSvgPath) {
  if (!IsSafeReproSvgPath(recordedSvgPath)) {
    return std::nullopt;
  }

  std::error_code pathError;
  const std::filesystem::path absoluteRnrPath = std::filesystem::absolute(rnrPath, pathError);
  if (pathError) {
    return std::nullopt;
  }
  const std::filesystem::path root = absoluteRnrPath.parent_path();
  if (!std::filesystem::is_directory(root, pathError) || pathError) {
    return std::nullopt;
  }

  const std::filesystem::path relativePath =
      std::filesystem::path(recordedSvgPath).lexically_normal();
  svg::SandboxedFileResourceLoader loader(root, absoluteRnrPath);
  auto readResult = loader.fetchExternalResource(relativePath.generic_string());
  const auto* bytes = std::get_if<std::vector<uint8_t>>(&readResult);
  if (bytes == nullptr) {
    return std::nullopt;
  }

  return ReproSvgFile{
      .path = root / relativePath,
      .contents = std::string(bytes->begin(), bytes->end()),
  };
}

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

std::optional<ReproFile> ParseReproFile(std::string_view contents) {
  if (contents.size() > kMaximumReproFileSize) {
    return std::nullopt;
  }
  ReproFile file;
  ReproParseBudget budget;
  bool gotMeta = false;
  int version = 0;
  size_t itemCount = 0;
  size_t pixelFrameCount = 0;
  int replayWindowWidth = kDefaultReproWindowWidth;
  int replayWindowHeight = kDefaultReproWindowHeight;
  double replayDisplayScale = 1.0;
  std::optional<double> previousTimestamp;
  std::string_view remaining = contents;
  while (!remaining.empty()) {
    const size_t newline = remaining.find('\n');
    const std::string_view view = remaining.substr(0, newline);
    remaining =
        newline == std::string_view::npos ? std::string_view() : remaining.substr(newline + 1);
    if (view.empty()) continue;
    const size_t maximumLineBytes =
        gotMeta ? kMaximumReproFrameLineBytes : kMaximumReproMetadataLineBytes;
    if (view.size() > maximumLineBytes || !budget.chargeScan(view.size())) {
      return std::nullopt;
    }
    if (!gotMeta) {
      // Metadata: `{"v":N,"svg":"...","wnd":[W,H],"scale":S,"exp":0|1,...}`.
      auto r = FindKey(view, "v", budget);
      if (r.empty()) {
        std::fprintf(stderr, "ReproFile: first line missing `v` field\n");
        return std::nullopt;
      }
      auto vn = ReadInteger<int>(r, budget);
      if (!vn) return std::nullopt;
      version = *vn;
      if (version < 1 || version > kReproFileVersion) {
        std::fprintf(stderr, "ReproFile: version %d, expected 1 through %d\n", version,
                     kReproFileVersion);
        return std::nullopt;
      }
      ReproMetadata meta;
      auto svgRest = FindKey(view, "svg", budget);
      if (!svgRest.empty()) {
        auto s = ReadString(svgRest, kMaximumReproSvgPathBytes, budget);
        if (s) meta.svgPath = std::move(*s);
        if (budget.rejected()) return std::nullopt;
      }
      // wnd parsed as two numbers between `[` `]`.
      auto wndRest = FindKey(view, "wnd", budget);
      if (!wndRest.empty()) {
        std::size_t p = 0;
        while (p < wndRest.size() && wndRest[p] != '[') ++p;
        if (p < wndRest.size()) {
          std::string_view cursor = wndRest.substr(p + 1);
          auto w = ReadInteger<int>(cursor, budget);
          while (!cursor.empty() && (cursor[0] == ',' || cursor[0] == ' ')) {
            cursor.remove_prefix(1);
          }
          auto h = ReadInteger<int>(cursor, budget);
          if (w && h && *w >= 0 && *w <= kMaximumReproDimension && *h >= 0 &&
              *h <= kMaximumReproDimension) {
            meta.windowWidth = *w;
            meta.windowHeight = *h;
          }
        }
      }
      auto scaleRest = FindKey(view, "scale", budget);
      if (!scaleRest.empty()) {
        auto v = ReadBoundedNumber(scaleRest, budget);
        if (v && *v > 0.0 && *v <= 64.0) meta.displayScale = *v;
      }
      auto expRest = FindKey(view, "exp", budget);
      if (!expRest.empty()) {
        auto v = ReadInteger<int>(expRest, budget);
        if (v && (*v == 0 || *v == 1)) meta.experimentalMode = *v != 0;
      }
      const int effectiveWidth = meta.windowWidth > 0 ? meta.windowWidth : kDefaultReproWindowWidth;
      const int effectiveHeight =
          meta.windowHeight > 0 ? meta.windowHeight : kDefaultReproWindowHeight;
      if (!IsSafePixelSurface(effectiveWidth, effectiveHeight, meta.displayScale)) {
        meta.windowWidth = 0;
        meta.windowHeight = 0;
        meta.displayScale = 1.0;
      }
      auto atRest = FindKey(view, "at", budget);
      if (!atRest.empty()) {
        auto s = ReadString(atRest, kMaximumReproShortStringBytes, budget);
        if (s) meta.startedAtIso8601 = std::move(*s);
        if (budget.rejected()) return std::nullopt;
      }
      auto svgBaseRest = FindKey(view, "svg_base", budget);
      if (!svgBaseRest.empty()) {
        auto s = ReadString(svgBaseRest, kMaximumReproSvgPathBytes, budget);
        if (s) meta.svgBasename = std::move(*s);
        if (budget.rejected()) return std::nullopt;
      }
      auto svgHashRest = FindKey(view, "svg_hash", budget);
      if (!svgHashRest.empty()) {
        auto s = ReadString(svgHashRest, kMaximumReproShortStringBytes, budget);
        if (s) meta.svgContentHash = std::move(*s);
        if (budget.rejected()) return std::nullopt;
      }
      auto svgSourceRest = FindKey(view, "svg_src", budget);
      if (!svgSourceRest.empty()) {
        auto s =
            ReadString(svgSourceRest, svg::parser::SVGParser::kDefaultMaximumInputSize, budget);
        if (s) meta.svgSource = std::move(*s);
        if (budget.rejected()) return std::nullopt;
      }
      if (!meta.svgSource.has_value() && !meta.svgPath.empty() &&
          !IsSafeReproSvgPath(meta.svgPath)) {
        return std::nullopt;
      }
      auto expectRest = FindKey(view, "expect", budget);
      if (!expectRest.empty()) {
        std::size_t p = 0;
        while (p < expectRest.size() && (expectRest[p] == ' ' || expectRest[p] == '\t')) ++p;
        if (p >= expectRest.size() || expectRest[p] != '{') return std::nullopt;
        std::string_view expectCursor = expectRest.substr(p + 1);
        std::string_view expectBody;
        if (!ExtractBalancedObject(expectCursor, expectBody, budget)) return std::nullopt;
        auto expect = ParseExpectationObject(expectBody, budget);
        if (!expect.has_value()) {
          std::fprintf(stderr, "ReproFile: malformed `expect` metadata block\n");
          return std::nullopt;
        }
        meta.expect = std::move(*expect);
      }
      if (budget.rejected()) return std::nullopt;
      file.metadata = std::move(meta);
      replayWindowWidth =
          file.metadata.windowWidth > 0 ? file.metadata.windowWidth : kDefaultReproWindowWidth;
      replayWindowHeight =
          file.metadata.windowHeight > 0 ? file.metadata.windowHeight : kDefaultReproWindowHeight;
      replayDisplayScale = file.metadata.displayScale;
      gotMeta = true;
      continue;
    }
    auto frame = ParseFrameLine(view, budget);
    if (!frame.has_value()) {
      std::fprintf(stderr, "ReproFile: malformed frame line (%zu bytes)\n", view.size());
      return std::nullopt;
    }
    if (budget.rejected()) return std::nullopt;
    if (previousTimestamp && frame->timestampSeconds < *previousTimestamp) {
      return std::nullopt;
    }
    if (file.frames.size() >= kMaximumReproFrames ||
        frame->actions.size() + frame->events.size() > kMaximumReproItems - itemCount) {
      return std::nullopt;
    }
    if (!IsSafePixelSurface(replayWindowWidth, replayWindowHeight, replayDisplayScale)) {
      return std::nullopt;
    }
    const size_t physicalWidth =
        static_cast<size_t>(std::ceil(replayWindowWidth * replayDisplayScale));
    const size_t physicalHeight =
        static_cast<size_t>(std::ceil(replayWindowHeight * replayDisplayScale));
    if (physicalWidth != 0 && physicalHeight > kMaximumParsedReproPixelFrames / physicalWidth) {
      return std::nullopt;
    }
    const size_t framePixels = physicalWidth * physicalHeight;
    if (framePixels > kMaximumParsedReproPixelFrames - pixelFrameCount) {
      return std::nullopt;
    }
    pixelFrameCount += framePixels;
    itemCount += frame->actions.size() + frame->events.size();
    previousTimestamp = frame->timestampSeconds;
    file.frames.push_back(*frame);
  }
  if (!gotMeta) {
    std::fprintf(stderr, "ReproFile: empty file\n");
    return std::nullopt;
  }
  if (budget.rejected()) return std::nullopt;
  return file;
}

std::optional<ReproFile> ReadReproFile(const std::filesystem::path& path) {
  FileReadResult result = ReadFileBounded(path, kMaximumReproFileSize);
  const auto* contents = std::get_if<std::string>(&result);
  if (contents == nullptr) {
    std::fprintf(stderr, "ReproFile: %s: %s\n",
                 FileReadErrorMessage(std::get<FileReadError>(result)), path.string().c_str());
    return std::nullopt;
  }
  return ParseReproFile(*contents);
}

}  // namespace donner::editor::repro
