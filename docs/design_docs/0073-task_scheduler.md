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
[0072-css_cascade_performance](0072-css_cascade_performance.md). Filter row bands, layer
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
        per-worker deques, a bounded injection queue for closures and an intrusive resumption list
        for coroutines, random-victim stealing, help-while-waiting joins with scope watermarks,
        per-scope shared queues and epoch words, drain before sleep, `trySpawn`,
        event-count sleep and wake, cross-thread slot return, `TaskScope` and `Join`, zero-worker
        inline mode.
  - Done when: deque linearization stress tests, scope and join tests at 0, 1 and 4 workers, the
    liveness tests listed under [Waiting](#internals), and the lost-wakeup injected-delay test (red
    with the protocol broken) are green. Each liveness test was observed red with the watermark
    taken at `wait()` instead of at scope construction, through a test hook.
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
[0072-css_cascade_performance](0072-css_cascade_performance.md) needs S1 to S3, and S4 for its
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
  follows at zero workers. `co_await scope.join()` suspends until the scope's counter reaches zero,
  and the last finishing task resumes the awaiting coroutine. `Task<T>` is lazily started and
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
- **Injection queue.** A closure spawned from a thread that is not a worker (the first spawn of a
  scope) goes to one bounded multi-producer queue that idle workers drain before stealing; when the
  queue is full the closure runs inline on the spawning thread. A coroutine resumption scheduled
  from a thread that is not a worker cannot run inline, since that would break the meaning of
  `schedule()`, and cannot be dropped; it goes instead to an intrusive list whose link lives in the
  awaiter inside the suspended coroutine frame. That list needs no allocation and cannot overflow:
  its link is part of the suspended frame, so each suspended coroutine adds exactly one entry and
  the list grows only as frames do.
- **Stealing.** An idle worker drains the injection queue, then tries random victims a bounded
  number of times, then prepares to sleep.
- **Sleeping without lost wakeups.** An event count, following Rayon's protocol: a worker that
  found nothing becomes sleepy, records the count, checks every queue once more, and sleeps only if
  the count is unchanged. A worker pushing to its own deque does no read-modify-write: it loads the
  sleep state and touches the count and wakes a sleeper only when some worker is sleepy or
  asleep, so the spawn fast path stays a deque push. A missed wake on such a push costs only
  parallelism, never progress, because the pushing worker runs its own deque. Pushes from outside
  the pool (injection queue, coroutine resumptions) issue a sequentially consistent fence and
  re-check the sleepers, and a sleepy worker fences and re-checks those queues before sleeping, so
  either the producer sees the sleeper or the sleeper sees the work. On native platforms the sleep
  is `std::atomic::wait`; in the editor Wasm build that reaches `emscripten_futex_wait`, which the
  build wraps (`-Wl,--wrap=emscripten_futex_wait`) for the GPU-owning application pthread. The
  wrapper passes every other thread straight to the real futex wait, so scheduler workers sleep
  through `Atomics.wait`, which is permitted on workers; Milestone S5 tests that branch.
- **Where spawned tasks go.** A `TaskScope` constructed on a worker records that worker's deque
  bottom index as its watermark. Scopes on one worker nest last in, first out, because a scope
  joins in its destructor before its creator returns. A spawn from the scope's creator goes onto
  the creator's own deque, above the watermark. A spawn made by one of the scope's tasks while it
  runs on any worker (a flat spawn) goes onto that worker's own deque like any other push, so a
  tree traversal gets ordinary deque-to-deque work stealing. Each queued task records its scope.
  Every scope also has a shared queue: an intrusive list through the task slots, so it needs no
  allocation and cannot overflow. It receives spawns from a creator that is not a worker and tasks
  moved there by the drain step below. Idle workers take work from their own deque, then from
  scopes' shared queues and the injection queue, then by stealing.
- **Scope state word.** Each scope has one atomic epoch word. It is incremented, after the change
  it announces, whenever the scope's counter reaches zero and whenever a task is put into the
  scope's shared queue, and the incrementing thread then notifies waiters on it. A creator that
  is about to sleep reads the epoch, re-checks rules 1 and 2 below, and only then calls
  `epoch.wait(read value)`. If the change happened before the read, the re-check sees it; if after,
  the value differs and the wait returns at once. A notify that did not change a waited-on word
  would let `std::atomic::wait` block again, so nothing wakes a creator except a change to this
  word. The injected-delay tests cover a delay between the re-check and the wait, and between the
  change and the increment, each observed red with the increment moved before the change.
