#include "donner/svg/renderer/SubDocumentWarnings.h"

#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/renderer/ExternalUseStyleRetention.h"
#include "donner/svg/renderer/RenderingContext.h"

namespace donner::svg::details {

namespace {

/**
 * Preparation warnings a sub-document has already delivered to a host sink. A child document is
 * prepared again for each external-use occurrence and each frame, and those preparations can repeat
 * the same warnings. Each warning, matched by its text and location, is delivered once per loaded
 * sub-document instead, so identical warnings without a location are merged.
 */
struct ReportedSubDocumentWarnings {
  std::unordered_set<std::string> keys;
  /// Whether the host sink was marked truncated for this sub-document's warnings.
  bool truncationReported = false;

  /// Whether delivering @p key would only drop it, after the truncation it causes was reported.
  bool dropsAfterTruncation(const std::string& key) const {
    return truncationReported && keys.size() >= ParseWarningSink::kMaximumWarnings &&
           !keys.contains(key);
  }
};

/**
 * Warnings a sub-document found while no collecting host sink was installed, held for the next
 * draw with one. A retained render tree is not prepared again, so it does not find them again.
 * Held warnings are matched like \ref ReportedSubDocumentWarnings and bounded by the sink's limits.
 * Present only while it holds a warning or a truncation mark.
 */
struct PendingSubDocumentWarnings {
  ParseWarningSink warnings;
  std::unordered_set<std::string> keys;
  /// The sub-document's mutation revision when the warnings were found.
  std::uint64_t documentRevision = 0;
};

/// Marks a sub-document whose held warnings a mutation dropped, so the next draw with a collecting
/// host sink restyles it to find its current warnings.
struct RediscoverSubDocumentWarnings {};

/// The last warnings a preparation without a collecting host sink found, once each of them was
/// delivered or held, so finding exactly the same warnings again needs no matching or formatting.
struct HeldSubDocumentWarningBatch {
  std::vector<ParseDiagnostic> warnings;  //!< The batch's warnings, in order.
  bool truncated = false;                 //!< Whether the batch was truncated.

  /// Whether @p batch holds exactly these warnings and truncation.
  bool matches(const ParseWarningSink& batch) const {
    return truncated == batch.resourceLimitExceeded() && warnings == batch.warnings();
  }
};

std::string SubDocumentWarningKey(const ParseDiagnostic& warning) {
  std::ostringstream key;
  key << warning;
  return std::move(key).str();
}

std::uint64_t SubDocumentRevision(const Registry& registry) {
  return registry.ctx().get<components::SVGDocumentContext>().mutationRevision();
}

/// The held set @p pending points to, created for the current revision if there is none yet.
PendingSubDocumentWarnings& EnsureHeldWarnings(Registry& registry,
                                               PendingSubDocumentWarnings*& pending) {
  if (pending == nullptr) {
    PendingSubDocumentWarnings fresh;
    fresh.documentRevision = SubDocumentRevision(registry);
    pending = &registry.ctx().insert_or_assign(std::move(fresh));
  }
  return *pending;
}

/**
 * Hold @p warning unless it was already delivered or held, or delivering it would only drop it,
 * creating the held set on first use.
 *
 * @return True when @p warning is new but delivering it would only drop it.
 */
bool HoldSubDocumentWarning(Registry& registry, const ReportedSubDocumentWarnings* reported,
                            PendingSubDocumentWarnings*& pending, const ParseDiagnostic& warning) {
  std::string key = SubDocumentWarningKey(warning);
  if (reported != nullptr && reported->dropsAfterTruncation(key)) {
    return true;
  }
  if ((reported != nullptr && reported->keys.contains(key)) ||
      (pending != nullptr && pending->keys.contains(key))) {
    return false;
  }
  PendingSubDocumentWarnings& held = EnsureHeldWarnings(registry, pending);
  const std::size_t count = held.warnings.warnings().size();
  held.warnings.add(ParseDiagnostic(warning));
  if (held.warnings.warnings().size() > count) {
    held.keys.insert(std::move(key));
  }
  return false;
}

}  // namespace

void DropStaleSubDocumentWarnings(Registry& registry) {
  const auto* pending = registry.ctx().find<PendingSubDocumentWarnings>();
  if (pending == nullptr || pending->documentRevision == SubDocumentRevision(registry)) {
    return;
  }
  registry.ctx().erase<PendingSubDocumentWarnings>();
  registry.ctx().erase<HeldSubDocumentWarningBatch>();
  registry.ctx().insert_or_assign(RediscoverSubDocumentWarnings{});
  components::ForgetMissingFallbackFontReport(registry);
}


void ReportSubDocumentWarnings(Registry& registry, const ParseWarningSink& warnings,
                               ParseWarningSink& hostSink) {
  auto& reported = registry.ctx().emplace<ReportedSubDocumentWarnings>();
  bool truncated = warnings.resourceLimitExceeded();
  for (const ParseDiagnostic& warning : warnings.warnings()) {
    std::string key = SubDocumentWarningKey(warning);
    if (reported.keys.contains(key)) {
      continue;
    }
    if (reported.keys.size() >= ParseWarningSink::kMaximumWarnings) {
      truncated = true;
      continue;
    }
    reported.keys.insert(std::move(key));
    hostSink.add(ParseDiagnostic(warning));
  }
  if (truncated) {
    hostSink.markResourceLimitExceeded();
    reported.truncationReported = true;
  }
}


void HoldSubDocumentWarnings(Registry& registry, const ParseWarningSink& warnings) {
  if (!warnings.hasWarnings() && !warnings.resourceLimitExceeded()) {
    return;
  }
  DropStaleSubDocumentWarnings(registry);
  const auto* last = registry.ctx().find<HeldSubDocumentWarningBatch>();
  if (last != nullptr && last->matches(warnings)) {
    return;
  }
  const auto* reported = registry.ctx().find<ReportedSubDocumentWarnings>();
  auto* pending = registry.ctx().find<PendingSubDocumentWarnings>();
  bool dropsNewWarning = false;
  for (const ParseDiagnostic& warning : warnings.warnings()) {
    if (pending != nullptr && pending->warnings.resourceLimitExceeded()) {
      break;
    }
    dropsNewWarning |= HoldSubDocumentWarning(registry, reported, pending, warning);
  }
  // A truncation the host already received is held again only when this batch found a warning it
  // has not received, which the next collecting sink would otherwise look complete without.
  const bool truncationDelivered = reported != nullptr && reported->truncationReported;
  if ((warnings.resourceLimitExceeded() && !truncationDelivered) || dropsNewWarning) {
    EnsureHeldWarnings(registry, pending).warnings.markResourceLimitExceeded();
  }
  registry.ctx().insert_or_assign(HeldSubDocumentWarningBatch{
      .warnings = warnings.warnings(), .truncated = warnings.resourceLimitExceeded()});
}

void DeliverHeldSubDocumentWarnings(Registry& registry, ParseWarningSink& hostSink) {
  if (registry.ctx().erase<RediscoverSubDocumentWarnings>()) {
    components::ExternalUseStyleRetention::ScheduleFullRestyle(registry);
  }
  if (auto* pending = registry.ctx().find<PendingSubDocumentWarnings>()) {
    const PendingSubDocumentWarnings held = std::move(*pending);
    registry.ctx().erase<PendingSubDocumentWarnings>();
    ReportSubDocumentWarnings(registry, held.warnings, hostSink);
  }
}

}  // namespace donner::svg::details
