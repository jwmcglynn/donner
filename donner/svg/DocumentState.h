#pragma once
/// @file

#include <algorithm>
#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "donner/base/EcsRegistry.h"

#if defined(__clang__)
#define DONNER_NO_THREAD_SAFETY_ANALYSIS __attribute__((no_thread_safety_analysis))
#else
#define DONNER_NO_THREAD_SAFETY_ANALYSIS
#endif

namespace donner::svg {

/// DOM threading policy for a document.
enum class ThreadingMode {
  /// Keep current zero-lock behavior for single-threaded users.
  SingleThreaded,
  /// Reserve the document for future guarded multi-threaded DOM access.
  ConcurrentDom,
};

class DocumentReadAccess;
class DetachedNodeCollectionDeferral;
class DocumentMutationBatch;
class DocumentWriteAccess;

/// Lightweight reader/writer gate for document access.
class DocumentAccessLock {
public:
  /// Acquire shared read access.
  void lockRead() {
    while (true) {
      waitForWriterToLeave();
      activeReaders_.fetch_add(1, std::memory_order_acquire);
      if (!writerPendingOrActive_.load(std::memory_order_acquire)) {
        return;
      }

      unlockRead();
    }
  }

  /// Release shared read access.
  void unlockRead() {
    const std::uint32_t previous = activeReaders_.fetch_sub(1, std::memory_order_release);
    assert(previous > 0 && "DocumentAccessLock read underflow");
    if (previous == 1 && writerPendingOrActive_.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> lock(waitMutex_);
      waitCondition_.notify_all();
    }
  }

  /// Acquire exclusive write access.
  void lockWrite() DONNER_NO_THREAD_SAFETY_ANALYSIS {
    writerMutex_.lock();
    writerPendingOrActive_.store(true, std::memory_order_release);
    if (activeReaders_.load(std::memory_order_acquire) != 0) {
      std::unique_lock<std::mutex> lock(waitMutex_);
      waitCondition_.wait(lock,
                          [this]() { return activeReaders_.load(std::memory_order_acquire) == 0; });
    }
  }

  /// Release exclusive write access.
  void unlockWrite() DONNER_NO_THREAD_SAFETY_ANALYSIS {
    writerPendingOrActive_.store(false, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(waitMutex_);
      waitCondition_.notify_all();
    }
    writerMutex_.unlock();
  }

private:
  void waitForWriterToLeave() {
    if (!writerPendingOrActive_.load(std::memory_order_acquire)) {
      return;
    }

    std::unique_lock<std::mutex> lock(waitMutex_);
    waitCondition_.wait(
        lock, [this]() { return !writerPendingOrActive_.load(std::memory_order_acquire); });
  }

