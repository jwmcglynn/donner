#include "donner/svg/components/resources/SubDocumentAdmission.h"

#include <algorithm>
#include <cassert>
#include <mutex>
#include <unordered_map>
#include <utility>

#include "donner/base/EcsRegistry.h"
#include "donner/svg/components/ParsedPayloadResourceBudget.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/resources/ImageComponent.h"

namespace donner::svg::components {
namespace {

constexpr size_t kMaximumFamilies = 64;
constexpr size_t kMaximumGraphWork = 128 * 1024;

// Only admission metadata is protected. Registry traversal and parser callbacks stay outside.
std::mutex& AdmissionMutex() {
  static std::mutex mutex;
  return mutex;
}

bool Fits(size_t retained, size_t additional, size_t maximum) {
  return retained <= maximum && additional <= maximum - retained;
}

}  // namespace

struct SubDocumentAdmissionState {
  SubDocumentCache::Limits limits;
  SubDocumentAdmission::Stats stats;
  bool originalRootAssigned = false;
  std::unordered_map<const Registry*, std::weak_ptr<SubDocumentAdmissionLease>> members;
};

struct SubDocumentAdmissionLease {
  std::weak_ptr<SubDocumentAdmissionState> state;
  const Registry* registry = nullptr;
  size_t entities = 0;
  size_t payloadBytes = 0;
  bool document = true;
  bool charged = false;

  ~SubDocumentAdmissionLease() {
    auto retainedState = state.lock();
    if (!retainedState || !charged) {
      return;
    }
    std::lock_guard lock(AdmissionMutex());
    auto& stats = retainedState->stats;
    assert(stats.entities >= entities && stats.payloadBytes >= payloadBytes);
    stats.entities -= entities;
    stats.payloadBytes -= payloadBytes;
    if (document) {
      assert(stats.documents != 0);
      --stats.documents;
    }
    const auto found = retainedState->members.find(registry);
    if (found != retainedState->members.end() && found->second.expired()) {
      retainedState->members.erase(found);
    }
  }
};

namespace {

struct SubDocumentAdmissionMemberships {
  std::vector<std::shared_ptr<SubDocumentAdmissionLease>> leases;
};

void Tighten(SubDocumentCache::Limits& limits, const SubDocumentCache::Limits& requested) {
  limits.maximumDocuments = std::min(limits.maximumDocuments, requested.maximumDocuments);
  limits.maximumParseAttempts =
      std::min(limits.maximumParseAttempts, requested.maximumParseAttempts);
  limits.maximumAggregateEntities =
      std::min(limits.maximumAggregateEntities, requested.maximumAggregateEntities);
  limits.maximumAggregatePayloadBytes =
      std::min(limits.maximumAggregatePayloadBytes, requested.maximumAggregatePayloadBytes);
}

void RejectLocked(const std::vector<std::shared_ptr<SubDocumentAdmissionState>>& states) {
  for (const auto& state : states) {
    ++state->stats.rejections;
  }
}

void Reject(const std::vector<std::shared_ptr<SubDocumentAdmissionState>>& states) {
  std::lock_guard lock(AdmissionMutex());
  RejectLocked(states);
}

bool AddState(std::vector<std::shared_ptr<SubDocumentAdmissionState>>& states,
              std::shared_ptr<SubDocumentAdmissionState> state) {
  if (!state || std::find(states.begin(), states.end(), state) != states.end()) {
    return true;
  }
  if (states.size() >= kMaximumFamilies) {
    return false;
  }
  states.push_back(std::move(state));
  return true;
}

}  // namespace

struct SubDocumentAdmission::Snapshot {
  struct Node {
    Registry* registry;
    std::shared_ptr<Registry> retained;
    std::vector<size_t> children;
    size_t entities;
    size_t payloadBytes;
    bool complete = false;
  };
  std::vector<Node> nodes;
  std::unordered_map<Registry*, size_t> indices;
  std::vector<size_t> order;
  size_t maximumNodes = 65;
  size_t maximumWork = kMaximumGraphWork;
  size_t work = 0;
  const Registry* forbidden = nullptr;

  explicit Snapshot(const std::vector<std::shared_ptr<SubDocumentAdmissionState>>& states) {
    std::lock_guard lock(AdmissionMutex());
    for (const auto& state : states) {
      maximumNodes = std::min(maximumNodes, state->limits.maximumDocuments + 1);
      maximumWork = std::min(maximumWork, state->limits.maximumAggregateEntities +
                                              4 * (state->limits.maximumDocuments + 1));
    }
  }

