#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <sstream>
#include <string>
#include <string_view>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/parser/StylesheetParser.h"

namespace donner::css::parser {
namespace {

void Require(bool condition, std::source_location location = std::source_location::current()) {
  if (!condition) {
    std::fprintf(stderr, "Stylesheet splice mismatch at assertion line %u\n", location.line());
    std::abort();
  }
}

void CheckComponent(const ComponentValue& incremental, const ComponentValue& full) {
  Require(incremental == full && incremental.sourceOffset() == full.sourceOffset() &&
          incremental.authoredRange() == full.authoredRange());
  std::visit(
      [&](const auto& value) {
        if constexpr (requires { value.values; }) {
          const auto& other = std::get<std::remove_cvref_t<decltype(value)>>(full.value);
          for (std::size_t i = 0; i < value.values.size(); ++i) {
            CheckComponent(value.values[i], other.values[i]);
          }
        }
      },
      incremental.value);
}

void CheckArguments(const std::optional<std::vector<ComponentValue>>& incremental,
                    const std::optional<std::vector<ComponentValue>>& full) {
  Require(incremental == full);
  if (incremental) {
    for (std::size_t i = 0; i < incremental->size(); ++i) {
      CheckComponent((*incremental)[i], (*full)[i]);
    }
  }
}

void CheckSelector(const Selector& incremental, const Selector& full);

void CheckSimpleSelector(const CompoundSelector::Entry& actual,
                         const CompoundSelector::Entry& expected) {
  Require(actual.index() == expected.index());
  if (const auto* pseudo = std::get_if<PseudoClassSelector>(&actual)) {
    const auto& other = std::get<PseudoClassSelector>(expected);
    Require(pseudo->ident == other.ident);
    Require(pseudo->anbValueIfAnb == other.anbValueIfAnb);
    CheckArguments(pseudo->argsIfFunction, other.argsIfFunction);
    Require(static_cast<bool>(pseudo->selector) == static_cast<bool>(other.selector));
    if (pseudo->selector) {
      CheckSelector(*pseudo->selector, *other.selector);
    }
  } else if (const auto* pseudo = std::get_if<PseudoElementSelector>(&actual)) {
    const auto& other = std::get<PseudoElementSelector>(expected);
    Require(pseudo->ident == other.ident);
    CheckArguments(pseudo->argsIfFunction, other.argsIfFunction);
  } else {
    std::ostringstream a, b;
    std::visit([&](const auto& simple) { a << simple; }, actual);
    std::visit([&](const auto& simple) { b << simple; }, expected);
    Require(a.str() == b.str());
  }
}

void CheckSelector(const Selector& incremental, const Selector& full) {
  Require(incremental.entries.size() == full.entries.size());
  for (std::size_t i = 0; i < incremental.entries.size(); ++i) {
    const auto& chain = incremental.entries[i].entries;
    const auto& fullChain = full.entries[i].entries;
    Require(chain.size() == fullChain.size());
    for (std::size_t j = 0; j < chain.size(); ++j) {
      Require(chain[j].combinator == fullChain[j].combinator);
      const auto& compound = chain[j].compoundSelector.entries;
      const auto& fullCompound = fullChain[j].compoundSelector.entries;
      Require(compound.size() == fullCompound.size());
      for (std::size_t k = 0; k < compound.size(); ++k) {
        CheckSimpleSelector(compound[k], fullCompound[k]);
      }
    }
  }
}

void CheckRule(const SelectorRule& actual, const SelectorRule& expected) {
  CheckSelector(actual.selector, expected.selector);
  Require(actual.ruleSourceRange == expected.ruleSourceRange &&
          actual.selectorSourceRange == expected.selectorSourceRange &&
          actual.selectorEntrySourceRanges == expected.selectorEntrySourceRanges &&
          actual.declarations == expected.declarations && actual.layer == expected.layer);
  for (std::size_t j = 0; j < actual.declarations.size(); ++j) {
    const auto& local = actual.declarations[j];
    const auto& reference = expected.declarations[j];
    Require(local.authoredNameRange() == reference.authoredNameRange());
    Require(local.authoredValueRange() == reference.authoredValueRange());
    Require(local.authoredPriorityRange() == reference.authoredPriorityRange());
    Require(local.authoredDeclarationRange() == reference.authoredDeclarationRange());
    Require(local.sourceByteSize == reference.sourceByteSize &&
            local.variableReferences == reference.variableReferences);
    for (std::size_t k = 0; k < local.values.size(); ++k) {
      CheckComponent(local.values[k], reference.values[k]);
    }
  }
}

void CheckStylesheet(const Stylesheet& incremental, const Stylesheet& full) {
  const auto& a = incremental.securityStats();
  const auto& b = full.securityStats();
  Require(a.rules == b.rules && a.declarations == b.declarations &&
          a.componentValues == b.componentValues && a.rawRules == b.rawRules &&
          a.rejected == b.rejected && a.hasImportRule == b.hasImportRule);
  Require(!incremental.layers() && !full.layers());
  Require(incremental.imports().empty() && full.imports().empty());
  Require(incremental.fontFaces().empty() && full.fontFaces().empty());
  Require(incremental.rules().size() == full.rules().size());
  for (std::size_t i = 0; i < incremental.rules().size(); ++i) {
    CheckRule(incremental.rules()[i], full.rules()[i]);
  }
}

Stylesheet ApplyTextEditOrFullParse(const Stylesheet& current, std::string_view before,
                                    std::string_view after, StylesheetParser::TextEdit edit,
                                    ParseWarningSink& warnings) {
  Stylesheet fresh = StylesheetParser::Parse(after, warnings);
  const auto incremental = StylesheetParser::TryReparseTextEdit(current, before, after, edit);
  if (!incremental) {
    return fresh;
  }
  CheckStylesheet(*incremental, fresh);
  return *incremental;
}

std::size_t U16(const uint8_t* bytes) {
  return std::size_t(bytes[0]) | (std::size_t(bytes[1]) << 8);
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, std::size_t size) {
  if (size < 6 || size > 8192) {
    return 0;
  }
  const std::string_view payload(reinterpret_cast<const char*>(data + 6), size - 6);
  const std::size_t split = U16(data) % (payload.size() + 1);
  const auto before = payload.substr(0, split);
  const auto inserted = payload.substr(split);
  const std::size_t offset = U16(data + 2) % (before.size() + 1);
  const std::size_t removed = U16(data + 4) % (before.size() - offset + 1);
  const std::string removedText(before.substr(offset, removed));
  std::string after(before);
  after.replace(offset, removed, inserted);

  ParseWarningSink warnings = ParseWarningSink::Disabled();
  Stylesheet current = StylesheetParser::Parse(before, warnings);
  current = ApplyTextEditOrFullParse(current, before, after, {offset, removed, inserted.size()},
                                     warnings);

  std::string restored(after);
  restored.replace(offset, inserted.size(), removedText);
  Require(restored == before);
  (void)ApplyTextEditOrFullParse(current, after, restored,
                                 {offset, inserted.size(), removedText.size()}, warnings);
  return 0;
}

}  // namespace donner::css::parser
