#include "donner/svg/components/resources/SubDocumentCache.h"

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/resources/SubDocumentAdmission.h"

namespace donner::svg::components {
namespace {

struct FamilyParse {
  bool bound;
  std::shared_ptr<SubDocumentAdmission> admission;
  std::unique_ptr<SubDocumentAdmission::Reservation> reservation;
  bool allowed = true;
};

FamilyParse BeginFamilyParse(bool bound, const std::shared_ptr<SubDocumentAdmission>& admission) {
  FamilyParse result{bound, admission, admission ? admission->beginParse() : nullptr};
  result.allowed = !bound || result.reservation != nullptr;
  return result;
}

bool FamilyAdmissionMatches(FamilyParse& parse, bool bound,
                            const std::shared_ptr<SubDocumentAdmission>& current,
                            const SVGDocumentHandle& document) {
  if (parse.bound != bound) {
    return false;
  }
  if (!bound) {
    return true;
  }
  if (!parse.admission || !current || !parse.reservation) {
    return false;
  }
  return current->admit(document, parse.reservation.get());
}

bool HasDocument(const std::optional<SVGDocumentHandle>& document) {
  return document.has_value() && *document;
}

struct DocumentFootprint {
  size_t entities;
  size_t payloadBytes;
};

DocumentFootprint MeasureDocument(const SVGDocumentHandle& document) {
  const auto* payload = document->registry().ctx().find<ParsedPayloadResourceBudget>();
  return {document->registry().storage<Entity>().size(),
          payload ? payload->securityStats().retainedBytes : 0};
}

}  // namespace

bool SubDocumentCache::acceptsCachedDocument(const SVGDocumentHandle& document) const {
  return !familyBound_ || (admission_ && admission_->admit(document));
}

bool SubDocumentCache::bindFamily(Registry& registry) {
  familyBound_ = true;
  admission_ = SubDocumentAdmission::Bind(registry, limits_);
  if (!admission_) {
    securityStats_.rejected = true;
  }
  return admission_ != nullptr;
}

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
  if (cache_.size() >= limits_.maximumDocuments ||
      rootEntityCount_ > limits_.maximumAggregateEntities ||
      securityStats_.entities > limits_.maximumAggregateEntities - rootEntityCount_ ||
      rootPayloadBytes_ > limits_.maximumAggregatePayloadBytes ||
      securityStats_.payloadBytes > limits_.maximumAggregatePayloadBytes - rootPayloadBytes_) {
    return false;
  }
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
    if (!acceptsCachedDocument(it->second)) {
      return std::nullopt;
    }
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

  auto familyParse = BeginFamilyParse(familyBound_, admission_);
  if (!familyParse.allowed) {
    securityStats_.rejected = true;
    rememberFailure(resolvedUrl);
    return std::nullopt;
  }

  // Mark as loading to guard against recursion.
  loading_.insert(resolvedUrl);

  ++securityStats_.parseAttempts;
  auto maybeDocument = parseCallback(svgContent, warningSink);

  loading_.erase(resolvedUrl);

  if (!HasDocument(maybeDocument)) {
    rememberFailure(resolvedUrl);
    return std::nullopt;
  }

  const auto footprint = MeasureDocument(*maybeDocument);
  if (!candidateFits(footprint.entities, footprint.payloadBytes)) {
    securityStats_.rejected = true;
    rememberFailure(resolvedUrl);
    return std::nullopt;
  }

  if (!FamilyAdmissionMatches(familyParse, familyBound_, admission_, *maybeDocument)) {
    securityStats_.rejected = true;
    rememberFailure(resolvedUrl);
    return std::nullopt;
  }

  auto [it, inserted] = cache_.emplace(resolvedUrl, std::move(*maybeDocument));
  if (inserted) {
    securityStats_.documents = cache_.size();
    securityStats_.entities += footprint.entities;
    securityStats_.payloadBytes += footprint.payloadBytes;
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
    if (!acceptsCachedDocument(it->second)) {
      return std::nullopt;
    }
    return it->second;
  }
  return std::nullopt;
}

bool SubDocumentCache::isLoading(const RcString& resolvedUrl) const {
  return loading_.contains(resolvedUrl);
}

}  // namespace donner::svg::components
