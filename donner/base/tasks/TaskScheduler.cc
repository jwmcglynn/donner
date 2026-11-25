#include "donner/base/tasks/TaskScheduler.h"

#include <algorithm>
#include <bit>
#include <limits>
#include <optional>
#include <random>
#include <thread>
#include <unordered_map>

#include "donner/base/tasks/AsyncTaskScope.h"

namespace donner::tasks {

namespace {

/// A worker takes from the outside sources at least once every this many tasks it runs from its
/// own deque, so a busy worker cannot leave the global queue waiting behind its local work.
constexpr std::size_t kOutsideTakeInterval = 61;

constexpr std::size_t kSlotsPerWord = 64;

constexpr std::size_t SlotMaskWords(std::size_t slots) {
  return slots / kSlotsPerWord + (slots % kSlotsPerWord != 0);
}

bool LiveRecordBoundFits(const TaskSchedulerOptions& options, std::uint64_t limit) {
  if (options.maxExternalTasks >= limit || options.workerCount == 0) {
    return options.maxExternalTasks < limit;
  }
  if (options.maxTasksPerCreator == 0 ||
      options.adoptableThreads == std::numeric_limits<std::size_t>::max()) {
    return false;
  }
  const auto creators = (limit - 1 - options.maxExternalTasks) / options.maxTasksPerCreator;
  return options.workerCount <= creators &&
         options.adoptableThreads <= creators - options.workerCount;
}

/// The worker the calling thread is, of whichever scheduler created it.
thread_local const TaskScheduler* tCurrentScheduler = nullptr;
thread_local void* tCurrentWorker = nullptr;

/// One step of a 32-bit xorshift generator.
std::uint32_t XorShift32(std::uint32_t& state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

/// A nonzero seed for worker \p index, so victim sequences are reproducible per worker.
std::uint32_t StealSeed(std::size_t index) {
  return static_cast<std::uint32_t>(index) * 0x9E3779B9u + 0x7F4A7C15u;
}

/// Maps a 32-bit random value onto [0, bound) by a multiply and shift.
std::size_t ReduceRange(std::uint32_t value, std::size_t bound) {
  return static_cast<std::size_t>((static_cast<std::uint64_t>(value) * bound) >> 32);
}

}  // namespace

thread_local TaskScheduler::AdoptedSlot* TaskScheduler::adoptedOnThread_ = nullptr;
thread_local const TaskScheduler::ScopeLink* TaskScheduler::innermostOnAdoptedThread_ = nullptr;
thread_local std::size_t TaskScheduler::threadScopeCount_ = 0;

std::ostream& operator<<(std::ostream& os, SchedulerStep step) {
  switch (step) {
    case SchedulerStep::BeforeSearchStart: return os << "BeforeSearchStart";
    case SchedulerStep::AfterEmptySearch: return os << "AfterEmptySearch";
    case SchedulerStep::BeforeAdoptedStealClaim: return os << "BeforeAdoptedStealClaim";
    case SchedulerStep::AfterAdoptedPush: return os << "AfterAdoptedPush";
    case SchedulerStep::AfterSearchStart: return os << "AfterSearchStart";
    case SchedulerStep::AfterDrain: return os << "AfterDrain";
    case SchedulerStep::AfterWaitingBit: return os << "AfterWaitingBit";
    case SchedulerStep::AfterSleeperDecrement: return os << "AfterSleeperDecrement";
    case SchedulerStep::BeforeSleeperWaits: return os << "BeforeSleeperWaits";
    case SchedulerStep::BeforeSelfWakeSwap: return os << "BeforeSelfWakeSwap";
    case SchedulerStep::BeforeHandOff: return os << "BeforeHandOff";
    case SchedulerStep::AfterPublishPush: return os << "AfterPublishPush";
    case SchedulerStep::BetweenWakeSwapAndParkerChange:
      return os << "BetweenWakeSwapAndParkerChange";
    case SchedulerStep::AfterStopFlagSet: return os << "AfterStopFlagSet";
    case SchedulerStep::ScopeWaiterBeforePrepareWait: return os << "ScopeWaiterBeforePrepareWait";
    case SchedulerStep::ScopeWaiterBeforeWait: return os << "ScopeWaiterBeforeWait";
    case SchedulerStep::ResumptionPublished: return os << "ResumptionPublished";
    case SchedulerStep::AsyncJoinBeforeRegister: return os << "AsyncJoinBeforeRegister";
    case SchedulerStep::AsyncJoinRegistered: return os << "AsyncJoinRegistered";
    case SchedulerStep::AsyncJoinFinisherAfterDecrement:
      return os << "AsyncJoinFinisherAfterDecrement";
    case SchedulerStep::AsyncJoinBeforeEnqueue: return os << "AsyncJoinBeforeEnqueue";
    case SchedulerStep::ScopeFinisherAfterDecrement: return os << "ScopeFinisherAfterDecrement";
    case SchedulerStep::ParallelForBeforeClaim: return os << "ParallelForBeforeClaim";
    case SchedulerStep::WorkerBeforeStartupBarrier: return os << "WorkerBeforeStartupBarrier";
    case SchedulerStep::ParallelForAfterClaim: return os << "ParallelForAfterClaim";
  }
  UTILS_UNREACHABLE();
}

TaskScheduler::Worker::Worker(TaskScheduler& scheduler, std::size_t index,
                              const TaskSchedulerOptions& options)
    : scheduler(scheduler),
      index(index),
      deque(options.dequeCapacity),
      pool(options.maxTasksPerCreator),
      stealRng(StealSeed(index)) {}

struct TaskScheduler::SeededState {
  SeededState(std::uint64_t seed, std::size_t capacity) : random(seed), pool(capacity) {}

