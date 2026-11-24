#include "donner/svg/renderer/tests/SvgCorpus.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string_view>
#include <utility>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/Runfiles.h"
#include "donner/svg/parser/SVGParser.h"

namespace donner::svg::tests {
namespace {

void AddDocuments(const std::filesystem::path& root, std::string_view prefix,
                  std::vector<CorpusDocument>& documents) {
  for (const auto& entry : std::filesystem::recursive_directory_iterator(root)) {
    if (entry.path().extension() != ".svg") {
      continue;
    }
    std::string name(prefix);
    for (const char c : entry.path().lexically_relative(root).replace_extension().string()) {
      name.push_back(std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
    }
    documents.push_back({entry.path(), std::move(name)});
  }
}

}  // namespace

std::vector<CorpusDocument> CorpusDocuments() {
  std::vector<CorpusDocument> documents;
  AddDocuments(Runfiles::instance().Rlocation("third_party/resvg-test-suite/tests"), "resvg_",
               documents);
  AddDocuments(Runfiles::instance().Rlocation("donner/svg/renderer/testdata"), "testdata_",
               documents);
  documents.push_back({Runfiles::instance().Rlocation("donner_splash.svg"), "donner_splash"});
  documents.push_back(
      {Runfiles::instance().Rlocation("third_party/bootstrap-icons/bootstrap-icons.svg"),
       "bootstrap_icons"});
  std::sort(
      documents.begin(), documents.end(),
      [](const CorpusDocument& lhs, const CorpusDocument& rhs) { return lhs.name < rhs.name; });
  return documents;
}

std::optional<SVGDocument> ParseCorpusDocument(const CorpusDocument& document) {
  std::ifstream file(document.path, std::ios::binary);
  std::ostringstream source;
  source << file.rdbuf();
  ParseWarningSink warnings = ParseWarningSink::Disabled();
  auto parsed = parser::SVGParser::ParseSVG(source.str(), warnings);
  if (parsed.hasError()) {
    return std::nullopt;
  }
  return std::move(parsed.result());
}

}  // namespace donner::svg::tests