  std::atomic<std::uint32_t> activeReaders_ = 0;
  std::atomic<bool> writerPendingOrActive_ = false;
  std::mutex writerMutex_;
  std::mutex waitMutex_;
  std::condition_variable waitCondition_;
};

/// Summary of detached-node collection state.
struct DetachedNodeDiagnostics {
  std::size_t queuedDetachedRoots = 0;      ///< Detached roots pending collection.
  std::size_t retainedByPublicHandles = 0;  ///< Queued roots currently retained by public handles.
  std::size_t retainedByPublicHandlesInLastPass = 0;     ///< Roots skipped in the last pass.
  std::size_t retainedBySnapshotOrObserverEpochs = 0;    ///< Roots skipped due to future epochs.
  std::uint32_t maxPublicHandlesOnRetainedRoot = 0;      ///< Max handles on a retained root.
  std::uint64_t maxRetainedSnapshotOrObserverEpoch = 0;  ///< Future epoch retention high-water.
  std::size_t collectedInLastPass = 0;                   ///< Roots destroyed in the last pass.
  bool isCollecting = false;                             ///< True while collection is active.
};

/// Summary of document access and lock state.
struct DocumentAccessDiagnostics {
  std::uint64_t readAccesses = 0;            ///< Read access guards created in ConcurrentDom.
  std::uint64_t writeAccesses = 0;           ///< Write access guards created in ConcurrentDom.
  std::uint64_t readLocksAcquired = 0;       ///< Shared locks acquired by read guards.
  std::uint64_t writeLocksAcquired = 0;      ///< Unique locks acquired by write guards.
  std::uint64_t reentrantReadAccesses = 0;   ///< Read guards nested inside a write guard.
  std::uint64_t reentrantWriteAccesses = 0;  ///< Write guards nested inside a write guard.
  std::uint64_t totalReadLockHeldNs = 0;     ///< Total read-lock hold time in nanoseconds.
  std::uint64_t totalWriteLockHeldNs = 0;    ///< Total write-lock hold time in nanoseconds.
  std::uint64_t maxReadLockHeldNs = 0;       ///< Longest read-lock hold time in nanoseconds.
  std::uint64_t maxWriteLockHeldNs = 0;      ///< Longest write-lock hold time in nanoseconds.
  std::uint32_t activeReadLocks = 0;         ///< Shared read locks currently held.
  bool writeLockHeld = false;                ///< True while a unique write lock is held.
};

/// One committed document mutation revision.
struct DocumentMutationRecord {
  std::uint64_t sequence = 0;  ///< Monotonic mutation-log sequence number.
  std::uint64_t revision = 0;  ///< Document revision after the mutation committed.
};

/// Snapshot of mutation-log records after a caller-owned sequence number.
struct DocumentMutationLogSnapshot {
  std::vector<DocumentMutationRecord> records;  ///< Mutation records newer than the caller cursor.
  std::uint64_t latestSequence = 0;             ///< Latest sequence in the document.
  bool missedRecords = false;                   ///< True if older records were truncated.
};

/// Document-local detached-node collection state.
struct DetachedNodeState {
  std::vector<Entity> detachedRoots;                ///< Detached roots pending collection.
  std::vector<Entity> lastCollectedRoots;           ///< Detached roots destroyed in the last pass.
  std::vector<Entity> lastRetainedByPublicHandles;  ///< Roots skipped due to public handles.
  std::vector<Entity> lastRetainedBySnapshotOrObserverEpochs;  ///< Roots skipped by epochs.
  std::uint64_t maxRetainedSnapshotOrObserverEpoch = 0;        ///< Max retaining epoch.
  bool isCollecting = false;                                   ///< Reentrancy guard for collection.

  /**
   * Queue a detached root for collection.
   *
   * @param detachedRoot Root entity to queue.
   */
  void queueDetachedRoot(Entity detachedRoot) {
    if (detachedRoot == entt::null) {
      return;
    }

    for (Entity root : detachedRoots) {
      if (root == detachedRoot) {
        return;
      }
    }

    detachedRoots.push_back(detachedRoot);
  }

  /**
   * Stop tracking a detached root.
   *
   * @param detachedRoot Root entity to remove.
   */
  void removeDetachedRoot(Entity detachedRoot) {
    if (detachedRoot == entt::null) {
      return;
    }

    for (auto it = detachedRoots.begin(); it != detachedRoots.end();) {
      if (*it == detachedRoot) {
        it = detachedRoots.erase(it);
      } else {
        ++it;
      }
    }
  }

