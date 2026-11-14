#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/css/parser/StylesheetParser.h"
#include "donner/svg/components/resources/StylesheetImportLoader.h"

namespace donner::svg::components {

/**
 * Fuzzer entry point, see https://llvm.org/docs/LibFuzzer.html.
 *
 * The input is split at NUL bytes into chunks. The first chunk is the root stylesheet; each fetch
 * returns the next chunk in turn (cycling), so fuzzed sheets can import each other, recurse,
 * repeat URLs and exhaust budgets. Invariants: the budget never exceeds its initial values, each
 * fetch respects the byte allowance it was offered, and the number of loaded sheets is bounded.
 */
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  // NOLINTNEXTLINE: Allow reinterpret_cast
  const std::string_view buffer(reinterpret_cast<const char*>(data), size);
  std::vector<std::string_view> chunks;
  size_t start = 0;
  while (start <= buffer.size()) {
    const size_t end = std::min(buffer.find('\0', start), buffer.size());
    chunks.push_back(buffer.substr(start, end - start));
    start = end + 1;
  }

  ParseWarningSink parseWarnings = ParseWarningSink::Disabled();
  const css::Stylesheet root = css::parser::StylesheetParser::Parse(chunks.front(), parseWarnings);

  size_t fetches = 0;
  const StylesheetImportFetch fetch = [&](std::string_view url, size_t maximumBytes)
      -> std::variant<UrlLoader::Result, RcString> {
    if (url.empty() || maximumBytes > StylesheetImportBudget::kMaximumBytes) {
      std::abort();
    }
    if (chunks.size() == 1) {
      ++fetches;
      return RcString("not found");
    }
    const std::string_view body = chunks[1 + fetches++ % (chunks.size() - 1)];
    if (body.size() > maximumBytes) {
      return RcString("too large");
    }
    UrlLoader::Result result;
    result.data.assign(body.begin(), body.end());
    return result;
  };

  StylesheetImportBudget budget;
  ParseWarningSink warnings;
  const std::vector<css::Stylesheet> imported = LoadStylesheetImports(root, fetch, budget, warnings);
  if (fetches > StylesheetImportBudget::kMaximumImports ||
      imported.size() > StylesheetImportBudget::kMaximumImports ||
      budget.remainingImports > StylesheetImportBudget::kMaximumImports ||
      budget.remainingBytes > StylesheetImportBudget::kMaximumBytes) {
    std::abort();
  }
  return 0;
}

}  // namespace donner::svg::components
