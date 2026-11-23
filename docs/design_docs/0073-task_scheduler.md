# Design: Work-Stealing Task Scheduler

**Status:** Draft
**Author:** Claude Fable 5.2
**Drafted by:** Claude Opus 5.5
**Requested by:** Jeff McGlynn
**Created:** 2026-11-23
**Tracking:** [#2660](https://github.com/jwmcglynn/donner/issues/2660) (first client)

## Summary

Donner has no shared way to run work in parallel. The editor renders on one dedicated worker
thread, the document access coordinator serializes DOM access, and the one design that plans a
thread pool, [0061-multithreaded_rendering](0061-multithreaded_rendering.md), proposes a
renderer-owned pool for filter row bands that does not exist yet. This design adds a general task
scheduler to the base library, `donner/base/tasks/`, for every parallel client:

- Work stealing: one deque per worker, idle workers steal from busy ones, and a thread waiting on
  its children runs them instead of blocking.
- Built for small work: work below a caller-chosen grain runs inline with no scheduling cost, and
  parallel loops split adaptively, only as far as idle workers ask for.
- Structured: every task belongs to a scope that its creator joins, so nothing outlives the call
  that started it.
- Coroutines: a coroutine can `co_await` a task, a scope or the scheduler itself to continue on a
  worker.
- Same code path at any worker count: zero workers runs everything inline on the caller, and a
  seeded deterministic mode runs tasks on one thread in a reproducible order so ordering-dependent
  failures reproduce.

Its first client is parallel style traversal, Part D of
[0072-css_cascade](0072-css_cascade.md). Filter row bands, layer
preparation, stylesheet parsing and resource decoding are named as later clients so the API is
not shaped by one user.

## Goals

- **Small-task overhead.** Targets, not yet measured: spawning and running an empty task that
  the owner itself pops costs at most 50 ns, and a `Join` of two empty closures that is not stolen
  costs at most 100 ns, on the benchmark in [Performance](#performance) at `-c opt`. The
  work-first principle (below) is why these are plausible: the owner's fast path has no
  read-modify-write atomic.
- **Scaling.** A parallel loop over 1,000,000 units of about 100 ns each runs at least 3 times
  faster on 4 workers than inline, and a loop whose total work is below the grain runs inline with
  no task created (asserted by a counter, not a time).
- **Idle behavior.** Idle workers sleep after a bounded spin, and no submitted task waits on a
  sleeping pool (no lost wakeup): proven by a stress test with injected delays between the
  sleep decision and the sleep, observed red with the wake protocol deliberately broken.
- **Same results at every worker count.** Clients that write disjoint outputs get identical
  results at 0, 1 and N workers and in the seeded mode; the scheduler's own fuzzer checks this for
  random fork-join programs.
- **Web.** In the threaded editor Wasm build the scheduler runs its workers on pre-created
  pthreads; in the non-threaded Wasm builds it compiles to the inline path with no thread code
  linked. Size growth is measured against the enforced ceilings; targets, not yet measured: at most
  2 KiB in the non-threaded renderer module and at most 24 KiB in the editor module. In the editor
  build, whose linear memory is fixed, the scheduler's memory (worker stacks, deques, task slots,
  frame pool) has a measured budget that fits the measured peak heap with headroom.
- **Race coverage.** The scheduler's tests and its clients' parallel tests run under
  ThreadSanitizer in a target reachable from `bazel test //...`.

## Non-Goals

- No detached threads, fire-and-forget tasks or tasks that outlive their scope.
- No priorities, deadlines, timers or IO reactor; this is a compute scheduler.
- No exceptions: the repository builds with `-fno-exceptions`. Errors are values.
- No preemption or fairness guarantees between unrelated clients; clients bound their own work.
- No change to the document access model: tasks never acquire document guards.
- No GPU command scheduling; Geode keeps its own submission model.
- No client designs here beyond naming their fit; each client is designed in its own document.

## Next Steps

- Land Milestone S1 (deque, workers, scopes, join, inline mode, sleep and wake) with its stress
  tests, then S4's ThreadSanitizer target so every later milestone runs under it.

## Implementation Plan

Estimates are engineering days, including tests and review fixes.

- [ ] Milestone S1: Core scheduler (7 days)
  - [ ] Chase-Lev deque with the memory orders of Le et al. and old-array reclamation,
        per-worker deques, one intrusive global queue, random-victim stealing, help-while-waiting
        joins with scope watermarks, parkers, drain before sleep, the last-awake sleep rule and
        its publish protocol, the stack-floor assertion, `trySpawn`, cross-thread slot return,
        `TaskScope` and `Join`, zero-worker inline mode.
  - Done when: deque linearization stress tests, scope and join tests at 0, 1 and 4 workers, the
    liveness tests listed under [Internals](#internals), and the lost-wakeup injected-delay test
    (red with the protocol broken) are green. Each liveness test must be observed red with the
    last-awake rule disabled, through a test hook, before it counts.
- [ ] Milestone S2: Parallel loops and determinism (3 days). After S1.
  - [ ] `ParallelFor` with grain and adaptive splitting; seeded deterministic mode; test hooks.
  - Done when: the inline-below-grain counter test, the seeded-mode replay test and the scaling
    benchmark target exist and pass.
- [ ] Milestone S3: Coroutines (3 days). After S1.
  - [ ] `Task<T>`, `TaskScheduler::schedule()` and scope-join awaitables, frame allocation from the
        scheduler's pool.
  - Done when: coroutine tests at 0, 1 and 4 workers, including resumption from a non-worker
    thread and nested awaits, pass under ThreadSanitizer.
- [ ] Milestone S4: ThreadSanitizer target and fuzzer (3 days). After S1.
  - [ ] A ThreadSanitizer test variant reachable from `//...` through a configuration transition,
        and `//donner/base/tasks:task_scheduler_fuzzer`.
  - Done when: the variant runs the scheduler tests in normal CI and the fuzzer is green.
- [ ] Milestone S5: WebAssembly (4 days). After S1.
  - [ ] Inline-only build for Wasm without pthreads; pool sizing, worker stack size, start-up and
        isolation probe for the editor build; size and memory measurements.
  - Done when: both Wasm builds pass their size tests, an editor smoke test records the worker
    count it obtained and the peak heap with the scheduler running, the fixed memory still has
    the headroom its build contract requires, and a test shows scheduler workers' sleeps take the
    pass-through branch of the editor's wrapped futex wait.
- [ ] Milestone S6: Benchmarks (1.5 days). After S2 and S3.
  - Done when: `//donner/benchmarks:task_scheduler_bench` reports spawn, join, wake latency and
    scaling, and the overhead goals are met.

Total committed estimate: 21.5 engineering days. Part D of
[0072-css_cascade](0072-css_cascade.md) needs S1 to S3, and S4 for its
race coverage.

## Background

### Donner Today

- **Threads.** `donner/editor/AsyncRenderer` owns one worker `std::thread` that owns the renderer
  for its lifetime. `donner/svg/DocumentState.h` implements the document access coordinator with a
  mutex and condition variables, and `ThreadingMode::ConcurrentDom` lets several threads use one
  document through `DocumentReadAccess` and `DocumentWriteAccess`
  ([0033-multithreading_and_dom_lifetime](0033-multithreading_and_dom_lifetime.md)). The Geode
  browser device and its Emscripten bridge synchronize with the application pthread. Nothing in
  `donner/base/` creates threads, and there is no pool.
- **Planned pool.** [0061-multithreaded_rendering](0061-multithreaded_rendering.md) (Draft)
  specifies one renderer-owned pool, `RenderWorkerPool`, for filter row bands, with joinable units,
  no detached threads, a zero-worker inline mode on the same code path and a ThreadSanitizer lane,
  and rejects task-graph or coroutine scheduling for the filter graph as premature. It is not
  implemented. This scheduler meets those contracts and lets 0061 build its band submission on it
  instead of a pool of its own.
- **Coroutines.** The build is C++20 (`--config=c++20` in `.bazelrc`). Coroutines are already in
  production: `ElementTraversalGenerator` in `donner/base/element/ElementTraversalGenerators.h` is a
  `co_yield` generator used by selector matching, so coroutine support is exercised by every
  toolchain the repository builds with, including libc++, libstdc++ host builds and Emscripten.
- **Errors.** `.bazelrc` passes `-fno-exceptions` to every build; errors are explicit values
  (`ParseResult`, diagnostics).
- **Sanitizers.** `.bazelrc` defines `--config=tsan`, but the sanitizer workflows run only the
  AddressSanitizer and UndefinedBehaviorSanitizer lanes, so nothing threaded runs under
  ThreadSanitizer in CI today. Repository rules keep one native Bazel invocation in CI and express
  variants as target-level transitions, and correctness tests carry no short deadlines; hang guards
  only.
- **WebAssembly.** The plain `wasm` configuration does not pass `-pthread`. The editor's Wasm build
  (`donner/editor/wasm/BUILD.bazel`) passes `-pthread`, `-sPROXY_TO_PTHREAD`,
  `-sOFFSCREENCANVASES_TO_PTHREAD=#canvas` and `-sPTHREAD_POOL_SIZE=2`, and
  `donner/editor/wasm/enable-threads.js` installs `Cross-Origin-Opener-Policy: same-origin` and
  `Cross-Origin-Embedder-Policy: require-corp` so the page is cross-origin isolated. The root
  `AGENTS.md` requires the application pthread to keep servicing GPU dispatch while it is blocked,
  through the editor's owner-only wait adapter. Wasm module and JavaScript size ceilings are
  enforced by `//donner/svg/renderer/wasm:graph_wasm_size_tests` and `//tools/ci:wasm_size_tests`.

### Prior Work

- **Work-first principle.** Cilk-5's scheduler is built on "Minimize the scheduling overhead
  borne by the work of a computation. Specifically, move overheads out of the work and onto the
  critical path", which brought a spawn to "between 2 and 6 times the cost of a C function call"
  ([Frigo, Leiserson and Randall, PLDI 1998](https://people.cs.umass.edu/~emery/classes/cmpsci691w-spring2006/papers/cilk5.pdf)).
  `Join` and `spawn` here follow it: the cost of parallelism is paid by thieves, not by the owner.
- **Work-stealing deque.** Chase and Lev's deque
  ([SPAA 2005](https://www.dre.vanderbilt.edu/%7Eschmidt/PDF/work-stealing-dequeue.pdf)) is "the state-of-the-art algorithm for the
  work-stealing deque": each worker pushes and pops at the bottom of its own growable array,
  thieves take from the top, and with sequentially consistent atomics it "involves only one atomic
  compare-and-swap (CAS) per steal, no CAS on push, and no CAS on take except when the deque has
  exactly only one element left". Le, Pop, Cohen and Zappa Nardelli give a C11 version with
  relaxed and release-acquire atomics plus a sequentially consistent fence in `take` and in
  `steal`, prove the algorithm correct on the ARM and POWER memory models, and show that a naive
  sequentially consistent translation inserts redundant barriers
  ([Correct and Efficient Work-Stealing for Weak Memory Models, PPoPP 2013](https://fzn.fr/readings/ppopp13.pdf)).
  Donner runs on Arm (Apple silicon) as well as x86-64, so the weak-memory proof matters. The
  paper's C11 listing uses unsigned indices, which underflow when `take` runs on an empty deque in
  its initial state
  ([Wingo, 2022](https://wingolog.org/archives/2022/10/03/on-correct-and-efficient-work-stealing-for-weak-memory-models));
  the original algorithm uses signed indices, and so does this design.
- **Sleeping without lost wakeups.** Rayon's workers search for work for several rounds, become
  "sleepy", search once more, and sleep only if a jobs event counter is unchanged; a thread that
  posts work toggles the counter and wakes sleepers, and a final check of the injection queue
  closes the remaining race for work submitted from outside the pool
  ([rayon-core sleep README](https://github.com/rayon-rs/rayon/blob/main/rayon-core/src/sleep/README.md)).
- **Join and adaptive splitting.** Rayon's `join` pushes the second closure to the local deque,
  runs the first, then runs the second itself unless it was stolen, helping with other work while
  it waits ([join/mod.rs](https://github.com/rayon-rs/rayon/blob/main/rayon-core/src/join/mod.rs)).
  Its parallel iterators use thief-splitting: "starts by splitting into enough jobs for every
  worker thread, and then resets itself whenever a job is actually stolen into a different thread"
  ([plumbing/mod.rs](https://github.com/rayon-rs/rayon/blob/main/src/iter/plumbing/mod.rs)).
- **Grain size.** oneTBB's rule of thumb is that a chunk "should take at least 100,000 clock
  cycles to execute"
  ([Controlling Chunking](https://uxlfoundation.github.io/oneTBB/main/tbb_userguide/Controlling_Chunking_os.html)),
  and that a loop "needs to take at least a million clock cycles to make it worth using
  parallel_for"
  ([Automatic Chunking](https://uxlfoundation.github.io/oneTBB/main/tbb_userguide/Automatic_Chunking.html)).
  A successful steal costs hundreds of cycles, rising with core count, in a measured TBB study
  ([Contreras and Martonosi, IISWC 2008](https://mrmgroup.cs.princeton.edu/papers/contreras-iiswc2008.pdf)).
  oneTBB's `task_group` is the structured fork-join this design's `TaskScope` mirrors
  ([task_group](https://oneapi-spec.uxlfoundation.org/specifications/oneapi/latest/elements/onetbb/source/task_scheduler/task_group/task_group_cls)).
- **Wake-up protocols.** Taskflow keeps at least one worker stealing while any worker is active
  and puts the rest to sleep on a two-phase event count derived from Eigen's
  ([Taskflow, TPDS](https://arxiv.org/pdf/2004.10908),
  [nonblocking_notifier.hpp](https://github.com/taskflow/taskflow/blob/master/taskflow/core/nonblocking_notifier.hpp)).
- **Coroutines on a scheduler.** In C++26 `std::execution` a scheduler's `schedule()` "returns a
  sender that completes on the scheduler"
  ([P2300R10](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2300r10.html)), and
  `as_awaitable` lets a coroutine `co_await` a sender by starting it in `await_suspend` and
  resuming from its receiver ([exec.as.awaitable](https://eel.is/c++draft/exec.as.awaitable));
  libunifex's `task` reschedules onto its scheduler after each await
  ([task.hpp](https://github.com/facebookexperimental/libunifex/blob/main/include/unifex/task.hpp)).
  cppcoro's `co_await threadPool.schedule()` suspends and enqueues the coroutine, and its pool
  uses local queues, a global queue for remote work and stealing
  ([cppcoro](https://github.com/lewissbaker/cppcoro)). Returning a `coroutine_handle` from
  `await_suspend` (symmetric transfer) keeps chained resumptions in constant stack space
  ([Baker, 2020](https://lewissbaker.github.io/2020/05/11/understanding_symmetric_transfer)).
- **Context.** Chromium's task system prefers sequences to locks and threads: "Sequenced execution
  (on virtual threads) is strongly preferred to single-threaded execution"
  ([threading_and_tasks.md](https://chromium.googlesource.com/chromium/src/+/HEAD/docs/threading_and_tasks.md)).
- **Small work in a parallel style engine.** Stylo hands children to other workers only in chunks
  (`work_unit_max`, 16 nodes by default in Firefox) and keeps a configurable `local_queue_size` of
  work local, with per-thread style contexts
  ([parallel.rs](https://github.com/servo/stylo/blob/main/style/parallel.rs)).
- **Coroutines in WebAssembly.** Emscripten's coroutine support for JavaScript promises "uses LLVM
  coroutines and so, doesn't depend on either Asyncify or JSPI"
  ([PR #20420](https://github.com/emscripten-core/emscripten/pull/20420)), so C++20 coroutines are
  an ordinary compiler transform on the web as well.
- **Threads on the web.** See [WebAssembly](#webassembly).

## Proposed Architecture

### API

All types live in `namespace donner::tasks`, in `donner/base/tasks/` (a new `donner_cc_library`
target, so code that never schedules work does not link it).

```cpp
struct TaskSchedulerOptions {
  std::size_t workerCount = 0;               // 0: run everything inline on the caller.
  std::optional<std::uint64_t> seed;         // Seeded deterministic mode (tests).
  std::size_t maxQueuedTasks = 64 * 1024;    // Bound on queued tasks across all queues.
  std::size_t helpMarginBytes = 128 * 1024;  // Stack kept free when stealing while waiting.
};

class TaskScheduler {
public:
  explicit TaskScheduler(TaskSchedulerOptions options);
  ~TaskScheduler();                          // Joins every worker; asserts no scope is live.
  std::size_t workerCount() const;           // Workers actually started (may be fewer).
  ScheduleAwaitable schedule();              // co_await: continue on a worker.
};

class TaskScope {                            // Structured fork-join; the destructor joins.
public:
  explicit TaskScope(TaskScheduler& scheduler);
  ~TaskScope();
  template <typename F> void spawn(F&& work);   // From the creator or any task of this scope.
  template <typename F> bool trySpawn(F&& work); // False at the queue bound; caller keeps work.
  void wait();                               // Runs queued work while waiting.
  JoinAwaitable join();                      // co_await: resume when every spawn finished.
  void requestStop();                        // Cooperative cancellation.
  bool stopRequested() const;
};

template <typename A, typename B> void Join(TaskScheduler& scheduler, A&& a, B&& b);
template <typename F>
void ParallelFor(TaskScheduler& scheduler, std::size_t begin, std::size_t end,
                 std::size_t grain, F&& body);  // body(std::size_t first, std::size_t last)

template <typename T> class Task;             // Lazily started coroutine; co_await yields T.
```

- **Join** pushes `b` to the worker's deque, runs `a` itself, then takes `b` back and runs it if
  no one stole it, as Rayon's `join` does; the work-first principle, so an unstolen join costs
  about two calls and a deque push and pop. If `b` was stolen, the caller helps (below) until it
  completes.
- **ParallelFor** never creates a task for a range below `grain`. Above it, it splits in halves
  only while the split half is likely to be stolen: a range is split again when it was stolen
  (adaptive splitting), so on an idle pool work spreads quickly and on a busy pool it stays in
  large pieces.
- **Coroutines.** `co_await scheduler.schedule()` enqueues the coroutine handle and suspends; a
  worker resumes it. With zero workers, or in seeded mode, `schedule()` is ready immediately and
  the coroutine continues inline on the calling thread, the same rule every other operation
  follows at zero workers. `co_await scope.join()` suspends until the scope's counter reaches zero
  (or does not suspend if it already has), and the last finishing task enqueues the awaiting
  coroutine's resumption. `Task<T>` is lazily started and
  resumes its awaiter by symmetric transfer, so chains of awaits do not grow the native stack. A
  coroutine's promise calls `std::abort()` from `unhandled_exception`, which cannot be reached in a
  `-fno-exceptions` build.
- **Errors and cancellation.** Work returns `void`; results and errors are written to
  caller-owned slots, or returned as `Task<ParseResult<T>>`. `requestStop()` sets a flag that
  queued tasks check before running and long tasks poll; stopped tasks still complete their scope
  accounting, so `wait()` always returns.

### Internals

- **Deques.** Each worker owns a Chase-Lev deque with signed 64-bit logical indices that only grow
  and never wrap or reset (the array slot is the index modulo the capacity), so watermarks stay
  comparable for the life of the scheduler: the owner pushes and pops at the bottom, thieves take
  from the top, and the last element is claimed by a compare-and-swap on the top index. The memory
  orders are the C11 ones of Le et al.'s Figure 1 (relaxed and release-acquire accesses,
  sequentially consistent fences in `take` and `steal`, sequentially consistent compare-and-swap),
  whose ARM counterpart they prove correct, not a port of the original pseudo-code. The array grows
  by doubling up to the per-worker share of `maxQueuedTasks`; past it, `spawn` runs the task inline
  and `trySpawn` returns false, which keeps the program correct and memory bounded. A thief can
  still be reading the old array after the owner grows it, so the old array is kept, not freed: old
  arrays stay on the deque's list until the scheduler destructor, as Chase and Lev's GC-free variant
  keeps buffers in a pool. Because arrays double, the kept ones together are smaller than the live
  one.
- **Global queue.** Besides the per-worker deques there is one global queue: a multi-producer,
  multi-consumer list linked through the task records themselves (and, for coroutine resumptions,
  through the awaiter in the suspended frame), so pushing needs no allocation and the queue cannot
  overflow; its length is bounded by the task records, which `maxQueuedTasks` bounds. It receives
  spawns from threads that are not workers, coroutine resumptions scheduled from outside a worker,
  and everything a worker drains before it sleeps. There are no per-scope queues: a queue that
  belongs to a scope would expose the scope's memory to idle workers looking for work while the
  scope's creator completes and destroys it.
- **Where spawned tasks go.** A spawn from a worker goes onto that worker's own deque, whether it
  comes from a scope's creator or from one of the scope's tasks (a flat spawn), so a tree traversal
  gets ordinary deque-to-deque work stealing. A spawn from a thread that is not a worker goes to
  the global queue. Every task record carries what a finisher needs: the address of its scope's
  counter and the scope creator's parker, both copied into the record at spawn time.
- **Watermarks.** A `TaskScope` that its creator waits on synchronously (`wait()`, the destructor,
  `Join`) records the creator worker's deque bottom index when it is constructed. Such scopes nest
  last in, first out on one worker, because each is joined before its creator returns, so
  everything the creator spawns from its own stack lies above the watermark. A scope joined with
  `co_await scope.join()` has no watermark: its creator is a coroutine whose frame leaves the
  worker's stack when it suspends, so the last-in, first-out argument does not apply. Its spawns
  from the coroutine go onto whichever worker's deque the coroutine is running on, as ordinary
  entries, and are run like any other task.
- **Waiting.** A creator that waits synchronously applies, in order:

  1. If its deque holds an entry above the scope's watermark, pop and run it.
  2. If the scope's counter is zero, return.
  3. Otherwise help: take a task from the global queue or steal one from another worker's deque,
     while the stack has more than the margin left. When it is past the margin, or finds nothing,
     it tries to sleep with the protocol below, which keeps it awake if it is the last awake
     worker and work is queued; it then takes work from the global queue even past the margin.

  A worker never runs a deque entry below its innermost live watermark; those entries leave only
  by being stolen from the top or by the drain step before a sleep. An idle worker runs its own
  deque, then the global queue, then steals.
- **Sleeping.** Every worker sleeps on its own parker (below), whether it is idle or a waiting
  creator. The scheduler keeps an `awake` count of workers not asleep and a per-worker state word:
  awake, asleep shallow (asleep while within its margin, for example in its top-level idle loop) or
  asleep deep (asleep past its margin). Every transition from either asleep state to awake is one
  compare-and-swap on that word, and exactly the thread whose swap succeeds increments `awake`; a
  thread whose swap fails does not increment, because the thread that won already counted the
  worker. Every waker changes and notifies the worker's parker whether or not its swap succeeded,
  which at worst causes a spurious wake. This rule covers every wake: a sleeper waking itself in
  step 4, a publisher, a finisher waking an asleep creator, the hand-off below, and shutdown. To
  sleep, a worker:

  1. drains its own deque into the global queue, removing entries with the owner's Chase-Lev
     `take` (including its compare-and-swap on the last element), never by reading the array;
  2. reads its parker value;
  3. stores asleep shallow or asleep deep in its state word and decrements `awake`, then issues a
     sequentially consistent fence;
  4. re-checks: every worker checks the stop flag, a waiting creator re-checks rules 1 and 2, and
     every worker checks the global queue. An idle worker that sees the stop flag wakes itself
     with the swap rule and exits. If its scope completed, or if the global queue is non-empty and
     it is not past its margin, it
     wakes itself (the swap on its own state word, incrementing `awake` only if the swap
     succeeds) and goes back to work. If the global queue is non-empty, it is past its margin and
     it was the last awake worker (the decrement returned 1 or less, so that a transiently low
     count errs toward staying awake), it first hands the work off: it
     looks for a worker asleep shallow and wakes it with the swap rule. If the swap succeeds, the
     woken worker is counted awake and will take the work from its shallow stack; if it fails, the
     worker was already woken and counted by another thread. Either way an awake worker exists
     that will re-check the global queue before it next sleeps, and this worker continues to step
     5. Only when no worker is asleep shallow does it wake itself and run the work past its
     margin;
  5. otherwise waits on its parker for a change from the value read in step 2. Whenever that wait
     returns, for any reason (a change, a change that happened before the wait began, or a
     spurious return), the worker first wakes itself with the swap on its own state word,
     incrementing `awake` only if the swap succeeds, and only then does anything else: a waiting
     creator re-checks its scope and rules 1 and 2, an idle worker exits if the stop flag is set
     and otherwise looks for work, and either one that finds nothing sleeps again from step 1.

  Steps 3 to 5 touch only the global queue's emptiness, state words and parkers; the worker runs,
  pops and steals nothing while its state is asleep. So a worker executing anything other than
  those steps is counted awake. Without the self-wake on leaving step 5 that fails: a waker whose
  swap failed because the worker had not yet reached step 3 still changes the parker, the worker
  then stores asleep and decrements, its wait returns at once, and it would run on counted asleep
  and decrement again at its next sleep.

  `awake` is a signed count. The transitions that change either side of it are exactly these:
  a worker storing an asleep value in its own state word in step 3, followed by its one decrement;
  and a successful swap from asleep to awake on some worker's word (the worker itself in step 4
  or on leaving step 5, a publisher, a finisher, the hand-off, or shutdown), followed by one
  increment by the thread that won the swap. A failed swap changes neither side. Workers start
  awake with `awake` equal to the worker count. So the protocol's count invariant is: `awake`
  equals the number of workers whose state word is awake, except for transitions in flight, each
  of which is one store or swap followed by its single matching decrement or increment. That
  clause allows a transiently negative count: a publisher can win the swap on a worker and be
  delayed before its increment while the worker, woken by another waker's parker change, fails its
  own swap, finds nothing and decrements again. So the invariant is asserted only at quiescence:
  the destructor, after joining every worker, asserts that `awake` equals the number of workers
  whose state word is awake, and the scheduler tests assert the same at their quiescent points. No
  assertion runs on individual decrements.

  A thread that publishes to the global queue pushes, issues a sequentially consistent fence, and
  reads `awake`. If no worker is awake, tested as `awake <= 0` so that a count driven below zero
  by a defect still reads as no worker awake and leads to a wake rather than to silence, it picks
  an asleep worker, switches that worker's state to awake with a compare-and-swap, increments
  `awake` if its swap succeeded, and changes and notifies that worker's parker. It
  may also wake one worker when some are asleep and none is searching, for parallelism. Because the
  sleeper's step 3 and the publisher's push are each followed by a fence before the other's read,
  at least one of them sees the other: either the sleeper sees the queued task in step 4, or the
  publisher sees `awake` at zero and wakes someone. A woken worker that is past its margin may go
  back to sleep only under the same steps, so the last awake worker never leaves queued work.
  The thread whose swap succeeds increments `awake` before it changes the woken worker's parker,
  so the count is never low while a woken worker is still starting, and never counts one wake
  twice. The hand-off keeps the invariant: the deep worker's own decrement happened before it
  wakes the shallow worker, and the shallow worker is counted awake by the successful swap before
  the deep worker waits, so from then on an awake worker exists for the queued work; in the short
  window between the two, a concurrent publisher reads `awake` as zero and wakes another worker,
  which only adds parallelism. If the shallow worker was waking itself at the same moment, exactly
  one of the two swaps succeeds and only that one counts it.
  On native platforms a parker wait is `std::atomic::wait`; in the editor Wasm build that reaches
  `emscripten_futex_wait`, which the build wraps (`-Wl,--wrap=emscripten_futex_wait`) for the
  GPU-owning application pthread. The wrapper passes every other thread straight to the real
  futex wait, so scheduler workers sleep through `Atomics.wait`, which is permitted on workers;
  Milestone S5 tests that branch.
- **The invariant and liveness.** The single invariant is: whenever a task is queued anywhere,
  at least one worker is awake. Deque entries exist only on awake workers, because a worker drains
  its deque before it sleeps and a worker that pushes is awake. Global-queue entries are covered
  by the fence pairing above. An awake worker always makes progress: it runs a task, pops one by
  rule 1, helps, or tries to sleep; it cannot sleep while it is the last awake worker and work is
  queued. So queued work always has a runner, and the margin decides only which worker runs it.

  Termination, once, for every kind of task: a program is a finite set of tasks. A task's own code
  runs a finite number of steps except where it waits; it waits only through the scheduler (a
  scope wait, `Join`, or a coroutine suspension), and every wait is on tasks it or its
  descendants spawned, so the waits-for relation is acyclic. Take a wait with no unfinished waiting
  task beneath it: each unfinished task it waits for is either running without waiting, so it
  finishes, or queued, so by the invariant some awake worker eventually runs it. When the last one
  finishes, the finisher changes the creator's parker (and, for a coroutine scope, enqueues the
  resumption), so the wait completes. By induction on the waits-for order every wait completes,
  whether the scope is waited on synchronously or by a coroutine, whether its tasks were spawned by
  its creator or flatly by its tasks, and whatever every worker's margin is.
- **Skipping the wake on a push.** A worker pushing onto its own deque may skip the publish
  protocol, because it is awake and runs, loses to a thief, or drains that entry before it can
  sleep, so the invariant holds and a missed wake costs parallelism only. Pushes into the global
  queue (spawns from non-workers, drains, coroutine resumptions from outside a worker) always take
  the fenced publish protocol.
- **Parkers and scope lifetime.** A scope lives in its creator's frame and is destroyed as soon as
  its creator sees it complete, so no thread may touch the scope after the change that lets the
  creator return. Every worker has a parker, an atomic epoch word, for the scheduler's lifetime;
  each other thread that creates a scope gets one from scheduler-owned stable storage. The rules:

  - A finishing task reads the creator's parker pointer from its task record, then decrements the
    scope's counter with release ordering. That decrement is its last access to the scope. If it
    reached zero, the finisher changes and notifies the parker (and, if the creator is an asleep
    worker, first swaps its state to awake, incrementing `awake` only if that swap succeeds),
    touching only the parker and the scheduler's own state.
  - `co_await scope.join()` registers its coroutine handle in the scope and sets a flag in the
    counter word with a compare-and-swap loop. If the loop observes a count of zero,
    `await_suspend` returns false and the coroutine continues without suspending. Otherwise the
    finisher whose decrement reaches zero sees the flag in the value it replaced and enqueues the
    resumption (onto its own deque if it is a worker, otherwise into the global queue), rather
    than resuming inline, so a resumption never nests on the finisher's stack. The scope is not
    destroyed in between, because its creator is that suspended coroutine.
  - A creator about to sleep reads its parker value, re-checks, and waits on that value, as in
    the sleep steps. A notify without a change would let `std::atomic::wait` block again, so every
    wake is a change of the parker; a creator woken by an event for an outer scope re-checks its
    current scope and sleeps again, which is a spurious wake and nothing more.
  - Parker storage for threads that are not workers lives in a `ParkerPool`, a separate object
    the scheduler creates and shares by `std::shared_ptr`: it holds the slots (stable addresses,
    allocated in fixed blocks) and a mutex-protected free list. Each such thread keeps a small
    thread-local map from a scheduler's unique generation number (not its address, which could be
    reused) to an entry holding a `std::shared_ptr` to that scheduler's pool and its slot index.
    The scheduler holds one more reference and, when it is destroyed, marks the pool closed after
    joining every worker. When a thread exits, its thread-local destructor returns each slot to
    the pool its entry references and drops the reference, so the pool outlives every thread that
    holds one of its slots, and a scheduler destroyed first (one local to `main()`, destroyed
    before the main thread's thread-local cleanup, for example) leaves nothing dangling. A lookup
    that meets an entry whose pool is closed drops it, so entries for destroyed schedulers do not
    accumulate. A scope that a coroutine created on that thread can still be live after the thread
    exits, if the coroutine was resumed elsewhere, and a finisher that read the parker before the
    slot was reused can still change it. Either way the slot's next owner gets one spurious wake:
    slot storage is stable while any reference to the pool lives, and every wake is followed by a
    re-check, so that is harmless. Storage is therefore bounded by the largest number of
    non-worker threads that hold the scheduler's scopes at the same time, and is freed when the
    scheduler and every thread that took a slot from it have released the pool.

  The injected-delay tests cover a delay between a creator's re-check and its parker wait, between
  a push to the global queue and the publisher's read of `awake`, between a sleeper's decrement
  and its re-check, and between the final decrement and the parker change with the creator
  returning and destroying the scope at once; that last test runs under AddressSanitizer and
  ThreadSanitizer and must be observed red with the wake word placed inside the scope before it
  counts.
- **Liveness tests.** Each is a test in Milestone S1, run at 1, 2 and 4 workers:

  - One worker recursing through nested synchronous scopes, the next child still on its own deque
    above the watermark: rule 1 runs the child; recursion depth is the program's own nesting.
  - Every worker past its margin with work queued: exactly one worker, the last awake, runs it,
    asserted by a counter of tasks run past the margin.
  - The last awake worker past its margin with work queued and another worker asleep shallow: the
    shallow worker runs the work and the counter of tasks run past the margin stays at zero.
  - A sleeper's self-wake racing a publisher's wake of the same worker: `awake` ends equal to the
    number of workers not asleep. The injected-delay test delays the sleeper between its step 4
    decision and its swap, and must be observed red with an unconditional increment (`awake`
    stays one too high, and a later schedule with every worker asleep leaves a task unrun).
  - A finisher's swap failing just before the creator's step 3: a worker waits on an inner scope
    inside an outer one, and the injected delay holds it between steps 2 and 3 while the outer
    scope's last task finishes, so the finisher sees it awake, its swap fails and it changes the
    parker. At quiescence `awake` must equal the number of workers whose state is awake, and a
    following schedule with every worker past its margin must complete. Must be observed red with
    the self-wake swap after the parker wait removed (`awake` ends one low).
  - Shutdown racing an idle worker's sleep: the injected delay holds an idle worker between steps
    1 and 2 while the destructor sets the stop flag, so the destructor's swap fails and its parker
    change lands before the worker reads its parker. The destructor's join must return. Must be
    observed red with the stop-flag re-check in step 4 removed (the worker waits forever on an
    unchanged parker).
  - Two workers past the margin, each holding the other's scope's flat task below its own
    watermark: both drain into the global queue and the last awake worker runs both tasks.
  - A coroutine-joined scope whose task is drained into the global queue while every worker is
    past its margin and asleep except one, and whose creator's parker belongs to a thread waiting
    on something else (the reported schedule): the last awake worker runs the task, the finisher
    enqueues the resumption, and the coroutine and every waiter blocked on it complete.
  - `co_await scope.join()` on a scope whose count is already zero does not suspend.
  - A scheduler destroyed before a non-worker thread that used it exits: the thread waits on a
    scope, the scheduler is destroyed, then the thread exits; and a small test binary whose
    `main()` creates a scheduler, waits on a scope from the main thread and destroys the
    scheduler before returning, so the main thread's thread-local cleanup runs after it.
    Both run under AddressSanitizer with no use-after-free, and must be observed red with the
    thread-local entry holding a raw pointer to the scheduler's free list.
  - A stolen task that waits; `Join` stolen and not stolen; a spawn at the queue bound
    (`spawn` inline, `trySpawn` returning false); a non-worker scope with zero idle workers.
- **Blocking inside tasks.** A worker that blocks inside a task on anything other than the
  scheduler (a condition variable, an `std::atomic::wait` in client code, a lock held by a thread
  that waits on scheduler work) is counted awake while it holds queued work it cannot run, so the
  invariant does not give that work a runner. The rule is therefore that tasks may block only
  through the scheduler: `TaskScope::wait()`, the join and schedule awaitables, and `Join`. A short
  critical section under a mutex is allowed when no holder of that mutex ever waits on scheduler
  work while holding it. The rule is documented in the API, and debug builds check it where Donner
  itself blocks: the per-thread in-task flag that the document access coordinator already checks
  is also checked by Donner's own waiting helpers, which assert when called from a task.
- **Worker stacks.** Workers are created with an explicit stack size (512 KiB by default, an
  option) instead of the platform default, which differs between platforms and toolchains. The
  margin (`TaskSchedulerOptions::helpMarginBytes`, 128 KiB by default) is a heuristic that decides
  which worker helps; it is not part of correctness. Because the last awake worker runs queued
  work even past its margin, native stack depth is bounded by policy instead. Each client declares
  its tasks' native stack bound and whether its tasks wait (with the declaration below for a client
  whose tasks wait); a task that waits can, through rule 1
  and helping, nest further tasks on the same stack, bounded by the program's own fork-join
  structure, as in other work-stealing runtimes. A hard limit turns exhaustion into a named
  failure: before running any task, a worker compares the stack pointer with its stack floor plus
  a reserve and fails with a release assertion naming the scheduler, instead of overflowing the
  stack. The style traversal of 0072 Part D declares one task frame and tasks that never wait
  (one flat scope, an explicit heap stack for the inline part of a subtree, `trySpawn` with the
  heap stack as the fallback), so each help by a style task adds one frame (0072 Milestone D3
  measures it at the parser's nesting limit).

  The hand-off in the sleep steps keeps most past-margin work off deep stacks, but not all of it.
  When every other worker is asleep past its margin, the last awake worker runs queued work on
  its own deep stack, and if that work is a task that waits, it can run another queued task on top
  of it, and so on: depth is then bounded by how many waiting tasks are queued at the same time,
  not by any one program's nesting. If several clients whose tasks wait share one scheduler and
  every worker is past its margin, that depth can exceed a worker stack. The stack-floor check then
  stops the process with a named release assertion. That is a clean, diagnosable crash, not a
  recovery. Style alone cannot reach it, because style tasks never wait.

  The rule for clients whose tasks wait: each such client either declares the maximum number of
  its waiting tasks that can be queued or running at once together with their stack bound, or
  gets a scheduler of its own. Sharing a scheduler without one of the two is not allowed. For
  clients that declare, the scheduler's owner checks, before letting them share, that the worker
  stack size less the reserve exceeds the sum over those clients of maximum count times stack
  bound, plus the largest stack bound of any client whose tasks do not wait; that is a sum of
  products and computable. A declaration that is wrong is not caught by the check, and the
  stack-floor assertion is then still what stops the process: a named, clean crash, not a
  recovery.
- **Non-worker waits.** A thread that is not a worker (the render worker holding document access,
  for example) is not counted in `awake` and never runs tasks while workers exist: its spawns go
  to the global queue with the publish protocol, and when it waits it re-checks its scope's counter
  and sleeps on its parker, which the finisher of its scope's last task changes. It therefore
  never runs another client's task while it holds a guard, and needs no way to tell its own tasks
  apart in the global queue. With zero workers, every spawn runs inline on the spawning thread as
  before.
- **Task storage.** A spawned closure is stored in a fixed-size slot (64 bytes inline, larger
  closures in a per-worker free list of blocks) so small tasks never call `malloc`. A slot is freed
  by whichever thread ran the task: a thief returns it to the owning worker's lock-free return
  stack (multi-producer, single-consumer), and the owner moves returned slots to its local free
  list when that list runs empty. Coroutine frames allocate through a per-scheduler frame pool via
  the promise's `operator new` and are returned the same way. The pool keeps at most a configured
  number of free bytes; past that bound, freed frames go back to the heap, and new frames come
  from the heap when the pool is empty, so the bound limits retained memory, not live
  coroutines.
- **Seeded mode.** With `seed` set, the scheduler runs on the calling thread only and, whenever
  more than one task is runnable, picks the next one with a seeded generator. It reproduces task
  orders, not steals: the deque's compare-and-swap race and real interleavings never happen in
  this mode, and are covered by the stress tests at real worker counts and by ThreadSanitizer.
- **Shutdown.** The destructor asserts no scope is live (`UTILS_RELEASE_ASSERT`), sets the stop
  flag, issues a sequentially consistent fence, then wakes every worker with the same swap rule
  (the swap, then the parker change, whether or not the swap succeeded) and joins them. With no
  scope live, every worker is idle. A worker's step 3 store is followed by a fence before its
  step 4 read of the stop flag, and the destructor's flag store by a fence before its swap, so
  either the worker's re-check sees the flag and it exits, or the swap sees the worker asleep and
  succeeds; the parker change then follows the worker's step 3, and so its step 2 read, and the
  wait in step 5 returns. A worker that has not reached step 3 sees the flag at its next
  re-check. A coroutine suspended on `schedule()` after destruction
  began is a programming error caught by the same assertion.
- **Nested parallelism.** Scopes nest freely; nested work goes to the current worker's deque, so a
  nested parallel loop inside a task adds parallelism without adding threads.
- **Locks.** A task must not hold a lock across `wait()` or `co_await`: helping could run a task
  that takes the same lock. Debug builds record, per thread, whether it is inside a task, and the
  document access coordinator asserts that no document guard is acquired inside one. That
  assertion fires only when a guard is acquired; reading shared state without a guard is prevented
  by the clients' own rules (0072 Part D gives workers a `const Registry&` and handles taken before
  the fork), not by the scheduler.
- **Waits on threads with obligations.** The editor's application pthread must keep servicing GPU
  dispatch while blocked. Such a thread never calls `wait()`; it either awaits the scope from a
  coroutine or routes the wait through its existing owner-wait adapter, which the options accept as
  a wait hook. The style pass runs on the render worker, not on the application pthread.

### WebAssembly

- **Non-threaded builds** (the renderer's `wasm` configuration): the library compiles to the inline
  implementation (`__EMSCRIPTEN__` without `__EMSCRIPTEN_PTHREADS__`): no thread, atomic wait or
  `<thread>` code is linked, `workerCount()` is 0, and every client runs its serial path through
  the same calls.
- **Threaded editor build.** Starting a new Web Worker for `pthread_create` "requires returning
  the main event loop", and `PTHREAD_POOL_SIZE` "creates the Web Workers before main is called, so
  they can just be used when `pthread_create` is called"; `Atomics.wait` does not work on the
  browser main thread, and pthreads need the cross-origin isolation headers
  ([Emscripten pthreads](https://emscripten.org/docs/porting/pthreads.html)). With
  `PROXY_TO_PTHREAD` the application runs off the browser main thread, which only services
  proxied calls, so scheduler workers never run there and may block. The editor creates its
  scheduler at start-up from the application pthread and raises `PTHREAD_POOL_SIZE` from 2 by the
  scheduler's worker count (initially 2, an open question below), so workers start without a round
  trip. The page is already cross-origin isolated; if a worker fails to start (isolation lost, pool
  exhausted) the scheduler starts with the workers it got, down to zero, and clients take their
  inline path.
- **Fixed memory.** The editor build's linear memory is fixed (`ALLOW_MEMORY_GROWTH=0` with
  `INITIAL_MEMORY` sized to a measured peak), and growth is a crash, not a slow path. Everything the
  scheduler allocates comes out of that heap: worker stacks (2 workers at 512 KiB), deques (an
  initial 256 entries each, and `maxQueuedTasks` lowered to 4,096 in this build), task slot blocks
  and the frame pool's retained bytes. Milestone S5 measures the editor's peak heap with the
  scheduler running and keeps the build's headroom contract; if it does not fit, the worker count
  or the memory size changes together with a new measurement, never by enabling growth.
- **Asyncify.** The editor build enables `-sASYNCIFY` for Geode's synchronous WebGPU bridge with no
  `ASYNCIFY_ONLY`, `ASYNCIFY_REMOVE`, `ASYNCIFY_ADD` or `ASYNCIFY_IGNORE_INDIRECT` setting
  (`donner/editor/wasm/BUILD.bazel`). Asyncify's analysis cannot tell where an indirect call goes,
  so by default it treats one as a possible unwind and instruments it and its callers
  ([Emscripten Asyncify](https://emscripten.org/docs/porting/asyncify.html)). The scheduler calls
  task closures and coroutine resumptions indirectly, so its run loop, `wait()` and their callers
  will be instrumented and grow. Scheduler tasks must never reach a function that can suspend
  through Asyncify (the GPU bridge); a debug assertion in those entry points rejects calls from
  scheduler workers. Milestone S5 measures the instrumented size; if it is too large, the
  scheduler's dispatch functions go on an `ASYNCIFY_REMOVE` list, which removes instrumentation from
  them and their callers. The documentation calls such lists error-prone, and the assertion is what
  makes this one safe.
- **Costs.** The scheduler adds code to both editor and renderer modules; Milestone S5 measures it
  against the size tests and the goals above. Each extra worker costs a Web Worker and its stack.

### Performance

`//donner/benchmarks:task_scheduler_bench` (Google Benchmark, `-c opt`, perf lane only) measures:
spawn plus owner pop of an empty task, unstolen `Join`, stolen `Join`, wake latency from sleep to
first task, and `ParallelFor` scaling at 1, 2, 4 and 8 workers over fixed work. Wall-clock goals
are checked there, never in correctness tests.

## Security / Privacy

The scheduler runs only code the process supplies; its untrusted-input exposure comes from
clients that size work from documents.

- **Bounded queues.** Total queued tasks are capped (`maxQueuedTasks`); past the cap work runs
  inline, so a client that spawns per document element cannot exhaust memory through the queues.
- **Bounded recursion, or a named stop.** Helping past the margin is handed to a shallow worker
  when one exists; otherwise the last awake worker stacks queued work, which for waiting tasks is
  bounded by how many are queued at once, as described under worker stacks, and the stack-floor
  check turns exhaustion into a named release assertion instead of an overflow. Coroutine chains
  use symmetric transfer, and `ParallelFor` splits iteratively.
- **Memory pressure.** Task slots come from per-worker free lists that are reused; frame-pool
  growth is charged to the scheduler's own counters and bounded; an allocation failure aborts, as
  everywhere in Donner.
- **Isolation.** Tasks never take document guards, so a client bug cannot deadlock or reorder
  document access across threads.

## Testing and Validation

- **Deque stress** (`//donner/base/tasks:deque_tests`): one owner pushing and popping against
  several thieves; every item is taken exactly once; run under ThreadSanitizer.
- **Scheduler tests** (`//donner/base/tasks:task_scheduler_tests`): scopes, `Join`, `ParallelFor`
  (including the inline-below-grain counter), cancellation, nested scopes, coroutines, shutdown
  assertions, each at 0, 1 and 4 workers and in seeded mode.
- **Injected-delay race tests.** Test hooks delay a worker at chosen points (between the sleep
  decision and the sleep, between steal and run, before a scope's final decrement, between a
  creator's re-check and its parker wait, between a global-queue push and the publisher's read of
  `awake`, between a sleeper's decrement and its re-check, between the final decrement and the
  parker change with the scope destroyed at once, and between a drain and the sleep). Each race test
  was observed red with the matching protocol step deliberately broken before it counts as
  coverage.
- **Fuzzer** (`//donner/base/tasks:task_scheduler_fuzzer`): decodes a random fork-join program
  (spawns, joins, loops, awaits, stops) and checks that its result equals sequential evaluation,
  in seeded mode and with real workers.
- **ThreadSanitizer variant** reachable from `//...` through a configuration transition, covering
  the scheduler tests and every client's parallel tests, starting with
  `//donner/svg/components/style:style_parallel_tests`.
- **No deadlines.** Tests assert ordering and counts; generous hang guards turn a deadlock into a
  named failure.

## Other Clients

- **Filter row bands** ([0061](0061-multithreaded_rendering.md)): independent output rows with a
  join between passes map directly onto `ParallelFor` with the band height as grain, and one
  scheduler satisfies 0061's "one pool, one owner" rule.
- **Layer preparation** ([0060](0060-tinyskia_retained_raster_cache.md)): independent retained
  layers fit a `TaskScope` that spawns one task per layer and joins before composition.
- **Stylesheet parsing.** Each `<style>` element and imported sheet parses independently, but
  sheets are small, so this fits `Join` only for large imports.
- **Resource decoding.** Image and font decoding is independent per resource and often finishes
  after the caller moved on, which fits a `Task<T>` that a coroutine awaits.

Every client that touches the document's ECS registry inherits one rule from an evaluation done
for 0072 Part D: EnTT's registry is not safe for structural changes (entity creation, emplace or
remove, first use of a component type) from several threads, and making it safe would mean
forking EnTT at several times the per-lookup cost with nondeterministic iteration order. Clients
therefore do structural work before a parallel region, give tasks read-only registry access and
pre-existing slots or per-worker buffers to write, and commit serially. Phases that create
entities (shadow-tree instantiation, shapes) would need per-worker command buffers applied at a
sync point in an order derived from the tree; that is a separate future facility, not part of this
scheduler.

## Alternatives Considered

- **A renderer-owned pool as in 0061.** Correct for filters, but each feature would need its own
  or would reach into the renderer for it; a base facility gives every client one scheduler.
- **A third-party scheduler (oneTBB, Taskflow, libunifex).** Mature, but each would be a new
  dependency measured against the Wasm size ceilings, would have to build under
  `-fno-exceptions` and as an inline-only Wasm library, and would still need the seeded
  deterministic mode the tests rely on. None of those fits is established, while the core needed
  here (a deque, a sleep protocol, scopes and two awaitables) is small.
- **`std::execution`.** The C++26 sender model is the standard's direction, adopted into the C++26
  draft in 2024 ([trip report](https://herbsutter.com/2024/07/02/trip-report-summer-iso-c-standards-meeting-st-louis-mo-usa/)); the awaitable API here maps onto a sender scheduler, and `schedule()` can become
  a sender when the toolchains Donner builds with ship it.
- **A central shared queue for all work.** Simple, but every spawn and pop contends on one lock or
  atomic, which is what work stealing exists to avoid for small tasks; the global queue here only
  carries spawns from non-workers, drained work and outside coroutine resumptions.
- **Per-scope queues with drains into them.** Moving drained work into the queue of the scope it
  belongs to, so that scope's creator runs it, needs a way for idle workers to find those queues,
  which exposes a completing scope's memory, and leaves a coroutine-joined scope, whose creator is
  not running, without a runner. One global queue and the last-awake rule cover every scope the
  same way.

## Open Questions

- **Ownership.** Recommendation: no hidden global scheduler; the application or renderer creates
  one and passes it by pointer, which keeps tests deterministic and Wasm pool sizing explicit.
- **Editor worker count.** Recommendation: start with 2 scheduler workers in the editor build
  (raising `PTHREAD_POOL_SIZE` from 2 to 4) and revisit with Part D's measurements.
- **0061.** Recommendation: update 0061 to build its row bands on this scheduler when its pool
  milestone starts, rather than editing it in this change.
