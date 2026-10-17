#pragma once
/// @file
/// Minimal streaming JSON writer for benchmark results. Produces compact single-line output so
/// each run appends exactly one JSON Lines record.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace donner::bakeoff {

/// Writes one JSON value. Keys and values must be emitted in valid order; the writer inserts
/// commas and escapes strings, and non-finite numbers become `null`.
class JsonWriter {
public:
  /// Opens an object, as a member value when \p key is non-empty.
  /// @param key Member key inside an enclosing object, or empty.
  JsonWriter& beginObject(std::string_view key = {});
  /// Closes the innermost object.
  JsonWriter& endObject();
  /// Opens an array, as a member value when \p key is non-empty.
  /// @param key Member key inside an enclosing object, or empty.
  JsonWriter& beginArray(std::string_view key = {});
  /// Closes the innermost array.
  JsonWriter& endArray();

  /// Writes a string member (or array element when \p key is empty).
  /// @param key Member key or empty. @param value Value.
  JsonWriter& string(std::string_view key, std::string_view value);
  /// Writes a floating-point member. @param key Member key or empty. @param value Value.
  JsonWriter& number(std::string_view key, double value);
  /// Writes an integer member. @param key Member key or empty. @param value Value.
  JsonWriter& integer(std::string_view key, std::int64_t value);
  /// Writes a boolean member. @param key Member key or empty. @param value Value.
  JsonWriter& boolean(std::string_view key, bool value);

  /// The JSON text written so far.
  [[nodiscard]] const std::string& str() const { return out_; }

private:
  void prefix(std::string_view key);
  void quoted(std::string_view value);

  std::string out_;
  std::vector<bool> needsComma_;
};

}  // namespace donner::bakeoff