  /// Current detached-node collection diagnostics that do not require registry traversal.
  DetachedNodeDiagnostics diagnostics() const {
    DetachedNodeDiagnostics result;
    result.queuedDetachedRoots = detachedRoots.size();
    result.retainedByPublicHandlesInLastPass = lastRetainedByPublicHandles.size();
    result.retainedBySnapshotOrObserverEpochs = lastRetainedBySnapshotOrObserverEpochs.size();
    result.maxRetainedSnapshotOrObserverEpoch = maxRetainedSnapshotOrObserverEpoch;
    result.collectedInLastPass = lastCollectedRoots.size();
    result.isCollecting = isCollecting;
    return result;
  }
};

/**
 * Shared mutable state behind \ref SVGDocument and \ref SVGElement facades.
 *
 * This currently owns the ECS registry and a mutation revision. It is intentionally a thin
 * indirection layer so later phases can add reader/writer guards and render snapshots without
 * changing the public DOM handles again.
 */
class DocumentState {
  friend class DetachedNodeCollectionDeferral;
  friend class DocumentReadAccess;
  friend class DocumentMutationBatch;
  friend class DocumentWriteAccess;

public:
  /// Create a new empty document state.
  DocumentState() : registry_(std::make_shared<Registry>()) {}

  /**
   * Wrap an existing shared registry.
   *
   * @param registry Registry to retain.
   */
  explicit DocumentState(std::shared_ptr<Registry> registry) : registry_(std::move(registry)) {}

  /// Get the underlying ECS registry.
  Registry& registry() { return *registry_; }

  /// Get the underlying ECS registry.
  const Registry& registry() const { return *registry_; }

  /// Get the shared registry retained by this state.
  std::shared_ptr<Registry> sharedRegistry() const { return registry_; }

  /// Acquire read access to the document.
  DocumentReadAccess read();

  /// Acquire write access to the document.
  DocumentWriteAccess write();

  /// Current document mutation revision.
  std::uint64_t revision() const { return revision_.load(std::memory_order_relaxed); }

  /// Latest mutation-log sequence number.
  std::uint64_t mutationSequence() const {
    std::lock_guard<std::mutex> lock(mutationLogMutex_);
    return latestMutationSequence_;
  }

  /**
   * Return mutation records after a previously observed sequence.
   *
   * This is a polling hook for renderers, editor integrations, scripting, and future mutation
   * observers. It intentionally does not invoke callbacks while document write access is held.
   *
   * @param afterSequence Last sequence number already observed by the caller.
   */
  DocumentMutationLogSnapshot mutationRecordsSince(std::uint64_t afterSequence) const {
    DocumentMutationLogSnapshot result;
    std::lock_guard<std::mutex> lock(mutationLogMutex_);
    result.latestSequence = latestMutationSequence_;
    if (!mutationLog_.empty()) {
      const std::uint64_t oldestSequence = mutationLog_.front().sequence;
      result.missedRecords = afterSequence < oldestSequence - 1;
    }

    result.records.reserve(mutationLog_.size());
    for (const DocumentMutationRecord& record : mutationLog_) {
      if (record.sequence > afterSequence) {
        result.records.push_back(record);
      }
    }

    return result;
  }

  /// Current document access diagnostics.
  DocumentAccessDiagnostics accessDiagnostics() const {
    DocumentAccessDiagnostics result;
    result.readAccesses = readAccesses_.load(std::memory_order_relaxed);
    result.writeAccesses = writeAccesses_.load(std::memory_order_relaxed);
    result.readLocksAcquired = readLocksAcquired_.load(std::memory_order_relaxed);
    result.writeLocksAcquired = writeLocksAcquired_.load(std::memory_order_relaxed);
    result.reentrantReadAccesses = reentrantReadAccesses_.load(std::memory_order_relaxed);
    result.reentrantWriteAccesses = reentrantWriteAccesses_.load(std::memory_order_relaxed);
    result.totalReadLockHeldNs = totalReadLockHeldNs_.load(std::memory_order_relaxed);
    result.totalWriteLockHeldNs = totalWriteLockHeldNs_.load(std::memory_order_relaxed);
    result.maxReadLockHeldNs = maxReadLockHeldNs_.load(std::memory_order_relaxed);
    result.maxWriteLockHeldNs = maxWriteLockHeldNs_.load(std::memory_order_relaxed);
    result.activeReadLocks = activeReadLocks_.load(std::memory_order_relaxed);
    result.writeLockHeld = activeWriteLocks_.load(std::memory_order_relaxed) > 0;
    return result;
  }