  TaskRecord* take(const std::atomic<std::uint64_t>& count) {
    const auto queue = readyByScope.find(&count);
    UTILS_RELEASE_ASSERT_MSG(queue != readyByScope.end(),
                             "A seeded scope is waiting with no runnable task");
    auto& ready = queue->second;
    const std::size_t index =
        ready.size() == 1 ? 0 : static_cast<std::size_t>(random() % ready.size());
    TaskRecord* const result = ready[index];
    ready[index] = ready.back();
    ready.pop_back();
    if (ready.empty()) {
      readyByScope.erase(queue);
    }
    return result;
  }

  const std::thread::id owner = std::this_thread::get_id();
  std::mt19937_64 random;
  CreatorRecordPool<> pool;
  std::unordered_map<const std::atomic<std::uint64_t>*, std::vector<TaskRecord*>> readyByScope;
};

TaskScheduler::TaskScheduler(TaskSchedulerOptions options) : TaskScheduler(options, nullptr) {}

TaskScheduler::TaskScheduler(TaskSchedulerOptions options, SchedulerTestHooks* testHooks)
    : options_(options),
      testHooks_(testHooks),
      generation_(ParkerPool::NextGeneration()),
      parkerPool_(std::make_shared<ParkerPool>()),
      externalPool_(globalMutex_, options.maxExternalTasks),
      framePool_(options.retainedCoroutineFrameBytes) {
  // Worker pools, adopted slots and the shared external pool bound the number of live records.
  // Keep a scope's counter clear of its reserved high bits.
  constexpr std::uint64_t kCountLimit = std::uint64_t{1} << 62;
  if (options_.seed.has_value()) {
    UTILS_RELEASE_ASSERT_MSG(options_.maxTasksPerCreator < kCountLimit,
                             "A task scheduler's live-record bound must be below 2^62");
    seeded_ = std::make_unique<SeededState>(*options_.seed, options_.maxTasksPerCreator);
    return;
  }
  UTILS_RELEASE_ASSERT_MSG(LiveRecordBoundFits(options_, kCountLimit),
                           "A task scheduler's live-record bound must be below 2^62");
  if (options_.workerCount != 0) {
    (void)WorkerStackSize(options_.workerStackBytes);  // Rejects a size the workers cannot use.
  }
  workers_.reserve(options_.workerCount);
  // A new thread must see the complete, densely indexed roster before scanning its peers.
  std::lock_guard startup(globalMutex_);
  for (std::size_t attempt = 0; attempt < options_.workerCount; ++attempt) {
    auto worker = std::make_unique<Worker>(*this, workers_.size(), options_);
    const WorkerThreadCreate create =
        testHooks_ ? testHooks_->workerThreadCreate(attempt) : &pthread_create;
    if (const auto thread = TryStartThreadWithStack(options_.workerStackBytes, &WorkerEntry,
                                                    worker.get(), create)) {
      worker->thread = *thread;
      workers_.push_back(std::move(worker));
    }
  }
  initializeAdoptedSlots();
  awake_.store(static_cast<std::int64_t>(workers_.size()), std::memory_order_relaxed);
}

TaskScheduler::~TaskScheduler() {
  UTILS_RELEASE_ASSERT_MSG(!hasAdoptedThread(),
                           "A task scheduler was destroyed with a live AdoptedThread");
  framePool_.assertNoLiveFrames();
  UTILS_RELEASE_ASSERT_MSG(
      liveScopes_.load(std::memory_order_relaxed) == 0,
      "A task scheduler was destroyed with a live TaskScope or AsyncTaskScope");
  // Sequentially consistent: half of the shutdown pairing with each sleeper's state store and its
  // re-check of this flag.
  stop_.exchange(true, std::memory_order_seq_cst);
  hook(SchedulerStep::AfterStopFlagSet, 0);
  for (const std::unique_ptr<Worker>& worker : workers_) {
    wakeWorker(*worker);
  }
  for (const std::unique_ptr<Worker>& worker : workers_) {
    UTILS_RELEASE_ASSERT_MSG(pthread_join(worker->thread, nullptr) == 0,
                             "A task scheduler worker thread could not be joined");
  }

  UTILS_RELEASE_ASSERT_MSG(searching_.load(std::memory_order_relaxed) == 0,
                           "A stopped scheduler still has searching workers");
  std::int64_t awakeStates = 0;
  for (const std::unique_ptr<Worker>& worker : workers_) {
    if (IsWorkerAwake(worker->state.load(std::memory_order_relaxed))) {
      ++awakeStates;
    }
    UTILS_RELEASE_ASSERT_MSG(!worker->deque.take().has_value(),
                             "A task scheduler was destroyed with queued tasks");
  }
  UTILS_RELEASE_ASSERT_MSG(awake_.load(std::memory_order_relaxed) == awakeStates,
                           "The awake count disagrees with the workers' state words");
  UTILS_RELEASE_ASSERT_MSG(
      globalHead_ == nullptr && globalQueued_.load(std::memory_order_relaxed) == 0,
      "A task scheduler was destroyed with queued tasks");

  // Every worker is joined and no scope is live, so none of the pool's slots will be used again.
  parkerPool_->close();
}

void TaskScheduler::initializeAdoptedSlots() {
  if (workers_.empty() || options_.adoptableThreads == 0) {
    return;
  }
  adoptedSlots_.reserve(options_.adoptableThreads);
  claimedSlots_ =
      std::make_unique<std::atomic<std::uint64_t>[]>(SlotMaskWords(options_.adoptableThreads));
  for (std::size_t i = 0; i < options_.adoptableThreads; ++i) {
    adoptedSlots_.push_back(std::make_unique<AdoptedSlot>(*this, i, options_));
  }
}

std::optional<std::size_t> TaskScheduler::claimAdoptedSlot() {
  const std::size_t slots = adoptedSlots_.size();
  for (std::size_t word = 0; word < SlotMaskWords(slots); ++word) {
    const std::size_t remaining = slots - word * kSlotsPerWord;
    const std::uint64_t valid = remaining < kSlotsPerWord
                                    ? (std::uint64_t{1} << remaining) - 1
                                    : std::numeric_limits<std::uint64_t>::max();
    auto& mask = claimedSlots_[word];
    std::uint64_t claimed = mask.load(std::memory_order_relaxed);
    while ((valid & ~claimed) != 0) {
      const auto bit = static_cast<std::size_t>(std::countr_zero(valid & ~claimed));
      if (mask.compare_exchange_weak(claimed, claimed | (std::uint64_t{1} << bit),
                                     std::memory_order_acq_rel, std::memory_order_relaxed)) {
        return word * kSlotsPerWord + bit;
      }
    }
  }
  return std::nullopt;
}

AdoptedThread TaskScheduler::adopt() {
  UTILS_RELEASE_ASSERT_MSG(tCurrentScheduler == nullptr, "A scheduler worker cannot adopt a slot");
  UTILS_RELEASE_ASSERT_MSG(threadScopeCount_ == 0, "A thread cannot adopt with a live TaskScope");
  if (const auto index = claimAdoptedSlot()) {
    AdoptedSlot& slot = *adoptedSlots_[*index];
    slot.stack = CurrentThreadStackBounds();
    slot.previousOnThread = adoptedOnThread_;
    adoptedOnThread_ = &slot;
    return AdoptedThread(*this, *index);
  }
  return {};
}

AdoptedThread::~AdoptedThread() {
  if (scheduler_ != nullptr) {
    scheduler_->releaseAdoptedSlot(index_);
  }
}

void TaskScheduler::releaseAdoptedSlot(std::size_t index) {
  UTILS_RELEASE_ASSERT_MSG(threadScopeCount_ == 0,
                           "An AdoptedThread was released with a live TaskScope");
  AdoptedSlot& slot = *adoptedSlots_[index];
  AdoptedSlot** link = &adoptedOnThread_;
  while (*link != nullptr && *link != &slot) {
    link = &(*link)->previousOnThread;
  }
  UTILS_RELEASE_ASSERT_MSG(*link == &slot, "An AdoptedThread was released on a different thread");
  while (const auto record = slot.deque.take()) {
    publishToGlobalQueue(*record);
  }
  *link = slot.previousOnThread;
  slot.previousOnThread = nullptr;
  // A later claim acquires the empty deque and the pool's owner-only state.
  claimedSlots_[index / kSlotsPerWord].fetch_and(~(std::uint64_t{1} << (index % kSlotsPerWord)),
                                                 std::memory_order_release);
}

bool TaskScheduler::hasAdoptedThread() const {
  for (std::size_t i = 0; i < SlotMaskWords(adoptedSlots_.size()); ++i) {
    if (claimedSlots_[i].load(std::memory_order_relaxed) != 0) {
      return true;
    }
  }
  return false;
}

TaskScheduler::AdoptedSlot* TaskScheduler::currentAdoptedSlot() const {
  for (AdoptedSlot* slot = adoptedOnThread_; slot != nullptr; slot = slot->previousOnThread) {
    if (&slot->scheduler == this) {
      return slot;
    }
  }
  return nullptr;
}

void TaskScheduler::recordAdoptedBirth(ScopeLink& scope) {
  // The stamp survives guard/slot reuse and does not retain an unrelated scheduler's storage.
  static thread_local const std::uint64_t thread = ParkerPool::NextGeneration();
  static thread_local std::uint64_t order = 0;
  UTILS_RELEASE_ASSERT_MSG(thread != 0 && order != std::numeric_limits<std::uint64_t>::max(),
                           "Adopted scope creation order exhausted");
  scope.birthThread = thread;
  scope.birthOrder = ++order;
}

bool TaskScheduler::adoptedSubmissionNeedsGlobal(const ScopeLink* scope) {
  const ScopeLink* inner = innermostOnAdoptedThread_;
  if (inner == nullptr || inner == scope) {
    return false;
  }
  // The active scope chain belongs to the thread, even across scheduler boundaries.
  UTILS_RELEASE_ASSERT_MSG(scope != nullptr && scope->asyncScope != nullptr &&
                               scope->birthThread == inner->birthThread &&
                               scope->birthOrder > inner->birthOrder,
                           "An adopted thread may only spawn into its innermost synchronous scope "
                           "or a younger async scope");
  return true;
}

ScheduleAwaitable TaskScheduler::schedule() noexcept {
  return ScheduleAwaitable(*this);
}

void ScheduleAwaitable::await_suspend(std::coroutine_handle<> handle) noexcept {
  resumption_.handle = handle;
  scheduler_.enqueueResumption(resumption_);
}

void TaskScheduler::enqueueResumption(CoroutineResumption& resumption) {
  UTILS_RELEASE_ASSERT_MSG(!stop_.load(std::memory_order_seq_cst),
                           "A coroutine tried to suspend while its scheduler was shutting down");
  if (Worker* worker = currentWorker(); worker != nullptr && worker->deque.push(&resumption)) {
    wakeForParallelism();
    return;
  }
  publishResumption(resumption);
}

TaskScheduler::Worker* TaskScheduler::currentWorker() const {
  return tCurrentScheduler == this ? static_cast<Worker*>(tCurrentWorker) : nullptr;
}

void TaskScheduler::checkSeededOwner() const {
  UTILS_RELEASE_ASSERT_MSG(seeded_->owner == std::this_thread::get_id(),
                           "A seeded scheduler runs only on its constructing thread");
}

TaskRecord* TaskScheduler::allocateSeededRecord() {
  assertSeededOwner();
  return seeded_->pool.tryAllocate();
}

void TaskScheduler::enqueueSeededRecord(TaskRecord* record) {
  seeded_->readyByScope[record->scopeCounter()].push_back(record);
}

void TaskScheduler::drainSeededUntil(const std::atomic<std::uint64_t>& count) {
  assertSeededOwner();
  while (count.load(std::memory_order_acquire) != 0) {
    runRecord(seeded_->pool, 0, seeded_->take(count));
  }
}

TaskRecord* TaskScheduler::allocateExternalRecord() {
  std::unique_lock lock(globalMutex_);
  return externalPool_.tryAllocate(lock);
}

void TaskScheduler::linkGlobal(SchedulerWork* work) {
  work->next = nullptr;
  if (globalTail_ == nullptr) {
    globalHead_ = work;
  } else {
    globalTail_->next = work;
  }
  globalTail_ = work;
  // Sequentially consistent: half of the publish pairing with a sleeper's decrement and re-check.
  globalQueued_.fetch_add(1, std::memory_order_seq_cst);
}

void TaskScheduler::publishToGlobalQueue(SchedulerWork* work) {
  {
    std::unique_lock lock(globalMutex_);
    linkGlobal(work);
  }
  hook(SchedulerStep::AfterPublishPush, 0);
  wakeAfterPublish();
}

void TaskScheduler::publishResumption(CoroutineResumption& resumption) {
  SchedulerTestHooks* const hooks = testHooks_;
  {
    std::unique_lock lock(globalMutex_);
    linkGlobal(&resumption);
    wakeAfterPublish();
  }
  // A consumer may now complete and release this scheduler and the suspended frame.
  if (hooks != nullptr) {
    hooks->at(SchedulerStep::ResumptionPublished, 0);
  }
}

void TaskScheduler::wakeAfterPublish() {
  // Sequentially consistent: the other half of the publish pairing. A count driven below zero by
  // a defect still leads to a wake rather than to silence.
  if (awake_.load(std::memory_order_seq_cst) <= 0) {
    wakeAsleepWorker();
  }
}

SchedulerWork* TaskScheduler::takeGlobal() {
  // A hint that skips the mutex for an empty queue; the take itself is under the mutex.
  if (globalQueued_.load(std::memory_order_relaxed) == 0) {
    return nullptr;
  }
  std::unique_lock lock(globalMutex_);
  SchedulerWork* work = globalHead_;
  if (work == nullptr) {
    return nullptr;
  }
  globalHead_ = work->next;
  if (globalHead_ == nullptr) {
    globalTail_ = nullptr;
  }
  work->next = nullptr;
  globalQueued_.fetch_sub(1, std::memory_order_seq_cst);
  return work;
}

TaskRecord* TaskScheduler::stealFromSlot(AdoptedSlot& slot, std::size_t workerIndex) {
  StealResult<TaskRecord*> result;
  do {
    result = testHooks_ == nullptr ? slot.deque.steal() : slot.deque.stealWithHook([&] {
      hook(SchedulerStep::BeforeAdoptedStealClaim, workerIndex);
    });
  } while (result.status == StealStatus::Lost);
  return result.status == StealStatus::Taken ? result.item : nullptr;
}

SchedulerWork* TaskScheduler::takeOutside(Worker& worker) {
  const std::size_t sources = 1 + adoptedSlots_.size();
  std::size_t source = worker.outsideCursor;
  for (std::size_t i = 0; i < sources; ++i) {
    SchedulerWork* work =
        source == 0 ? takeGlobal() : stealFromSlot(*adoptedSlots_[source - 1], worker.index);
    source = source + 1 == sources ? 0 : source + 1;
    if (work != nullptr) {
      worker.outsideCursor = source;
      return work;
    }
  }
  return nullptr;
}

SchedulerWork* TaskScheduler::stealFromWorkers(Worker& worker) {
  const std::size_t others = workers_.size() - 1;
  if (others == 0) {
    return nullptr;
  }
  const std::size_t offset = ReduceRange(XorShift32(worker.stealRng), others);
  for (std::size_t k = 0; k < others; ++k) {
    const std::size_t victim = StealVictim(worker.index, workers_.size(), offset, k);
    const StealResult<SchedulerWork*> result = workers_[victim]->deque.steal();
    if (result.status == StealStatus::Taken) {
      return result.item;
    }
  }
  return nullptr;
}

SchedulerWork* TaskScheduler::findTask(Worker& worker) {
  if (worker.localRunsSinceOutside >= kOutsideTakeInterval) {
    worker.localRunsSinceOutside = 0;
    if (SchedulerWork* work = takeOutside(worker)) {
      return work;
    }
  }
  if (const std::optional<SchedulerWork*> local = worker.deque.take()) {
    ++worker.localRunsSinceOutside;
    return *local;
  }
  return findOtherWork(worker);
}

SchedulerWork* TaskScheduler::findOtherWork(Worker& worker) {
  startSearching(worker);
  if (SchedulerWork* work = takeOutside(worker)) {
    return work;
  }
  return stealFromWorkers(worker);
}

void TaskScheduler::runTask(Worker& worker, SchedulerWork* work) {
  finishSearchWithWork(worker);
  UTILS_RELEASE_ASSERT_MSG(
      StackRemaining(CurrentStackPosition(), worker.stack) >= kStackReserveBytes,
      "A task scheduler worker is out of stack: less than the reserve is "
      "left below the task it would run");
  // The dispatch branch lets both queues carry frame-backed resumptions without new records.
  if (work->kind == SchedulerWork::Kind::Resumption) {
    const auto handle = static_cast<CoroutineResumption*>(work)->handle;
    handle.resume();
    return;
  }
  runRecord(worker.pool, worker.index, static_cast<TaskRecord*>(work));
}

void TaskScheduler::runRecord(CreatorRecordPool<>& localPool, std::size_t workerIndex,
                              TaskRecord* record) {
  // Read before the record is returned: the record may be reused at once.
  std::atomic<std::uint64_t>* const counter = record->scopeCounter();
  Parker<>* const parker = record->parker();
  AsyncTaskScope* const asyncScope = record->asyncScope();
  if (ScopeStop::SkipsQueuedTask(record->stop())) {
    record->discard();
  } else {
    record->run();
  }
  if (record->pool() == &localPool) {
    localPool.releaseFromCreator(record);
  } else if (record->pool() == &externalPool_) {
    std::unique_lock lock(globalMutex_);
    externalPool_.release(record, lock);
  } else {
    // Every other record belongs to a worker or adopted slot of this scheduler.
    static_cast<CreatorRecordPool<>*>(const_cast<void*>(record->pool()))
        ->returnFromOtherThread(record);
  }
  // Last, so that a creator that sees its scope complete finds the task's closure destroyed and
  // its record returned.
  if (asyncScope != nullptr) {
    asyncScope->finish(workerIndex);
  } else if (counter != nullptr) {
    finishScopeTask(counter, parker, workerIndex);
  }
}

void TaskScheduler::finishScopeTask(std::atomic<std::uint64_t>* counter, Parker<>* parker,
                                    std::size_t workerIndex) {
  // Acquire-release: the release half publishes the task's results to the creator; all the
  // decrements form one release sequence. This is the task's last access to the scope, which its
  // creator may destroy as soon as it reads zero.
  const std::uint64_t before = counter->fetch_sub(1, std::memory_order_acq_rel);
  UTILS_RELEASE_ASSERT_MSG(before != 0, "A scope's task count went below zero");
  if (before == 1) {
    hook(SchedulerStep::ScopeFinisherAfterDecrement, workerIndex);
    if (parker == nullptr) {
      return;  // The seeded caller runs this record itself.
    }
    // The parker is the scheduler's (a worker's, or in its pool), not the scope's, so it outlives
    // the scope.
    if (Worker* creator = workerOwning(parker)) {
      // A creator that is a worker may be asleep in its wait: the swap counts it awake before the
      // parker change lets it run.
      wakeWorker(*creator);
    } else {
      parker->unpark();
    }
  }
}

TaskScheduler::Worker* TaskScheduler::workerOwning(const Parker<>* parker) const {
  for (const std::unique_ptr<Worker>& worker : workers_) {
    if (&worker->parker == parker) {
      return worker.get();
    }
  }
  return nullptr;
}

void TaskScheduler::waitOnScope(Worker& worker, const std::atomic<std::uint64_t>& count,
                                WorkStealingDeque<SchedulerWork*>::Index watermark) {
  while (true) {
    // The next take claims bottom - 1. The entry at the watermark belongs to this wait.
    if (worker.deque.bottom() > watermark) {
      if (const std::optional<SchedulerWork*> work = worker.deque.take()) {
        runTask(worker, *work);
        continue;
      }
    }

    // Acquire the task results before returning to the creator.
    if (count.load(std::memory_order_acquire) == 0) {
      stopSearching(worker);
      return;
    }

    const bool withinMargin =
        HelpsWithin(StackRemaining(CurrentStackPosition(), worker.stack), options_.helpMarginBytes);
    if (withinMargin) {
      if (SchedulerWork* work = findOtherWork(worker)) {
        runTask(worker, work);
        continue;
      }
    }

    switch (sleepWhileWaiting(worker, count, !withinMargin)) {
      case WaitSleep::ScopeComplete: return;
      case WaitSleep::Resume: break;
      case WaitSleep::RunQueuedPastMargin:
        // Queued work was visible; another worker may have taken it since, and the rules then
        // run again.
        if (SchedulerWork* work = takeOutside(worker)) {
          pastMarginRuns_.fetch_add(1, std::memory_order_relaxed);
          runTask(worker, work);
        }
        break;
    }
  }
}

TaskScheduler::Worker* TaskScheduler::findAsleepShallow(const Worker& self) const {
  for (const std::unique_ptr<Worker>& worker : workers_) {
    if (worker.get() != &self &&
        worker->state.load(std::memory_order_seq_cst) == WorkerState::AsleepShallow) {
      return worker.get();
    }
  }
  return nullptr;
}

TaskScheduler::WaitSleep TaskScheduler::sleepWhileWaiting(Worker& worker,
                                                          const std::atomic<std::uint64_t>& count,
                                                          bool pastMargin) {
  const auto sleep =
      prepareSleep(worker, pastMargin ? WorkerState::AsleepDeep : WorkerState::AsleepShallow);

  // 4. Re-check. The stop flag is set only with no scope live, so a waiting creator never sees it.
  UTILS_RELEASE_ASSERT_MSG(!stop_.load(std::memory_order_seq_cst),
                           "A task scheduler was destroyed with a live TaskScope");
  // The finisher may have decremented before swapping our state or changing our parker.
  if (count.load(std::memory_order_acquire) == 0) {
    hook(SchedulerStep::BeforeSelfWakeSwap, worker.index);
    finishSleep(worker);
    stopSearching(worker);
    return WaitSleep::ScopeComplete;
  }
  if (queuedWorkVisible()) {
    if (!pastMargin) {
      hook(SchedulerStep::BeforeSelfWakeSwap, worker.index);
      finishSleep(worker);
      return WaitSleep::Resume;
    }
    // A transiently low count errs toward staying awake.
    const bool lastAwake =
        sleep.awakeBefore <= 1 && (testHooks_ == nullptr || testHooks_->lastAwakeRule());
    if (lastAwake) {
      if (Worker* shallow = findAsleepShallow(worker)) {
        // The hand-off: whether or not this swap succeeds, an awake worker exists that re-checks
        // before it next sleeps, and it runs the work from its shallow stack.
        hook(SchedulerStep::BeforeHandOff, worker.index);
        wakeWorker(*shallow);
      } else {
        hook(SchedulerStep::BeforeSelfWakeSwap, worker.index);
        finishSleep(worker);
        return WaitSleep::RunQueuedPastMargin;
      }
    }
  }
  if (!pastMargin && runAfterFinalSearch(worker, sleep)) {
    return WaitSleep::Resume;
  }
  hook(SchedulerStep::BeforeSleeperWaits, worker.index);

  // 5. Wait; whatever ends the wait, wake through the swap before doing anything else.
  worker.parker.wait(sleep.token);
  finishSleep(worker);
  return WaitSleep::Resume;
}

bool TaskScheduler::queuedWorkVisible() const {
  if (globalQueued_.load(std::memory_order_seq_cst) > 0) {
    return true;
  }
  return std::any_of(adoptedSlots_.begin(), adoptedSlots_.end(),
                     [](const auto& slot) { return slot->deque.publishedWorkVisible(); });
}

bool TaskScheduler::swapToAwake(Worker& worker) {
  WorkerState observed = worker.state.load(std::memory_order_seq_cst);
  if (IsWorkerAwake(observed)) {
    observed = WorkerState::AsleepShallow;
  }
  // Sequentially consistent on success and on failure: a failed swap's read is half of the
  // shutdown pairing, and every swap is in one total order with the awake count.
  while (!worker.state.compare_exchange_strong(
      observed, WorkerState::Awake, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
    if (IsWorkerAwake(observed)) {
      return false;
    }
  }
  awake_.fetch_add(1, std::memory_order_seq_cst);
  return true;
}

void TaskScheduler::wakeWorker(Worker& worker) {
  swapToAwake(worker);
  hook(SchedulerStep::BetweenWakeSwapAndParkerChange, worker.index);
  // Changed whether or not the swap succeeded: at worst a spurious wake.
  worker.parker.unpark();
}

void TaskScheduler::finishSleep(Worker& worker) {
  const auto previous = worker.state.exchange(WorkerState::Awake, std::memory_order_seq_cst);
  if (previous == WorkerState::AwakeReserved) {
    hook(SchedulerStep::BeforeSearchStart, worker.index);
    worker.searching = true;
    hook(SchedulerStep::AfterSearchStart, worker.index);
  } else if (previous != WorkerState::Awake) {
    awake_.fetch_add(1, std::memory_order_seq_cst);
  }
  worker.parker.finishWait();
}

bool TaskScheduler::runAfterFinalSearch(Worker& worker, const SleepPreparation& sleep) {
  if (sleep.rescanPeers) {
    if (SchedulerWork* work = stealFromWorkers(worker)) {
      hook(SchedulerStep::BeforeSelfWakeSwap, worker.index);
      finishSleep(worker);
      startSearching(worker);
      runTask(worker, work);
      return true;
    }
  }
  return false;
}

void TaskScheduler::passSearchCredit() {
  if (searching_.load(std::memory_order_relaxed) == 1 &&
      awake_.load(std::memory_order_relaxed) < static_cast<std::int64_t>(workers_.size()) &&
      (testHooks_ == nullptr || testHooks_->allowParallelismWake())) {
    for (const auto& candidate : workers_) {
      if (candidate->state.load(std::memory_order_relaxed) != WorkerState::AsleepShallow) {
        continue;
      }
      WorkerState expected = WorkerState::AsleepShallow;
      if (candidate->state.compare_exchange_strong(expected, WorkerState::AwakeReserved,
                                                   std::memory_order_seq_cst)) {
        awake_.fetch_add(1, std::memory_order_seq_cst);
        hook(SchedulerStep::BetweenWakeSwapAndParkerChange, candidate->index);
        candidate->parker.unpark();
        return;
      }
    }
  }
  searching_.fetch_sub(1, std::memory_order_relaxed);
}

void TaskScheduler::finishSearchWithWork(Worker& worker) {
  if (!worker.searching) {
    return;
  }
  worker.searching = false;
  auto count = searching_.load(std::memory_order_relaxed);
  while (count != 1) {
    if (searching_.compare_exchange_weak(count, count - 1, std::memory_order_relaxed)) {
      return;
    }
  }
  // Keep the credit counted while choosing a successor: producers cannot start a second chain.
  passSearchCredit();
}

void TaskScheduler::startSearching(Worker& worker) {
  if (!worker.searching) {
    hook(SchedulerStep::BeforeSearchStart, worker.index);
    worker.searching = true;
    searching_.fetch_add(1, std::memory_order_relaxed);
    hook(SchedulerStep::AfterSearchStart, worker.index);
  }
}

bool TaskScheduler::stopSearching(Worker& worker) {
  if (!worker.searching) {
    return false;
  }
  worker.searching = false;
  return searching_.fetch_sub(1, std::memory_order_relaxed) == 1;
}

void TaskScheduler::wakeAsleepWorker() {
  for (const WorkerState asleep : {WorkerState::AsleepShallow, WorkerState::AsleepDeep}) {
    for (const std::unique_ptr<Worker>& worker : workers_) {
      if (worker->state.load(std::memory_order_seq_cst) == asleep) {
        wakeWorker(*worker);
        return;
      }
    }
  }
}

TaskScheduler::SleepPreparation TaskScheduler::prepareSleep(Worker& worker, WorkerState state) {
  const bool rescanPeers = worker.searching && state == WorkerState::AsleepShallow;
  if (worker.searching) {
    hook(SchedulerStep::AfterEmptySearch, worker.index);
  }
  stopSearching(worker);
  while (const std::optional<SchedulerWork*> work = worker.deque.take()) {
    publishToGlobalQueue(*work);
  }
  hook(SchedulerStep::AfterDrain, worker.index);

  const Parker<>::Word token = worker.parker.prepareWait();
  hook(SchedulerStep::AfterWaitingBit, worker.index);

  // These operations pair with publishers and shutdown in the same total order.
  worker.state.store(state, std::memory_order_seq_cst);
  const std::int64_t awakeBefore = awake_.fetch_sub(1, std::memory_order_seq_cst);
  hook(SchedulerStep::AfterSleeperDecrement, worker.index);
  return {token, awakeBefore, rescanPeers};
}

bool TaskScheduler::trySleep(Worker& worker) {
  const auto sleep = prepareSleep(worker, WorkerState::AsleepShallow);
  // 4. Re-check the stop flag and the visible queued work.
  if (stop_.load(std::memory_order_seq_cst)) {
    hook(SchedulerStep::BeforeSelfWakeSwap, worker.index);
    finishSleep(worker);
    return false;
  }
  if (queuedWorkVisible()) {
    hook(SchedulerStep::BeforeSelfWakeSwap, worker.index);
    finishSleep(worker);
    return true;
  }
  if (runAfterFinalSearch(worker, sleep)) {
    return true;
  }
  hook(SchedulerStep::BeforeSleeperWaits, worker.index);

  // 5. Wait; whatever ends the wait, wake through the swap before doing anything else.
  worker.parker.wait(sleep.token);
  finishSleep(worker);
  return !stop_.load(std::memory_order_seq_cst);
}

std::optional<std::size_t> TaskScheduler::CurrentWorkerStackRemaining() {
  const Worker* worker = static_cast<const Worker*>(tCurrentWorker);
  if (worker == nullptr) {
    return std::nullopt;
  }
  return StackRemaining(CurrentStackPosition(), worker->stack);
}

void* TaskScheduler::WorkerEntry(void* worker) {
  Worker& self = *static_cast<Worker*>(worker);
  self.scheduler.hook(SchedulerStep::WorkerBeforeStartupBarrier, self.index);
  {
    // Acquire the final roster and initial awake count from the constructor.
    std::lock_guard startup(self.scheduler.globalMutex_);
  }
  const WorkerQos qos = self.scheduler.options_.workerQos;
  const int error = self.scheduler.testHooks_ ? self.scheduler.testHooks_->setWorkerQos(qos)
                                              : detail::TrySetCurrentWorkerQos(qos);
  if (error != 0) {
    self.scheduler.workerQosRefusals_.fetch_add(1, std::memory_order_relaxed);
  }
  self.stack = UsableStack(CurrentThreadStackBounds(),
                           WorkerStackSize(self.scheduler.options_.workerStackBytes));
  self.scheduler.workerMain(self);
  return nullptr;
}

void TaskScheduler::workerMain(Worker& worker) {
  tCurrentScheduler = this;
  tCurrentWorker = &worker;
  std::size_t emptyRounds = 0;
  while (!stop_.load(std::memory_order_seq_cst)) {
    if (SchedulerWork* work = findTask(worker)) {
      emptyRounds = 0;
      runTask(worker, work);
      continue;
    }
    if (++emptyRounds < options_.idleSpinRounds) {
      continue;
    }
    emptyRounds = 0;
    if (!trySleep(worker)) {
      break;
    }
  }
  stopSearching(worker);
  tCurrentScheduler = nullptr;
  tCurrentWorker = nullptr;
}

}  // namespace donner::tasks
