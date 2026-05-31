#include "donner/editor/ShapeClipboardPayload.h"

#include <charconv>
#include <sstream>
#include <string>

#include "donner/base/parser/NumberParser.h"

namespace donner::editor {

namespace {

/// Trim ASCII whitespace from both ends of \p text.
std::string_view trim(std::string_view text) {
  const auto notSpace = [](char c) { return c != ' ' && c != '\t' && c != '\r' && c != '\n'; };
  size_t begin = 0;
  while (begin < text.size() && !notSpace(text[begin])) {
    ++begin;
  }
  size_t end = text.size();
  while (end > begin && !notSpace(text[end - 1])) {
    --end;
  }
  return text.substr(begin, end - begin);
}

/// Join \p ids with commas, escaping commas/backslashes so the list round-trips.
std::string encodeIdList(const std::vector<std::string>& ids) {
  std::string out;
  for (size_t i = 0; i < ids.size(); ++i) {
    if (i != 0) {
      out.push_back(',');
    }
    for (char c : ids[i]) {
      if (c == '\\' || c == ',') {
        out.push_back('\\');
      }
      out.push_back(c);
    }
  }
  return out;
}

/// Inverse of \ref encodeIdList.
std::vector<std::string> decodeIdList(std::string_view encoded) {
  std::vector<std::string> ids;
  if (encoded.empty()) {
    return ids;
  }
  std::string current;
  bool escaped = false;
  for (char c : encoded) {
    if (escaped) {
      current.push_back(c);
      escaped = false;
    } else if (c == '\\') {
      escaped = true;
    } else if (c == ',') {
      ids.push_back(std::move(current));
      current.clear();
    } else {
      current.push_back(c);
    }
  }
  ids.push_back(std::move(current));
  return ids;
}

/// Format a double with full round-trip precision.
std::string formatDouble(double value) {
  char buffer[32];
  auto [ptr, ec] = std::to_chars(buffer, buffer + sizeof(buffer), value);
  if (ec != std::errc()) {
    return "0";
  }
  return std::string(buffer, ptr);
}

/// Parse a double, returning std::nullopt on failure.
std::optional<double> parseDouble(std::string_view text) {
  text = trim(text);
  if (text.empty()) {
    return std::nullopt;
  }
  // `std::from_chars` for floating point is availability-gated on Apple libc++
  // (unavailable before macOS 26.0), so use Donner's locale-independent SVG
  // number parser. Require the whole trimmed token to be consumed, matching the
  // previous strict-parse semantics.
  const auto result = ::donner::parser::NumberParser::Parse(text);
  if (result.hasError() || result.result().consumedChars != text.size()) {
    return std::nullopt;
  }
  return result.result().number;
}

}  // namespace

std::string ShapeClipboardPayload::toClipboardText() const {
  std::ostringstream oss;
  oss << kHeader << '\n';
  if (documentBounds) {
    oss << "bounds: " << formatDouble(documentBounds->topLeft.x) << ' '
        << formatDouble(documentBounds->topLeft.y) << ' '
        << formatDouble(documentBounds->bottomRight.x) << ' '
        << formatDouble(documentBounds->bottomRight.y) << '\n';
  }
  oss << "ids: " << encodeIdList(sourceElementIds) << '\n';
  oss << "group-selection: " << (wasGroupSelection ? "1" : "0") << '\n';
  // Blank line terminates the metadata block; everything after is the fragment.
  oss << '\n';
  oss << svgFragment;
  return oss.str();
}

std::optional<ShapeClipboardPayload> ShapeClipboardPayload::parse(std::string_view clipboardText) {
  // Best-effort path: no header means treat the whole text as an SVG fragment.
  if (trim(clipboardText).empty()) {
    return std::nullopt;
  }

  // Detect and strip the header line.
  std::string_view rest = clipboardText;
  const std::string_view leading = trim(clipboardText.substr(0, kHeader.size() + 2));
  const bool hasHeader = leading.substr(0, kHeader.size()) == kHeader;
  if (!hasHeader) {
    ShapeClipboardPayload payload;
    payload.svgFragment = std::string(clipboardText);
    return payload;
  }

  // Advance past the header line.
  const size_t firstNewline = rest.find('\n');
  if (firstNewline == std::string_view::npos) {
    // Header only; no fragment.
    return std::nullopt;
  }
  rest.remove_prefix(firstNewline + 1);

  ShapeClipboardPayload payload;

  // Parse metadata lines until the blank-line terminator.
  while (true) {
    const size_t newline = rest.find('\n');
    std::string_view line = (newline == std::string_view::npos) ? rest : rest.substr(0, newline);
    const std::string_view trimmedLine = trim(line);
    if (trimmedLine.empty()) {
      // Blank line terminates metadata; consume it and stop.
      if (newline != std::string_view::npos) {
        rest.remove_prefix(newline + 1);
      } else {
        rest = std::string_view();
      }
      break;
    }

    const size_t colon = trimmedLine.find(':');
    if (colon != std::string_view::npos) {
      const std::string_view key = trim(trimmedLine.substr(0, colon));
      const std::string_view value = trim(trimmedLine.substr(colon + 1));
      if (key == "bounds") {
        std::istringstream iss{std::string(value)};
        std::string a;
        std::string b;
        std::string c;
        std::string d;
        if (iss >> a >> b >> c >> d) {
          auto x0 = parseDouble(a);
          auto y0 = parseDouble(b);
          auto x1 = parseDouble(c);
          auto y1 = parseDouble(d);
          if (x0 && y0 && x1 && y1) {
            payload.documentBounds = Box2d(Vector2d(*x0, *y0), Vector2d(*x1, *y1));
          }
        }
      } else if (key == "ids") {
        payload.sourceElementIds = decodeIdList(value);
      } else if (key == "group-selection") {
        payload.wasGroupSelection = (value == "1" || value == "true");
      }
    }

    if (newline == std::string_view::npos) {
      rest = std::string_view();
      break;
    }
    rest.remove_prefix(newline + 1);
  }

  payload.svgFragment = std::string(rest);
  if (trim(payload.svgFragment).empty()) {
    return std::nullopt;
  }
  return payload;
}

}  // namespace donner::editor