  /**
   * Reset cumulative access diagnostics.
   *
   * Active lock state is not modified. Callers should only reset diagnostics between measured
   * intervals when no access guard is held if they need a clean lock-hold sample.
   */
  void resetAccessDiagnostics() {
    readAccesses_.store(0, std::memory_order_relaxed);
    writeAccesses_.store(0, std::memory_order_relaxed);
    readLocksAcquired_.store(0, std::memory_order_relaxed);
    writeLocksAcquired_.store(0, std::memory_order_relaxed);
    reentrantReadAccesses_.store(0, std::memory_order_relaxed);
    reentrantWriteAccesses_.store(0, std::memory_order_relaxed);
    totalReadLockHeldNs_.store(0, std::memory_order_relaxed);
    totalWriteLockHeldNs_.store(0, std::memory_order_relaxed);
    maxReadLockHeldNs_.store(0, std::memory_order_relaxed);
    maxWriteLockHeldNs_.store(0, std::memory_order_relaxed);
  }

  /// Returns true if this thread currently holds reentrant write access to this document.
  bool currentThreadHasWriteAccess() const { return activeWriteDocument_ == this; }

  /// Returns true if this thread currently holds read or write access to this document.
  bool currentThreadHasAccess() const {
    return currentThreadHasWriteAccess() || currentThreadHasReadAccess();
  }

  /// Mark the document as mutated.
  void bumpMutationRevision() {
    if (activeMutationBatchDocument_ == this && activeMutationBatchDepth_ > 0) {
      activeMutationBatchMutated_ = true;
      return;
    }

    bumpMutationRevisionNow();
  }

  /// Detached-node collection state for this document.
  DetachedNodeState& detachedNodeState() { return detachedNodeState_; }

  /// Detached-node collection state for this document.
  const DetachedNodeState& detachedNodeState() const { return detachedNodeState_; }

  /// Defer detached-node collection while a render snapshot or observer epoch may use it.
  DetachedNodeCollectionDeferral deferDetachedNodeCollection();

  /// Returns true when a render snapshot or observer epoch is deferring collection.
  bool hasActiveDetachedNodeCollectionDeferral() const {
    return activeDetachedCollectionDeferrals_.load(std::memory_order_acquire) > 0;
  }

  /// Current detached-node collection epoch high-water.
  std::uint64_t activeDetachedNodeCollectionEpoch() const {
    return detachedCollectionEpoch_.load(std::memory_order_acquire);
  }

  /// Current DOM threading policy.
  ThreadingMode threadingMode() const { return threadingMode_; }

  /**
   * Set the DOM threading policy.
   *
   * @param mode New threading mode.
   */
  void setThreadingMode(ThreadingMode mode) { threadingMode_ = mode; }

private:
  void assertSingleThreadedAccess() const {
    assert(threadingMode_ != ThreadingMode::SingleThreaded ||
           ownerThread_ == std::this_thread::get_id());
  }

  bool beginMutationBatch() {
    if (activeMutationBatchDocument_ != nullptr && activeMutationBatchDocument_ != this) {
      return false;
    }

    if (activeMutationBatchDocument_ == nullptr) {
      activeMutationBatchDocument_ = this;
      activeMutationBatchDepth_ = 0;
      activeMutationBatchMutated_ = false;
    }

    ++activeMutationBatchDepth_;
    return true;
  }

  void endMutationBatch() {
    assert(activeMutationBatchDocument_ == this && activeMutationBatchDepth_ > 0);
    --activeMutationBatchDepth_;
    if (activeMutationBatchDepth_ > 0) {
      return;
    }

    const bool mutated = activeMutationBatchMutated_;
    activeMutationBatchDocument_ = nullptr;
    activeMutationBatchMutated_ = false;
    if (mutated) {
      bumpMutationRevisionNow();
    }
  }

