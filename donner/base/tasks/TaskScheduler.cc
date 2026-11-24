#include "donner/base/tasks/TaskScheduler.h"

#include <optional>

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
  }
  UTILS_UNREACHABLE();
}

TaskScheduler::Worker::Worker(std::size_t index, const TaskSchedulerOptions& options)
    : index(index),
      deque(options.dequeCapacity),
      pool(options.maxTasksPerCreator),
      stealRng(StealSeed(index)) {}

TaskScheduler::TaskScheduler(TaskSchedulerOptions options) : TaskScheduler(options, nullptr) {}

TaskScheduler::TaskScheduler(TaskSchedulerOptions options, SchedulerTestHooks* testHooks)
    : options_(options),
      testHooks_(testHooks),
      externalPool_(globalMutex_, options.maxExternalTasks) {
  workers_.reserve(options_.workerCount);
  for (std::size_t i = 0; i < options_.workerCount; ++i) {
    workers_.push_back(std::make_unique<Worker>(i, options_));
  }
  // Workers start awake and counted, before any of them runs.
  awake_.store(static_cast<std::int64_t>(workers_.size()), std::memory_order_relaxed);
  for (const std::unique_ptr<Worker>& worker : workers_) {
    Worker* w = worker.get();
    w->thread = std::thread([this, w] { workerMain(*w); });
  }
}

TaskScheduler::~TaskScheduler() {
  // Sequentially consistent: half of the shutdown pairing with each sleeper's state store and its
  // re-check of this flag.
  stop_.exchange(true, std::memory_order_seq_cst);
  hook(SchedulerStep::AfterStopFlagSet, 0);
  for (const std::unique_ptr<Worker>& worker : workers_) {
    wakeWorker(*worker);
  }
  for (const std::unique_ptr<Worker>& worker : workers_) {
    worker->thread.join();
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
}

TaskScheduler::Worker* TaskScheduler::currentWorker() const {
  return tCurrentScheduler == this ? static_cast<Worker*>(tCurrentWorker) : nullptr;
}

TaskRecord* TaskScheduler::allocateExternalRecord() {
  std::unique_lock lock(globalMutex_);
  return externalPool_.tryAllocate(lock);
}

void TaskScheduler::pushGlobal(TaskRecord* record) {
  std::unique_lock lock(globalMutex_);
  record->setNext(nullptr);
  if (globalTail_ == nullptr) {
    globalHead_ = record;
  } else {
    globalTail_->setNext(record);
  }
  globalTail_ = record;
  // Sequentially consistent: half of the publish pairing with a sleeper's decrement and re-check.
  globalQueued_.fetch_add(1, std::memory_order_seq_cst);
}

void TaskScheduler::publishToGlobalQueue(TaskRecord* record) {
  pushGlobal(record);
  hook(SchedulerStep::AfterPublishPush, 0);
  // Sequentially consistent: the other half of the publish pairing. A count driven below zero by
  // a defect still leads to a wake rather than to silence.
  if (awake_.load(std::memory_order_seq_cst) <= 0) {
    wakeAsleepWorker();
  }
}

TaskRecord* TaskScheduler::takeGlobal() {
  // A hint that skips the mutex for an empty queue; the take itself is under the mutex.
  if (globalQueued_.load(std::memory_order_relaxed) == 0) {
    return nullptr;
  }
  std::unique_lock lock(globalMutex_);
  TaskRecord* record = globalHead_;
  if (record == nullptr) {
    return nullptr;
  }
  globalHead_ = record->next();
  if (globalHead_ == nullptr) {
    globalTail_ = nullptr;
  }
  record->setNext(nullptr);
  globalQueued_.fetch_sub(1, std::memory_order_seq_cst);
  return record;
}

TaskRecord* TaskScheduler::takeOutside(Worker& worker) {
  // The outside sources, in a fixed cyclic order. The global queue is the only one a scheduler
  // without adoptable slots has.
  constexpr std::size_t kSources = 1;
  for (std::size_t i = 0; i < kSources; ++i) {
    const std::size_t source = (worker.outsideCursor + i) % kSources;
    if (TaskRecord* record = takeGlobal()) {
      worker.outsideCursor = (source + 1) % kSources;
      return record;
    }
  }
  return nullptr;
}

TaskRecord* TaskScheduler::stealFromWorkers(Worker& worker) {
  const std::size_t others = workers_.size() - 1;
  if (others == 0) {
    return nullptr;
  }
  const std::size_t offset = ReduceRange(XorShift32(worker.stealRng), others);
  for (std::size_t k = 0; k < others; ++k) {
    const std::size_t victim = StealVictim(worker.index, workers_.size(), offset, k);
    const StealResult<TaskRecord*> result = workers_[victim]->deque.steal();
    if (result.status == StealStatus::Taken) {
      return result.item;
    }
  }
  return nullptr;
}

TaskRecord* TaskScheduler::findTask(Worker& worker) {
  if (worker.localRunsSinceOutside >= kOutsideTakeInterval) {
    worker.localRunsSinceOutside = 0;
    if (TaskRecord* record = takeOutside(worker)) {
      return record;
    }
  }
  if (const std::optional<TaskRecord*> local = worker.deque.take()) {
    ++worker.localRunsSinceOutside;
    return *local;
  }
  // A steal round: every outside source first, then every other worker once.
  if (TaskRecord* record = takeOutside(worker)) {
    return record;
  }
  return stealFromWorkers(worker);
}

void TaskScheduler::runTask(Worker& worker, TaskRecord* record) {
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
  // 1. Drain the deque into the global queue, through the owner's take.
  while (const std::optional<TaskRecord*> record = worker.deque.take()) {
    publishToGlobalQueue(*record);
  }
  hook(SchedulerStep::AfterDrain, worker.index);

  // 2. Set the waiting bit; the token is the word it returns, with the bit.
  const Parker<>::Word token = worker.parker.prepareWait();

  // 3. Store asleep, then decrement the awake count; both sequentially consistent.
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

void TaskScheduler::workerMain(Worker& worker) {
  tCurrentScheduler = this;
  tCurrentWorker = &worker;
  std::size_t emptyRounds = 0;
  while (!stop_.load(std::memory_order_seq_cst)) {
    if (TaskRecord* record = findTask(worker)) {
      emptyRounds = 0;
      runTask(worker, record);
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
