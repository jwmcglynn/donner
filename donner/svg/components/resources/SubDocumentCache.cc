#include "donner/svg/components/resources/SubDocumentCache.h"

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/ParsedPayloadResourceBudget.h"

namespace donner::svg::components {

bool SubDocumentCache::admissionAvailable() const {
  if (securityStats_.rejected || cache_.size() >= limits_.maximumDocuments ||
      securityStats_.parseAttempts >= limits_.maximumParseAttempts) {
    return false;
  }
  if (rootEntityCount_ > limits_.maximumAggregateEntities ||
      securityStats_.entities > limits_.maximumAggregateEntities - rootEntityCount_) {
    return false;
  }
  return rootPayloadBytes_ <= limits_.maximumAggregatePayloadBytes &&
         securityStats_.payloadBytes <= limits_.maximumAggregatePayloadBytes - rootPayloadBytes_;
}

bool SubDocumentCache::candidateFits(size_t entityCount, size_t payloadBytes) const {
  const size_t retainedBefore = rootEntityCount_ + securityStats_.entities;
  const size_t payloadBefore = rootPayloadBytes_ + securityStats_.payloadBytes;
  return entityCount <= limits_.maximumAggregateEntities - retainedBefore &&
         payloadBytes <= limits_.maximumAggregatePayloadBytes - payloadBefore;
}

std::optional<SVGDocumentHandle> SubDocumentCache::getOrParse(
    const RcString& resolvedUrl, const std::vector<uint8_t>& svgContent,
    const ParseCallback& parseCallback, ParseWarningSink& warningSink) {
  // Check cache first.
  if (auto it = cache_.find(resolvedUrl); it != cache_.end()) {
    return it->second;
  }
  if (isRejected(resolvedUrl)) {
    ++securityStats_.negativeCacheHits;
    return std::nullopt;
  }

  if (!admissionAvailable()) {
    securityStats_.rejected = true;
    rememberFailure(resolvedUrl);
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

  ++securityStats_.parseAttempts;
  auto maybeDocument = parseCallback(svgContent, warningSink);

  loading_.erase(resolvedUrl);

  if (!maybeDocument) {
    rememberFailure(resolvedUrl);
    return std::nullopt;
  }

  const size_t entityCount = (*maybeDocument)->registry().storage<Entity>().size();
  const auto* payloadBudget =
      (*maybeDocument)->registry().ctx().find<ParsedPayloadResourceBudget>();
  const size_t payloadBytes = payloadBudget ? payloadBudget->securityStats().retainedBytes : 0;
  if (!candidateFits(entityCount, payloadBytes)) {
    securityStats_.rejected = true;
    rememberFailure(resolvedUrl);
    return std::nullopt;
  }

  auto [it, inserted] = cache_.emplace(resolvedUrl, std::move(*maybeDocument));
  if (inserted) {
    securityStats_.documents = cache_.size();
    securityStats_.entities += entityCount;
    securityStats_.payloadBytes += payloadBytes;
  }
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