  void bumpMutationRevisionNow() {
    const std::uint64_t revision = revision_.fetch_add(1, std::memory_order_relaxed) + 1;
    recordMutation(revision);
  }

  void recordMutation(std::uint64_t revision) {
    std::lock_guard<std::mutex> lock(mutationLogMutex_);
    const std::uint64_t sequence = ++latestMutationSequence_;
    mutationLog_.push_back(DocumentMutationRecord{sequence, revision});
    if (mutationLog_.size() > kMaxMutationLogRecords) {
      mutationLog_.pop_front();
    }
  }

  void recordReadAccess(bool acquiredLock) {
    readAccesses_.fetch_add(1, std::memory_order_relaxed);
    if (acquiredLock) {
      readLocksAcquired_.fetch_add(1, std::memory_order_relaxed);
      activeReadLocks_.fetch_add(1, std::memory_order_relaxed);
    } else {
      reentrantReadAccesses_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void releaseReadLock() { activeReadLocks_.fetch_sub(1, std::memory_order_relaxed); }

  void recordReadLockHeld(std::uint64_t heldNs) {
    totalReadLockHeldNs_.fetch_add(heldNs, std::memory_order_relaxed);
    updateMax(maxReadLockHeldNs_, heldNs);
  }

  void recordWriteAccess(bool acquiredLock) {
    writeAccesses_.fetch_add(1, std::memory_order_relaxed);
    if (acquiredLock) {
      writeLocksAcquired_.fetch_add(1, std::memory_order_relaxed);
      activeWriteLocks_.fetch_add(1, std::memory_order_relaxed);
    } else {
      reentrantWriteAccesses_.fetch_add(1, std::memory_order_relaxed);
    }
  }

  void releaseWriteLock() { activeWriteLocks_.fetch_sub(1, std::memory_order_relaxed); }

  void recordWriteLockHeld(std::uint64_t heldNs) {
    totalWriteLockHeldNs_.fetch_add(heldNs, std::memory_order_relaxed);
    updateMax(maxWriteLockHeldNs_, heldNs);
  }

  std::uint64_t beginDetachedNodeCollectionDeferral() {
    const std::uint64_t epoch =
        detachedCollectionEpoch_.fetch_add(1, std::memory_order_acq_rel) + 1;
    activeDetachedCollectionDeferrals_.fetch_add(1, std::memory_order_release);
    return epoch;
  }

  void endDetachedNodeCollectionDeferral() {
    [[maybe_unused]] const std::uint32_t previous =
        activeDetachedCollectionDeferrals_.fetch_sub(1, std::memory_order_release);
    assert(previous > 0 && "Detached-node collection deferral underflow");
  }

  bool currentThreadHasReadAccess() const {
    for (const DocumentState* documentState : activeReadDocuments_) {
      if (documentState == this) {
        return true;
      }
    }

    return false;
  }

  void pushReadAccessMarker() { activeReadDocuments_.push_back(this); }

  void popReadAccessMarker() {
    for (std::size_t index = activeReadDocuments_.size(); index > 0; --index) {
      if (activeReadDocuments_[index - 1] == this) {
        activeReadDocuments_.erase(activeReadDocuments_.begin() +
                                   static_cast<std::ptrdiff_t>(index - 1));
        return;
      }
    }

    assert(false && "DocumentReadAccess marker missing");
  }

  static void updateMax(std::atomic<std::uint64_t>& target, std::uint64_t value) {
    std::uint64_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed,
                                         std::memory_order_relaxed)) {
      // `current` is updated with the observed value after each failed exchange.
    }
  }

  static std::uint64_t elapsedNs(std::chrono::steady_clock::time_point start) {
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count());
  }

