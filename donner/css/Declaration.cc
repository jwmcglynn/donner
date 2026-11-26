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

bool ReplacesStyleDeclaration(const Declaration& previous, const Declaration& next) {
  return next.important || !previous.important || !std::string_view(next.name).starts_with("--");
}

}  // namespace

std::string mergeStyleDeclarations(std::span<const Declaration> existing,
                                   std::span<const Declaration> updates) {
  // Build set of update property names for O(1) lookup.
  std::unordered_map<std::string, size_t> updateIndexByName;
  updateIndexByName.reserve(updates.size());
  for (size_t i = 0; i < updates.size(); ++i) {
    // Custom properties retain importance before using source order.
    const auto [it, inserted] = updateIndexByName.try_emplace(PropertyKey(updates[i].name), i);
    if (!inserted && ReplacesStyleDeclaration(updates[it->second], updates[i])) {
      it->second = i;
    }
  }

  // Collect existing declarations not overridden by updates.
  std::vector<const Declaration*> merged;
  merged.reserve(existing.size() + updateIndexByName.size());

  for (const auto& decl : existing) {
    const auto update = updateIndexByName.find(PropertyKey(decl.name));
    if (update == updateIndexByName.end() ||
        !ReplacesStyleDeclaration(decl, updates[update->second])) {
      merged.push_back(&decl);
    }
  }

  // Emit the selected occurrences in source order.
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
