#include "donner/css/Declaration.h"

#include <unordered_map>

#include "donner/base/StringUtils.h"

namespace donner::css {

DeclarationOrAtRule::DeclarationOrAtRule(DeclarationOrAtRule::Type&& value)
    : value(std::move(value)) {}

bool DeclarationOrAtRule::operator==(const DeclarationOrAtRule& other) const {
  return value == other.value;
}

std::string Declaration::toCssText() const {
  std::string result = std::string(name) + ":";
  for (size_t i = 0; i < values.size(); ++i) {
    const std::string text = values[i].toCssText();
    // Ensure there's a space after the colon if the first value doesn't start with whitespace.
    if (i == 0 && !text.empty() && text[0] != ' ') {
      result += ' ';
    }
    result += text;
  }
  if (important) {
    result += " !important";
  }
  return result;
}

std::ostream& operator<<(std::ostream& os, const Declaration& declaration) {
  os << "  " << declaration.name << ":";
  for (const auto& value : declaration.values) {
    os << " " << value;
  }
  if (declaration.important) {
    os << " !important";
  }
  return os;
}

namespace {

/// Key that identifies a property for merging. Custom property names (`--*`) are case-sensitive;
/// other property names match ASCII case-insensitively.
std::string PropertyKey(const RcString& name) {
  if (std::string_view(name).starts_with("--")) {
    return std::string(name);
  }
  return StringUtils::ToLowerAscii(name);
}

}  // namespace

std::string mergeStyleDeclarations(std::span<const Declaration> existing,
                                   std::span<const Declaration> updates) {
  // Build set of update property names for O(1) lookup.
  std::unordered_map<std::string, size_t> updateIndexByName;
  updateIndexByName.reserve(updates.size());
  for (size_t i = 0; i < updates.size(); ++i) {
    // Keep the last occurrence of each property name in the updates.
    updateIndexByName[PropertyKey(updates[i].name)] = i;
  }

  // Collect existing declarations not overridden by updates.
  std::vector<const Declaration*> merged;
  merged.reserve(existing.size() + updateIndexByName.size());

  for (const auto& decl : existing) {
    if (!updateIndexByName.contains(PropertyKey(decl.name))) {
      merged.push_back(&decl);
    }
  }

  // Add deduplicated updates in order (but only the last occurrence of each name).
  // We need to preserve insertion order, so iterate updates and only emit if this index is the
  // last for its name.
  for (size_t i = 0; i < updates.size(); ++i) {
    const std::string key = PropertyKey(updates[i].name);
    if (updateIndexByName[key] == i) {
      merged.push_back(&updates[i]);
    }
  }

  // Serialize.
  std::string result;
  for (size_t i = 0; i < merged.size(); ++i) {
    if (i > 0) {
      result += "; ";
    }
    result += merged[i]->toCssText();
  }

  return result;
}

}  // namespace donner::css