  inline static thread_local std::vector<DocumentState*> activeReadDocuments_;
  inline static thread_local DocumentState* activeWriteDocument_ = nullptr;
  inline static thread_local DocumentState* activeMutationBatchDocument_ = nullptr;
  inline static thread_local int activeMutationBatchDepth_ = 0;
  inline static thread_local bool activeMutationBatchMutated_ = false;

  std::shared_ptr<Registry> registry_;
  std::atomic<std::uint64_t> revision_ = 0;
  std::atomic<std::uint64_t> readAccesses_ = 0;
  std::atomic<std::uint64_t> writeAccesses_ = 0;
  std::atomic<std::uint64_t> readLocksAcquired_ = 0;
  std::atomic<std::uint64_t> writeLocksAcquired_ = 0;
  std::atomic<std::uint64_t> reentrantReadAccesses_ = 0;
  std::atomic<std::uint64_t> reentrantWriteAccesses_ = 0;
  std::atomic<std::uint64_t> totalReadLockHeldNs_ = 0;
  std::atomic<std::uint64_t> totalWriteLockHeldNs_ = 0;
  std::atomic<std::uint64_t> maxReadLockHeldNs_ = 0;
  std::atomic<std::uint64_t> maxWriteLockHeldNs_ = 0;
  std::atomic<std::uint32_t> activeReadLocks_ = 0;
  std::atomic<std::uint32_t> activeWriteLocks_ = 0;
  std::atomic<std::uint32_t> activeDetachedCollectionDeferrals_ = 0;
  std::atomic<std::uint64_t> detachedCollectionEpoch_ = 0;
  mutable std::mutex mutationLogMutex_;
  std::deque<DocumentMutationRecord> mutationLog_;
  std::uint64_t latestMutationSequence_ = 0;
  DetachedNodeState detachedNodeState_;
  ThreadingMode threadingMode_ = ThreadingMode::SingleThreaded;
  std::thread::id ownerThread_ = std::this_thread::get_id();
  mutable DocumentAccessLock accessMutex_;

  static constexpr std::size_t kMaxMutationLogRecords = 4096;
};

/// Scoped read access to a \ref DocumentState.
class DocumentReadAccess {
public:
  /**
   * Create a read access guard.
   *
   * @param documentState Document state being accessed.
   */
  explicit DocumentReadAccess(DocumentState& documentState);

  /// Copying access guards is not allowed.
  DocumentReadAccess(const DocumentReadAccess& other) = delete;

  /// Moving access guards transfers the held access.
  DocumentReadAccess(DocumentReadAccess&& other) noexcept;

  /// Destructor, releasing the held access.
  ~DocumentReadAccess();

  /// Copying access guards is not allowed.
  DocumentReadAccess& operator=(const DocumentReadAccess& other) = delete;

  /// Moving access guards by assignment is not needed by callers.
  DocumentReadAccess& operator=(DocumentReadAccess&& other) noexcept = delete;

  /// Get the guarded document state.
  DocumentState& documentState() const { return *documentState_; }

  /// Get the guarded registry.
  Registry& registry() const { return documentState_->registry(); }

private:
  DocumentState* documentState_;
  std::chrono::steady_clock::time_point lockAcquiredAt_;
  bool ownsReadMarker_ = false;
  bool ownsReadLockDiagnostics_ = false;
};

/// Scoped write access to a \ref DocumentState.
class DocumentWriteAccess {
public:
  /**
   * Create a write access guard.
   *
   * @param documentState Document state being accessed.
   */
  explicit DocumentWriteAccess(DocumentState& documentState);

  /// Copying access guards is not allowed.
  DocumentWriteAccess(const DocumentWriteAccess& other) = delete;

  /// Moving access guards transfers the held access.
  DocumentWriteAccess(DocumentWriteAccess&& other) noexcept;

  /// Destructor, releasing the held access.
  ~DocumentWriteAccess();

  /// Copying access guards is not allowed.
  DocumentWriteAccess& operator=(const DocumentWriteAccess& other) = delete;