  bool step() {
    if (work >= maximumWork) {
      return false;
    }
    ++work;
    return true;
  }
};

bool SubDocumentAdmission::AppendChild(Snapshot& snapshot, const SVGDocumentHandle& child,
                                       std::vector<SVGDocumentHandle>& children) {
  if (!snapshot.step()) {
    return false;
  }
  if (child) {
    children.push_back(child);
  }
  return true;
}

bool SubDocumentAdmission::CollectChildren(Snapshot& snapshot, Registry& registry,
                                           std::vector<SVGDocumentHandle>& children) {
  if (const auto* cache = registry.ctx().find<SubDocumentCache>()) {
    for (const auto& [url, child] : cache->cache_) {
      if (!AppendChild(snapshot, child, children)) {
        return false;
      }
    }
  }
  for (const auto& [entity, image] : registry.view<LoadedSVGImageComponent>().each()) {
    if (!AppendChild(snapshot, image.subDocument, children)) {
      return false;
    }
  }
  for (const auto& [entity, use] : registry.view<ExternalUseComponent>().each()) {
    if (!AppendChild(snapshot, use.subDocument, children)) {
      return false;
    }
  }
  return CollectFilterChildren(snapshot, registry, children);
}

bool SubDocumentAdmission::CollectFilterChildren(Snapshot& snapshot, Registry& registry,
                                                 std::vector<SVGDocumentHandle>& children) {
  for (const auto& [entity, filter] : registry.view<ComputedFilterComponent>().each()) {
    if (!snapshot.step()) {
      return false;
    }
    for (const auto& node : filter.filterGraph.nodes) {
      if (!snapshot.step()) {
        return false;
      }
      if (const auto* image = std::get_if<filter_primitive::Image>(&node.primitive)) {
        if (!AppendChild(snapshot, image->svgSubDocument, children)) {
          return false;
        }
      }
    }
  }
  return true;
}

bool SubDocumentAdmission::Visit(Snapshot& snapshot, Registry& registry,
                                 std::shared_ptr<Registry> retained) {
  if (!snapshot.step() || &registry == snapshot.forbidden) {
    return false;
  }
  if (const auto found = snapshot.indices.find(&registry); found != snapshot.indices.end()) {
    return snapshot.nodes[found->second].complete;
  }
  if (snapshot.nodes.size() >= snapshot.maximumNodes) {
    return false;
  }
  const size_t index = snapshot.nodes.size();
  const auto* payload = registry.ctx().find<ParsedPayloadResourceBudget>();
  snapshot.nodes.push_back({&registry,
                            std::move(retained),
                            {},
                            registry.storage<Entity>().size(),
                            payload ? payload->securityStats().retainedBytes : 0});
  snapshot.indices.emplace(&registry, index);
  std::vector<SVGDocumentHandle> children;
  if (!CollectChildren(snapshot, registry, children)) {
    return false;
  }
  for (const auto& child : children) {
    auto childRegistry = child->sharedRegistry();
    if (!Visit(snapshot, *childRegistry, childRegistry)) {
      return false;
    }
    snapshot.nodes[index].children.push_back(snapshot.indices.at(childRegistry.get()));
  }
  snapshot.nodes[index].complete = true;
  snapshot.order.push_back(index);
  return true;
}

struct SubDocumentAdmission::Transaction {
  struct Update {
    Snapshot::Node* node;
    std::shared_ptr<SubDocumentAdmissionLease> lease;
    bool fresh;
    bool root;
    size_t entities;
    size_t payloadBytes;
  };
  struct Ledger {
    std::shared_ptr<SubDocumentAdmissionState> state;
    size_t documents = 0;
    size_t entities = 0;
    size_t payloadBytes = 0;
    size_t pendingCredit = 0;
    std::vector<Update> updates;
  };
  std::vector<Ledger> ledgers;
  std::vector<std::vector<std::shared_ptr<SubDocumentAdmissionState>>> flowing;
  std::vector<std::shared_ptr<SubDocumentAdmissionLease>> retired;

  Ledger& ledger(const std::shared_ptr<SubDocumentAdmissionState>& state) {
    const auto found = std::find_if(ledgers.begin(), ledgers.end(),
                                    [&](const Ledger& item) { return item.state == state; });
    if (found != ledgers.end()) {
      return *found;
    }
    ledgers.push_back({state});
    return ledgers.back();
  }

