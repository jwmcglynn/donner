#include "donner/svg/components/resources/SubDocumentCache.h"

namespace donner::svg::components {

std::optional<SVGDocumentHandle> SubDocumentCache::getOrParse(
    const RcString& resolvedUrl, const std::vector<uint8_t>& svgContent,
    const ParseCallback& parseCallback, ParseWarningSink& warningSink) {
  // Check cache first.
  if (auto it = cache_.find(resolvedUrl); it != cache_.end()) {
    return it->second;
  }
  if (isRejected(resolvedUrl)) {
    return std::nullopt;
  }

  // Detect circular references.
  if (loading_.contains(resolvedUrl)) {
    ParseDiagnostic err;
    err.reason = "Circular SVG sub-document reference detected: " + std::string(resolvedUrl);
    warningSink.add(std::move(err));
    return std::nullopt;
  }

  // Mark as loading to guard against recursion.
  loading_.insert(resolvedUrl);

  auto maybeDocument = parseCallback(svgContent, warningSink);

  loading_.erase(resolvedUrl);

  if (!maybeDocument) {
    rememberFailure(resolvedUrl);
    return std::nullopt;
  }

  auto [it, inserted] = cache_.emplace(resolvedUrl, std::move(*maybeDocument));
  return it->second;
}

void SubDocumentCache::rememberFailure(const RcString& resolvedUrl) {
  if (cache_.contains(resolvedUrl) || isRejected(resolvedUrl)) {
    return;
  }
  if (rejected_.size() >= kMaximumRejectedEntries) {
    rejectAll_ = true;
    return;
  }
  rejected_.insert(resolvedUrl);
}

bool SubDocumentCache::isRejected(const RcString& resolvedUrl) const {
  return rejectAll_ || rejected_.contains(resolvedUrl);
}

void SubDocumentCache::clearFailures() {
  rejected_.clear();
  rejectAll_ = false;
}

std::optional<SVGDocumentHandle> SubDocumentCache::get(const RcString& resolvedUrl) const {
  if (auto it = cache_.find(resolvedUrl); it != cache_.end()) {
    return it->second;
  }
  return std::nullopt;
}

bool SubDocumentCache::isLoading(const RcString& resolvedUrl) const {
  return loading_.contains(resolvedUrl);
}

}  // namespace donner::svg::components