  /// Moving access guards by assignment is not needed by callers.
  DocumentWriteAccess& operator=(DocumentWriteAccess&& other) noexcept = delete;

  /// Get the guarded document state.
  DocumentState& documentState() const { return *documentState_; }

  /// Get the guarded registry.
  Registry& registry() const { return documentState_->registry(); }

  /// Mark the guarded document as mutated.
  void bumpMutationRevision() const { documentState_->bumpMutationRevision(); }

private:
  DocumentState* documentState_;
  std::chrono::steady_clock::time_point lockAcquiredAt_;
  DocumentState* previousWriteDocument_ = nullptr;
  bool ownsWriteMarker_ = false;
};

/// Scoped deferral for detached-node collection during snapshot or observer epochs.
class DetachedNodeCollectionDeferral {
public:
  /**
   * Start a detached-node collection deferral.
   *
   * @param documentState Document state whose detached nodes should be retained.
   */
  explicit DetachedNodeCollectionDeferral(DocumentState& documentState)
      : documentState_(&documentState),
        epoch_(documentState.beginDetachedNodeCollectionDeferral()) {}

  /// Copying deferral guards is not allowed.
  DetachedNodeCollectionDeferral(const DetachedNodeCollectionDeferral& other) = delete;

  /// Moving deferral guards transfers the held deferral.
  DetachedNodeCollectionDeferral(DetachedNodeCollectionDeferral&& other) noexcept
      : documentState_(other.documentState_), epoch_(other.epoch_) {
    other.documentState_ = nullptr;
    other.epoch_ = 0;
  }

  /// Destructor, ending the deferral if this guard still owns it.
  ~DetachedNodeCollectionDeferral() {
    if (documentState_ != nullptr) {
      documentState_->endDetachedNodeCollectionDeferral();
    }
  }

  /// Copying deferral guards is not allowed.
  DetachedNodeCollectionDeferral& operator=(const DetachedNodeCollectionDeferral& other) = delete;

  /// Moving deferral guards by assignment is not needed by callers.
  DetachedNodeCollectionDeferral& operator=(DetachedNodeCollectionDeferral&& other) noexcept =
      delete;

  /// Epoch assigned to this deferral.
  std::uint64_t epoch() const { return epoch_; }

private:
  DocumentState* documentState_;
  std::uint64_t epoch_ = 0;
};

/// Scoped write access that coalesces nested DOM mutation revision bumps.
class DocumentMutationBatch {
public:
  /**
   * Create a mutation batch for a document.
   *
   * @param documentState Document state being mutated.
   */
  explicit DocumentMutationBatch(DocumentState& documentState)
      : access_(documentState), documentState_(&documentState) {
    coalescesRevisions_ = documentState_->beginMutationBatch();
  }

  /// Copying mutation batches is not allowed.
  DocumentMutationBatch(const DocumentMutationBatch& other) = delete;

  /// Moving mutation batches transfers the held batch.
  DocumentMutationBatch(DocumentMutationBatch&& other) noexcept
      : access_(std::move(other.access_)), documentState_(other.documentState_) {
    other.documentState_ = nullptr;
    coalescesRevisions_ = other.coalescesRevisions_;
    other.coalescesRevisions_ = false;
  }

  /// Destructor, flushing a single revision bump if the batch mutated the document.
  ~DocumentMutationBatch() {
    if (documentState_ && coalescesRevisions_) {
      documentState_->endMutationBatch();
    }
  }

  /// Copying mutation batches is not allowed.
  DocumentMutationBatch& operator=(const DocumentMutationBatch& other) = delete;

  /// Moving mutation batches by assignment is not needed by callers.
  DocumentMutationBatch& operator=(DocumentMutationBatch&& other) noexcept = delete;

  /// Get the underlying write access.
  DocumentWriteAccess& access() { return access_; }