  bool add(Snapshot::Node& node, const std::shared_ptr<SubDocumentAdmissionState>& state,
           bool root) {
    auto& target = ledger(state);
    const auto found = state->members.find(node.registry);
    auto lease = found == state->members.end() ? nullptr : found->second.lock();
    const bool fresh = !lease;
    if (fresh) {
      lease = std::make_shared<SubDocumentAdmissionLease>();
      lease->state = state;
      lease->registry = node.registry;
      lease->document = !root;
    }
    const size_t entities = std::max(lease->entities, node.entities);
    const size_t payloadBytes = std::max(lease->payloadBytes, node.payloadBytes);
    const size_t addedEntities = entities - lease->entities;
    const size_t addedPayload = payloadBytes - lease->payloadBytes;
    if (!Fits(target.entities, addedEntities, state->limits.maximumAggregateEntities) ||
        !Fits(target.payloadBytes, addedPayload, state->limits.maximumAggregatePayloadBytes)) {
      return false;
    }
    target.documents += fresh && lease->document ? 1 : 0;
    target.entities += addedEntities;
    target.payloadBytes += addedPayload;
    target.updates.push_back({&node, std::move(lease), fresh, root, entities, payloadBytes});
    return true;
  }

  bool addExistingStates(Snapshot::Node& node,
                         std::vector<std::shared_ptr<SubDocumentAdmissionState>>& states) {
    if (const auto* memberships = node.registry->ctx().find<SubDocumentAdmissionMemberships>()) {
      for (const auto& lease : memberships->leases) {
        if (!AddState(states, lease->state.lock())) {
          return false;
        }
      }
    }
    return true;
  }

  bool planNode(Snapshot& snapshot, size_t index, SubDocumentAdmissionState* initialRoot) {
    auto& node = snapshot.nodes[index];
    auto& states = flowing[index];
    if (!addExistingStates(node, states)) {
      return false;
    }
    for (const auto& state : states) {
      const bool root = index == 0 && state.get() == initialRoot && !state->originalRootAssigned;
      if (!snapshot.step() || !add(node, state, root)) {
        return false;
      }
      for (const size_t child : node.children) {
        if (!snapshot.step() || !AddState(flowing[child], state)) {
          return false;
        }
      }
    }
    return true;
  }

  bool fits() const {
    for (const auto& plan : ledgers) {
      const auto& stats = plan.state->stats;
      const auto& limits = plan.state->limits;
      if (stats.pending < plan.pendingCredit ||
          !Fits(stats.documents, stats.pending - plan.pendingCredit, limits.maximumDocuments) ||
          !Fits(stats.documents + stats.pending - plan.pendingCredit, plan.documents,
                limits.maximumDocuments) ||
          !Fits(stats.entities, plan.entities, limits.maximumAggregateEntities) ||
          !Fits(stats.payloadBytes, plan.payloadBytes, limits.maximumAggregatePayloadBytes)) {
        return false;
      }
    }
    return true;
  }

