#include "harness/JsonWriter.h"

#include <cmath>
#include <cstdio>

namespace donner::bakeoff {

void JsonWriter::prefix(std::string_view key) {
  if (!needsComma_.empty()) {
    if (needsComma_.back()) {
      out_ += ',';
    }
    needsComma_.back() = true;
  }
  if (!key.empty()) {
    quoted(key);
    out_ += ':';
  }
}

void JsonWriter::quoted(std::string_view value) {
  out_ += '"';
  for (const char c : value) {
    switch (c) {
      case '"': out_ += "\\\""; break;
      case '\\': out_ += "\\\\"; break;
      case '\n': out_ += "\\n"; break;
      case '\r': out_ += "\\r"; break;
      case '\t': out_ += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char escaped[8];
          std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned char>(c));
          out_ += escaped;
        } else {
          out_ += c;
        }
    }
  }
  out_ += '"';
}

JsonWriter& JsonWriter::beginObject(std::string_view key) {
  prefix(key);
  out_ += '{';
  needsComma_.push_back(false);
  return *this;
}

JsonWriter& JsonWriter::endObject() {
  out_ += '}';
  needsComma_.pop_back();
  return *this;
}

JsonWriter& JsonWriter::beginArray(std::string_view key) {
  prefix(key);
  out_ += '[';
  needsComma_.push_back(false);
  return *this;
}

JsonWriter& JsonWriter::endArray() {
  out_ += ']';
  needsComma_.pop_back();
  return *this;
}

JsonWriter& JsonWriter::string(std::string_view key, std::string_view value) {
  prefix(key);
  quoted(value);
  return *this;
}

JsonWriter& JsonWriter::number(std::string_view key, double value) {
  prefix(key);
  if (!std::isfinite(value)) {
    out_ += "null";
    return *this;
  }
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%.6g", value);
  out_ += buffer;
  return *this;
}

JsonWriter& JsonWriter::integer(std::string_view key, std::int64_t value) {
  prefix(key);
  out_ += std::to_string(value);
  return *this;
}

JsonWriter& JsonWriter::boolean(std::string_view key, bool value) {
  prefix(key);
  out_ += value ? "true" : "false";
  return *this;
}

}  // namespace donner::bakeoff
