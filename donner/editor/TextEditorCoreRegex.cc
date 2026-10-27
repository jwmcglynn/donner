/// @file

#include <memory>
#include <regex>
#include <utility>
#include <vector>

#include "donner/editor/TextEditorCore.h"

namespace donner::editor {

void TextEditorCore::setLanguageDefinition(const LanguageDefinition& langDef) {
  using RegexList = std::vector<std::pair<std::regex, ColorIndex>>;
  auto compiled = std::make_shared<RegexList>();
  compiled->reserve(langDef.tokenRegexStrings.size());
  for (const auto& [pattern, color] : langDef.tokenRegexStrings) {
    compiled->emplace_back(std::regex(pattern, std::regex_constants::optimize), color);
  }

  LanguageDefinition::TokenizeCallback fallback;
  if (!compiled->empty()) {
    fallback = [patterns = std::shared_ptr<const RegexList>(std::move(compiled))](
                   const char* begin, const char* end, const char*& tokenBegin,
                   const char*& tokenEnd, ColorIndex& tokenColor) {
      std::cmatch results;
      for (const auto& [pattern, color] : *patterns) {
        if (std::regex_search(begin, end, results, pattern,
                              std::regex_constants::match_continuous)) {
          const auto& match = *results.begin();
          tokenBegin = match.first;
          tokenEnd = match.second;
          tokenColor = color;
          return true;
        }
      }
      return false;
    };
  }
  installPreparedLanguage(langDef, std::move(fallback));
}

}  // namespace donner::editor