  void commit() {
    for (auto& plan : ledgers) {
      auto& state = *plan.state;
      for (auto& update : plan.updates) {
        auto& context = update.node->registry->ctx();
        if (!context.contains<SubDocumentAdmissionMemberships>()) {
          context.emplace<SubDocumentAdmissionMemberships>();
        }
        auto& memberships = context.get<SubDocumentAdmissionMemberships>().leases;
        for (auto it = memberships.begin(); it != memberships.end();) {
          if ((*it)->state.expired()) {
            retired.push_back(std::move(*it));
            it = memberships.erase(it);
          } else {
            ++it;
          }
        }
        if (update.fresh) {
          state.members.insert_or_assign(update.node->registry, update.lease);
          memberships.push_back(update.lease);
        }
        update.lease->entities = update.entities;
        update.lease->payloadBytes = update.payloadBytes;
        update.lease->charged = true;
        state.originalRootAssigned |= update.root;
      }
      state.stats.pending -= plan.pendingCredit;
      state.stats.documents += plan.documents;
      state.stats.entities += plan.entities;
      state.stats.payloadBytes += plan.payloadBytes;
    }
  }
};

bool SubDocumentAdmission::Apply(
    Snapshot& snapshot, const std::vector<std::shared_ptr<SubDocumentAdmissionState>>& states,
    Reservation* reservation, SubDocumentAdmissionState* initialRoot) {
  // These containers release their strong references only after the lock leaves scope.
  Transaction transaction;
  transaction.flowing.resize(snapshot.nodes.size());
  transaction.flowing.front() = states;
  std::lock_guard lock(AdmissionMutex());
  for (auto it = snapshot.order.rbegin(); it != snapshot.order.rend(); ++it) {
    if (!transaction.planNode(snapshot, *it, initialRoot)) {
      RejectLocked(states);
      return false;
    }
  }
  if (reservation && reservation->active_) {
    for (const auto& state : reservation->states_) {
      transaction.ledger(state).pendingCredit = 1;
    }
  }
  if (!transaction.fits()) {
    RejectLocked(states);
    return false;
  }
  transaction.commit();
  if (reservation) {
    reservation->active_ = false;
  }
  return true;
}

std::shared_ptr<SubDocumentAdmission> SubDocumentAdmission::Bind(
    Registry& registry, const SubDocumentCache::Limits& limits) {
  auto result = std::shared_ptr<SubDocumentAdmission>(new SubDocumentAdmission);
  std::shared_ptr<DocumentResourceFamilyBudget> family;
  std::vector<std::shared_ptr<SubDocumentAdmissionState>> states;
  {
    std::lock_guard lock(AdmissionMutex());
    if (!registry.ctx().contains<DocumentResourceFamilyContext>()) {
      registry.ctx().emplace<DocumentResourceFamilyContext>();
    }
    auto& context = registry.ctx().get<DocumentResourceFamilyContext>();
    if (!context.budget) {
      context.budget = std::make_shared<DocumentResourceFamilyBudget>();
    }
    family = context.budget;
    if (!family->subDocumentAdmission_) {
      family->subDocumentAdmission_ = std::make_shared<SubDocumentAdmissionState>();
    }
    result->primary_ = family->subDocumentAdmission_;
    states.push_back(result->primary_);
    if (const auto* memberships = registry.ctx().find<SubDocumentAdmissionMemberships>()) {
      for (const auto& lease : memberships->leases) {
        if (!AddState(states, lease->state.lock())) {
          RejectLocked(states);
          return {};
        }
      }
    }
    for (const auto& state : states) {
      Tighten(state->limits, limits);
    }
  }
  Snapshot snapshot(states);
  if (!Visit(snapshot, registry, {}) || !Apply(snapshot, states, nullptr, result->primary_.get())) {
    Reject(states);
    return {};
  }
  {
    std::lock_guard lock(AdmissionMutex());
    result->owner_ = result->primary_->members.at(&registry);
    for (const auto& state : states) {
      result->states_.push_back(state);
    }
  }
  return result;
}

std::vector<std::shared_ptr<SubDocumentAdmissionState>> SubDocumentAdmission::liveStates() const {
  std::vector<std::shared_ptr<SubDocumentAdmissionState>> result;
  auto owner = owner_.lock();
  std::lock_guard lock(AdmissionMutex());
  for (const auto& state : states_) {
    if (!AddState(result, state.lock())) {
      RejectLocked(result);
      return {};
    }
  }
  if (owner) {
    if (const auto* memberships = owner->registry->ctx().find<SubDocumentAdmissionMemberships>()) {
      for (const auto& lease : memberships->leases) {
        if (!AddState(result, lease->state.lock())) {
          RejectLocked(result);
          return {};
        }
      }
    }
  }
  return result;
}

std::unique_ptr<SubDocumentAdmission::Reservation> SubDocumentAdmission::beginParse() const {
  auto states = liveStates();
  if (states.empty()) {
    return {};
  }
  auto result = std::unique_ptr<Reservation>(new Reservation(std::move(states)));
  result->active_ = false;
  std::lock_guard lock(AdmissionMutex());
  for (const auto& state : result->states_) {
    const auto& stats = state->stats;
    if (stats.parseAttempts >= state->limits.maximumParseAttempts ||
        !Fits(stats.documents, stats.pending, state->limits.maximumDocuments) ||
        !Fits(stats.documents + stats.pending, 1, state->limits.maximumDocuments)) {
      RejectLocked(result->states_);
      return {};
    }
  }
  for (const auto& state : result->states_) {
    ++state->stats.parseAttempts;
    ++state->stats.pending;
  }
  result->active_ = true;
  return result;
}

SubDocumentAdmission::Reservation::~Reservation() {
  std::lock_guard lock(AdmissionMutex());
  if (active_) {
    for (const auto& state : states_) {
      assert(state->stats.pending != 0);
      --state->stats.pending;
    }
  }
}

bool SubDocumentAdmission::admit(const SVGDocumentHandle& document,
                                 Reservation* reservation) const {
  auto states = liveStates();
  if (!document || states.empty()) {
    return false;
  }
  if (reservation) {
    for (const auto& state : states) {
      if (std::find(reservation->states_.begin(), reservation->states_.end(), state) ==
          reservation->states_.end()) {
        Reject(states);
        return false;
      }
    }
  }
  auto owner = owner_.lock();
  Snapshot snapshot(states);
  snapshot.forbidden = owner ? owner->registry : nullptr;
  auto registry = document->sharedRegistry();
  if (!Visit(snapshot, *registry, registry)) {
    Reject(states);
    return false;
  }
  return Apply(snapshot, states, reservation);
}

SubDocumentAdmission::Stats SubDocumentAdmission::GetStats(
    const DocumentResourceFamilyBudget& family) {
  std::lock_guard lock(AdmissionMutex());
  return family.subDocumentAdmission_ ? family.subDocumentAdmission_->stats : Stats{};
}

}  // namespace donner::svg::components