- **Waiting.** Only a scope's creator waits on it. While it waits it applies, in order:

  1. If its deque holds an entry above the scope's watermark, or the scope's shared queue is not
     empty, take one and run it, whatever the stack margin.
  2. If the scope's counter is zero, return.
  3. Otherwise steal unrelated work while the stack has more than the margin left (checked
     against the stack pointer). Past the margin, or when there is nothing to steal, drain and
     sleep: move every entry still on its own deque, all of which are below its watermark, into
     the shared queue of the scope each entry belongs to (incrementing that scope's epoch), then
     sleep on this scope's epoch with the protocol above, and start again at rule 1 when it
     changes.

  A worker never runs a deque entry below its innermost live watermark; those entries leave only
  by being stolen from the top or moved by the drain step. An idle worker also drains before it
  sleeps on the global event count (its deque is then empty, so the step is free).

  **Liveness.** The invariant is that every queued task is either on the deque of a worker that is
  not asleep, or in its scope's shared queue, because no worker sleeps with a non-empty deque. A
  worker that is not asleep eventually runs, is robbed of, or drains each entry of its deque. A
  task in a scope's shared queue is run by that scope's creator through rule 1, and the move
  changed the epoch that creator sleeps on, so a sleeping creator wakes. A scope is waited on only
  by its creator and waits only for tasks it spawned and their descendants, so the waits-for
  relation is acyclic and finite. Take a waiting scope none of whose running tasks waits: its
  running tasks finish and its queued tasks are run or moved to where its creator runs them, so it
  completes; by induction every wait completes, even when every worker is past its margin. The
  cases raised in review, each a liveness test in Milestone S1:

  - One worker recursing: each level constructs a scope, spawns its child above the watermark and
    waits; rule 1 runs the child on the same stack. Recursion depth is the program's own nesting,
    as in a serial run. The reported schedule (deep nested scopes, the next child still on the
    worker's own deque, an external caller waiting on the outer scope) runs the child instead of
    sleeping.
  - Every worker past its margin: each drains before sleeping, so all queued tasks sit in shared
    queues whose creators are awake or are woken by the move.
  - Two workers past the margin, each holding the other's scope's flat task below its own
    watermark (the schedule that a deque without the drain step deadlocks on): both drain, each
    task lands in its own scope's shared queue, and each creator runs its task by rule 1.
  - A stolen task that waits: it constructs its own scope on the thief, so its children sit above
    the thief's watermark.
  - `Join`: an internal scope is constructed before `b` is pushed, so `b` is above its watermark;
    after `a` returns, rule 1 takes `b` back unless it was stolen.
  - A spawn that meets the queue bound: `spawn` runs it inline at spawn time and it counts as
    finished; `trySpawn` returns false and the caller keeps the work (0072 Part D walks such a
    subtree on its own heap stack, so no task frames nest).
  - Non-worker scopes: below.

  **Skipping the wake on a push.** A worker pushing onto its own deque may skip the global event
  count, because it runs, loses to a thief, or drains that entry before it can sleep, so a missed
  wake costs parallelism only. Moves into a shared queue, pushes into the injection queue and
  coroutine resumptions always take the fenced wake protocol, and moves also increment the scope's
  epoch.
- **Worker stacks and the margin.** Workers are created with an explicit stack size (512 KiB by
  default, an option) instead of the platform default, which differs between platforms and
  toolchains. The margin limits only stealing, and a stolen task can come from any client, so the
  margin is one scheduler-wide value: the largest stack bound declared by any client of that
  scheduler (`TaskSchedulerOptions::helpMarginBytes`, which clients raise when they register),
  plus slack. A client's declaration is its guarantee about one stolen task: run on a worker,
  that task, including any waits it makes and the rule 1 work those waits run, never uses more
  native stack than the declared bound. Rule 1 itself runs regardless of the margin, so a client
  whose tasks wait must bound its own scope nesting to meet that guarantee. The style traversal of
  0072 Part D declares one task frame: one flat scope, tasks that never wait, an explicit heap
  stack for the inline part of a subtree, and `trySpawn` with the heap stack as the fallback, so a
  style task's stack use does not depend on tree depth (0072 Milestone D3 measures the bound at
  the parser's nesting limit). The default margin is 128 KiB.
- **Non-worker waits.** A scope created on a thread that is not a worker (the render worker
  holding document access, for example) has no deque: its spawns go to its shared queue, with a
  fenced wake for idle workers. Flat spawns by its tasks running on workers go to those workers'
  deques as above. When that thread waits, it runs tasks from its scope's shared queue (rule 1),
  never from the injection queue or another worker's deque, and sleeps on the scope's epoch when
  that queue is empty and the counter is not zero. Drained tasks of its scope arrive in its shared
  queue and wake it. Such a thread never runs another client's task while it holds a guard.
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
  flag, wakes every worker and joins them. A coroutine suspended on `schedule()` after destruction
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
- **No unbounded recursion.** Stealing while waiting stops at a stack margin, helping within a
  wait follows the program's own nesting, coroutine chains use symmetric transfer, and
  `ParallelFor` splits iteratively.
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
  creator's re-check and its epoch wait, between a shared-queue move and its epoch increment, and
  between a drain and the sleep). Each race test
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
- **A central shared queue.** Simple, but every spawn and pop contends on one lock or atomic, which
  is what work stealing exists to avoid for small tasks.

## Open Questions

- **Ownership.** Recommendation: no hidden global scheduler; the application or renderer creates
  one and passes it by pointer, which keeps tests deterministic and Wasm pool sizing explicit.
- **Editor worker count.** Recommendation: start with 2 scheduler workers in the editor build
  (raising `PTHREAD_POOL_SIZE` from 2 to 4) and revisit with Part D's measurements.
- **0061.** Recommendation: update 0061 to build its row bands on this scheduler when its pool
  milestone starts, rather than editing it in this change.
