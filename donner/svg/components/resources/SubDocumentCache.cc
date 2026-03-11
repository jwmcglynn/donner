#include "donner/svg/components/resources/SubDocumentCache.h"

namespace donner::svg::components {

std::any* SubDocumentCache::getOrParse(const RcString& resolvedUrl,
                                       const std::vector<uint8_t>& svgContent,
                                       const ParseCallback& parseCallback,
                                       std::vector<ParseError>* outWarnings) {
  // Check cache first.
  if (auto it = cache_.find(resolvedUrl); it != cache_.end()) {
    return &it->second;
  }

  // Detect circular references.
  if (loading_.contains(resolvedUrl)) {
    if (outWarnings) {
      ParseError err;
      err.reason = "Circular SVG sub-document reference detected: " + std::string(resolvedUrl);
      outWarnings->emplace_back(err);
    }
    return nullptr;
  }

  // Mark as loading to guard against recursion.
  loading_.insert(resolvedUrl);

  auto maybeDocument = parseCallback(svgContent, outWarnings);

  loading_.erase(resolvedUrl);

  if (!maybeDocument) {
    return nullptr;
  }

  auto [it, inserted] = cache_.emplace(resolvedUrl, std::move(*maybeDocument));
  return &it->second;
}

std::any* SubDocumentCache::get(const RcString& resolvedUrl) {
  if (auto it = cache_.find(resolvedUrl); it != cache_.end()) {
    return &it->second;
  }
  return nullptr;
}

bool SubDocumentCache::isLoading(const RcString& resolvedUrl) const {
  return loading_.contains(resolvedUrl);
}

}  // namespace donner::svg::components