  /// Get the underlying write access.
  const DocumentWriteAccess& access() const { return access_; }

private:
  DocumentWriteAccess access_;
  DocumentState* documentState_;
  bool coalescesRevisions_ = false;
};

inline DocumentReadAccess::DocumentReadAccess(DocumentState& documentState)
    : documentState_(&documentState) {
  if (documentState.threadingMode_ == ThreadingMode::ConcurrentDom) {
    if (DocumentState::activeWriteDocument_ != &documentState &&
        !documentState.currentThreadHasReadAccess()) {
      documentState.accessMutex_.lockRead();
      lockAcquiredAt_ = std::chrono::steady_clock::now();
      ownsReadLockDiagnostics_ = true;
    }
    documentState.pushReadAccessMarker();
    ownsReadMarker_ = true;
    documentState.recordReadAccess(ownsReadLockDiagnostics_);
  } else {
    documentState.assertSingleThreadedAccess();
  }
}

inline DocumentReadAccess::DocumentReadAccess(DocumentReadAccess&& other) noexcept
    : documentState_(other.documentState_),
      lockAcquiredAt_(other.lockAcquiredAt_),
      ownsReadMarker_(other.ownsReadMarker_),
      ownsReadLockDiagnostics_(other.ownsReadLockDiagnostics_) {
  other.documentState_ = nullptr;
  other.ownsReadMarker_ = false;
  other.ownsReadLockDiagnostics_ = false;
}

inline DocumentReadAccess::~DocumentReadAccess() {
  if (ownsReadLockDiagnostics_) {
    documentState_->recordReadLockHeld(DocumentState::elapsedNs(lockAcquiredAt_));
    documentState_->accessMutex_.unlockRead();
    documentState_->releaseReadLock();
  }

  if (ownsReadMarker_) {
    documentState_->popReadAccessMarker();
  }
}

inline DocumentWriteAccess::DocumentWriteAccess(DocumentState& documentState)
    : documentState_(&documentState) {
  if (documentState.threadingMode_ == ThreadingMode::ConcurrentDom) {
    bool acquiredLock = false;
    if (DocumentState::activeWriteDocument_ != &documentState) {
      documentState.accessMutex_.lockWrite();
      lockAcquiredAt_ = std::chrono::steady_clock::now();
      previousWriteDocument_ = DocumentState::activeWriteDocument_;
      DocumentState::activeWriteDocument_ = &documentState;
      ownsWriteMarker_ = true;
      acquiredLock = true;
    }
    documentState.recordWriteAccess(acquiredLock);
  } else {
    documentState.assertSingleThreadedAccess();
  }
}

inline DocumentWriteAccess::DocumentWriteAccess(DocumentWriteAccess&& other) noexcept
    : documentState_(other.documentState_),
      lockAcquiredAt_(other.lockAcquiredAt_),
      previousWriteDocument_(other.previousWriteDocument_),
      ownsWriteMarker_(other.ownsWriteMarker_) {
  other.documentState_ = nullptr;
  other.previousWriteDocument_ = nullptr;
  other.ownsWriteMarker_ = false;
}

inline DocumentWriteAccess::~DocumentWriteAccess() {
  if (ownsWriteMarker_) {
    DocumentState::activeWriteDocument_ = previousWriteDocument_;
    documentState_->recordWriteLockHeld(DocumentState::elapsedNs(lockAcquiredAt_));
    documentState_->accessMutex_.unlockWrite();
    documentState_->releaseWriteLock();
  }
}

inline DetachedNodeCollectionDeferral DocumentState::deferDetachedNodeCollection() {
  return DetachedNodeCollectionDeferral(*this);
}

inline DocumentReadAccess DocumentState::read() {
  return DocumentReadAccess(*this);
}

inline DocumentWriteAccess DocumentState::write() {
  return DocumentWriteAccess(*this);
}

}  // namespace donner::svg

#undef DONNER_NO_THREAD_SAFETY_ANALYSIS
