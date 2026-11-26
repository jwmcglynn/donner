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
  const bool respectsImportance = std::string_view(next.name).starts_with("--") ||
                                  next.variableReferences == VariableReferences::Valid ||
                                  previous.variableReferences == VariableReferences::Valid;
  return next.important || !previous.important || !respectsImportance;
}

std::unordered_map<std::string, size_t> SelectStyleUpdates(std::span<const Declaration> updates) {
  // Build set of update property names for O(1) lookup.
  std::unordered_map<std::string, size_t> updateIndexByName;
  updateIndexByName.reserve(updates.size());
  for (size_t i = 0; i < updates.size(); ++i) {
    if (updates[i].variableReferences == VariableReferences::Invalid) {
      continue;
    }
    // Custom properties and ordinary variable values retain importance before source order.
    const auto [it, inserted] = updateIndexByName.try_emplace(PropertyKey(updates[i].name), i);
    if (!inserted && ReplacesStyleDeclaration(updates[it->second], updates[i])) {
      it->second = i;
    }
  }

  return updateIndexByName;
}

}  // namespace

std::string mergeStyleDeclarations(std::span<const Declaration> existing,
                                   std::span<const Declaration> updates,
                                   std::span<const std::string_view> existingSpellings,
                                   std::span<const std::string_view> updateSpellings) {
  const auto updateIndexByName = SelectStyleUpdates(updates);

  std::string result;
  const auto append = [&](const Declaration& declaration,
                          std::span<const std::string_view> spellings, std::size_t i) {
    if (!result.empty()) {
      result += "; ";
    }
    if (i < spellings.size()) {
      result += spellings[i];
    } else {
      result += declaration.toCssText();
    }
  };
  for (std::size_t i = 0; i < existing.size(); ++i) {
    const auto& decl = existing[i];
    const auto update = updateIndexByName.find(PropertyKey(decl.name));
    if (update == updateIndexByName.end() ||
        !ReplacesStyleDeclaration(decl, updates[update->second])) {
      append(decl, existingSpellings, i);
    }
  }

  // Emit the selected occurrences in source order.
  for (size_t i = 0; i < updates.size(); ++i) {
    const std::string key = PropertyKey(updates[i].name);
    const auto selected = updateIndexByName.find(key);
    if (selected != updateIndexByName.end() && selected->second == i) {
      append(updates[i], updateSpellings, i);
    }
  }

  return result;
}

}  // namespace donner::css
