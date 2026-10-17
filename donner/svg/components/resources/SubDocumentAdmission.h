#pragma once
/// @file

#include <memory>
#include <utility>
#include <vector>

#include "donner/svg/components/DocumentResourceFamilyBudget.h"
#include "donner/svg/components/resources/SubDocumentCache.h"

namespace donner::svg::components {

struct SubDocumentAdmissionLease;

/// Internal count admission for retained SVG resource graphs.
/// Resource graphs must be idle/serialized during traversal; the mutex protects metadata only.
class SubDocumentAdmission {
public:
  /// Family-wide admission observations; entity/payload charges are high-water snapshots.
  struct Stats {
    size_t documents = 0;      ///< Retained documents, excluding the original root.
    size_t pending = 0;        ///< Document slots reserved before parser callbacks.
    size_t entities = 0;       ///< Admitted root and descendant entity snapshots.
    size_t payloadBytes = 0;   ///< Admitted parsed-payload snapshots.
    size_t parseAttempts = 0;  ///< Parser calls, including failed calls; never refunded.
    size_t rejections = 0;     ///< Refused admission operations.
  };

  /// Provisional document slots. Destruction refunds slots but not parse attempts.
  class Reservation {
  public:
    ~Reservation();
    Reservation(const Reservation&) = delete;
    Reservation& operator=(const Reservation&) = delete;

  private:
    friend class SubDocumentAdmission;
    explicit Reservation(std::vector<std::shared_ptr<SubDocumentAdmissionState>> states)
        : states_(std::move(states)) {}
    std::vector<std::shared_ptr<SubDocumentAdmissionState>> states_;
    bool active_ = true;
  };

  /// Bind a cache to its owning registry and all requesting admission families.
  /// @param registry Cache owner. @param limits Local ceilings that may tighten the family.
  static std::shared_ptr<SubDocumentAdmission> Bind(Registry& registry,
                                                    const SubDocumentCache::Limits& limits);

  /// Atomically spend one attempt and reserve one pending document slot per requesting family.
  std::unique_ptr<Reservation> beginParse() const;

  /// Admit a returned or cached document's bounded, acyclic retained graph.
  /// @param document Candidate. @param reservation Optional slots reserved before parsing.
  bool admit(const SVGDocumentHandle& document, Reservation* reservation = nullptr) const;

  /// Inspect the ledger attached to an existing family without creating one.
  /// @param family Family whose admission counters are requested.
  static Stats GetStats(const DocumentResourceFamilyBudget& family);

private:
  struct Snapshot;
  struct Transaction;
  static bool CollectChildren(Snapshot& snapshot, Registry& registry,
                              std::vector<SVGDocumentHandle>& children);
  static bool CollectFilterChildren(Snapshot& snapshot, Registry& registry,
                                    std::vector<SVGDocumentHandle>& children);
  static bool AppendChild(Snapshot& snapshot, const SVGDocumentHandle& child,
                          std::vector<SVGDocumentHandle>& children);
  static bool Visit(Snapshot& snapshot, Registry& registry, std::shared_ptr<Registry> retained);
  static bool Apply(Snapshot& snapshot,
                    const std::vector<std::shared_ptr<SubDocumentAdmissionState>>& states,
                    Reservation* reservation, SubDocumentAdmissionState* initialRoot = nullptr);
  std::vector<std::shared_ptr<SubDocumentAdmissionState>> liveStates() const;

  std::shared_ptr<SubDocumentAdmissionState> primary_;
  std::vector<std::weak_ptr<SubDocumentAdmissionState>> states_;
  std::weak_ptr<SubDocumentAdmissionLease> owner_;
};

}  // namespace donner::svg::components
