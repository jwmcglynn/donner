#include "donner/base/tasks/TaskScheduler.h"

#include <optional>

#include "donner/base/tasks/AsyncTaskScope.h"

namespace donner::tasks {

namespace {

/// A worker takes from the outside sources at least once every this many tasks it runs from its
/// own deque, so a busy worker cannot leave the global queue waiting behind its local work.
constexpr std::size_t kOutsideTakeInterval = 61;

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

std::ostream& operator<<(std::ostream& os, SchedulerStep step) {
  switch (step) {
    case SchedulerStep::AfterDrain: return os << "AfterDrain";
    case SchedulerStep::AfterSleeperDecrement: return os << "AfterSleeperDecrement";
    case SchedulerStep::BeforeSleeperWaits: return os << "BeforeSleeperWaits";
    case SchedulerStep::BeforeSelfWakeSwap: return os << "BeforeSelfWakeSwap";
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

TaskScheduler::TaskScheduler(TaskSchedulerOptions options) : TaskScheduler(options, nullptr) {}

TaskScheduler::TaskScheduler(TaskSchedulerOptions options, SchedulerTestHooks* testHooks)
    : options_(options),
      testHooks_(testHooks),
      generation_(ParkerPool::NextGeneration()),
      parkerPool_(std::make_shared<ParkerPool>()),
      externalPool_(globalMutex_, options.maxExternalTasks),
      framePool_(options.retainedCoroutineFrameBytes) {
  // A scope's count is at most the live-record bound, workers times records per worker plus the
  // external records; keep it clear of the counter's top bits.
  constexpr std::uint64_t kCountLimit = std::uint64_t{1} << 62;
  const std::uint64_t external = options_.maxExternalTasks;
  bool boundFits = external < kCountLimit;
  if (boundFits && options_.workerCount != 0) {
    boundFits = options_.maxTasksPerCreator <= (kCountLimit - 1 - external) / options_.workerCount;
  }
  UTILS_RELEASE_ASSERT_MSG(boundFits, "A task scheduler's live-record bound must be below 2^62");
  if (options_.workerCount != 0) {
    (void)WorkerStackSize(options_.workerStackBytes);  // Rejects a size the workers cannot use.
  }
  workers_.reserve(options_.workerCount);
  for (std::size_t i = 0; i < options_.workerCount; ++i) {
    workers_.push_back(std::make_unique<Worker>(*this, i, options_));
  }
  // Workers start awake and counted, before any of them runs.
  awake_.store(static_cast<std::int64_t>(workers_.size()), std::memory_order_relaxed);
  for (const std::unique_ptr<Worker>& worker : workers_) {
    worker->thread = StartThreadWithStack(options_.workerStackBytes, &WorkerEntry, worker.get());
  }
}

TaskScheduler::~TaskScheduler() {
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

  std::int64_t awakeStates = 0;
  for (const std::unique_ptr<Worker>& worker : workers_) {
    if (worker->state.load(std::memory_order_relaxed) == WorkerState::Awake) {
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
    return;
  }
  publishResumption(resumption);
}

TaskScheduler::Worker* TaskScheduler::currentWorker() const {
  return tCurrentScheduler == this ? static_cast<Worker*>(tCurrentWorker) : nullptr;
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

SchedulerWork* TaskScheduler::takeOutside(Worker& worker) {
  // The outside sources, in a fixed cyclic order. The global queue is the only one a scheduler
  // without adoptable slots has.
  constexpr std::size_t kSources = 1;
  for (std::size_t i = 0; i < kSources; ++i) {
    const std::size_t source = (worker.outsideCursor + i) % kSources;
    if (SchedulerWork* work = takeGlobal()) {
      worker.outsideCursor = (source + 1) % kSources;
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
  // A steal round: every outside source first, then every other worker once.
  if (SchedulerWork* work = takeOutside(worker)) {
    return work;
  }
  return stealFromWorkers(worker);
}

void TaskScheduler::runTask(Worker& worker, SchedulerWork* work) {
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
  runRecord(worker, static_cast<TaskRecord*>(work));
}

void TaskScheduler::runRecord(Worker& worker, TaskRecord* record) {
  // Read before the record is returned: the record may be reused at once.
  std::atomic<std::uint64_t>* const counter = record->scopeCounter();
  Parker<>* const parker = record->parker();
  AsyncTaskScope* const asyncScope = record->asyncScope();
  // Runs the closure and destroys it.
  record->run();
  if (record->pool() == &worker.pool) {
    worker.pool.releaseFromCreator(record);
  } else if (record->pool() == &externalPool_) {
    std::unique_lock lock(globalMutex_);
    externalPool_.release(record, lock);
  } else {
    // Every other record belongs to another worker of this scheduler.
    static_cast<CreatorRecordPool<>*>(const_cast<void*>(record->pool()))
        ->returnFromOtherThread(record);
  }
  // Last, so that a creator that sees its scope complete finds the task's closure destroyed and
  // its record returned.
  if (asyncScope != nullptr) {
    asyncScope->finish(worker.index);
  } else if (counter != nullptr) {
    finishScopeTask(counter, parker, worker.index);
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
    // The parker is the scheduler's (in its pool), not the scope's, so it outlives the scope.
    parker->unpark();
  }
}

bool TaskScheduler::queuedWorkVisible() const {
  return globalQueued_.load(std::memory_order_seq_cst) > 0;
}

bool TaskScheduler::swapToAwake(Worker& worker) {
  WorkerState observed = worker.state.load(std::memory_order_seq_cst);
  if (observed == WorkerState::Awake) {
    observed = WorkerState::AsleepShallow;
  }
  // Sequentially consistent on success and on failure: a failed swap's read is half of the
  // shutdown pairing, and every swap is in one total order with the awake count.
  while (!worker.state.compare_exchange_strong(
      observed, WorkerState::Awake, std::memory_order_seq_cst, std::memory_order_seq_cst)) {
    if (observed == WorkerState::Awake) {
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

bool TaskScheduler::trySleep(Worker& worker) {
  // 1. Drain the deque into the global queue, through the owner's take. An idle worker's deque is
  // already empty here, since its own take just found nothing and only it pushes; a worker waiting
  // on a scope reaches this with entries below its watermark.
  while (const std::optional<SchedulerWork*> work = worker.deque.take()) {
    publishToGlobalQueue(*work);
  }
  hook(SchedulerStep::AfterDrain, worker.index);

  // 2. Set the waiting bit; the token is the word it returns, with the bit.
  const Parker<>::Word token = worker.parker.prepareWait();

  // 3. Store asleep, then decrement the awake count; both sequentially consistent.
  // Must stay sequentially consistent: it pairs with the destructor's stop store and swap.
  worker.state.store(WorkerState::AsleepShallow, std::memory_order_seq_cst);
  awake_.fetch_sub(1, std::memory_order_seq_cst);
  hook(SchedulerStep::AfterSleeperDecrement, worker.index);

  // 4. Re-check the stop flag and the visible queued work.
  if (stop_.load(std::memory_order_seq_cst)) {
    hook(SchedulerStep::BeforeSelfWakeSwap, worker.index);
    swapToAwake(worker);
    worker.parker.finishWait();
    return false;
  }
  if (queuedWorkVisible()) {
    hook(SchedulerStep::BeforeSelfWakeSwap, worker.index);
    swapToAwake(worker);
    worker.parker.finishWait();
    return true;
  }
  hook(SchedulerStep::BeforeSleeperWaits, worker.index);

  // 5. Wait; whatever ends the wait, wake through the swap before doing anything else.
  worker.parker.wait(token);
  swapToAwake(worker);
  worker.parker.finishWait();
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
  tCurrentScheduler = nullptr;
  tCurrentWorker = nullptr;
}

}  // namespace donner::tasks
