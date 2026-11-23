# Design: Work-Stealing Task Scheduler

**Status:** Draft
**Author:** Claude Opus 5.5
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
  seeded deterministic mode replays one interleaving so tests reproduce.

Its first client is parallel style traversal, Part D of
[0072-css_cascade_performance](0072-css_cascade_performance.md). Filter row bands, layer
preparation, stylesheet parsing and resource decoding are named as later clients so the API is
not shaped by one user.

## Goals

- **Small-task overhead.** Spawning and running an empty task that the owner itself pops costs at
  most 50 ns, and a `join` of two empty closures that is not stolen costs at most 100 ns, on the
  benchmark in [Performance](#performance) at `-c opt`. The work-first principle (below) is the
  reason these are achievable.
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
  linked. Size growth is measured against the enforced ceilings: at most 2 KiB in the non-threaded
  renderer module and at most 24 KiB in the editor module.
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

- [ ] Milestone S1: Core scheduler (5 days)
  - [ ] Chase-Lev deque with the memory orders of Le et al., per-worker deques, a bounded
        injection queue, random-victim stealing, help-while-waiting joins, event-count sleep and
        wake, `TaskScope` and `Join`, zero-worker inline mode.
  - Done when: deque linearization stress tests, scope and join tests at 0, 1 and 4 workers, and
    the lost-wakeup injected-delay test (red with the protocol broken) are green.
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
- [ ] Milestone S5: WebAssembly (3 days). After S1.
  - [ ] Inline-only build for Wasm without pthreads; pool sizing, start-up and isolation probe for
        the editor build; size measurements.
  - Done when: both Wasm builds pass their size tests within the goals and an editor smoke test
    records the worker count it obtained.
- [ ] Milestone S6: Benchmarks (1.5 days). After S2 and S3.
  - Done when: `//donner/benchmarks:task_scheduler_bench` reports spawn, join, wake latency and
    scaling, and the overhead goals are met.

Total committed estimate: 18.5 engineering days. Part D of
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
  template <typename F> void spawn(F&& work);   // Runs work() on some worker or inline.
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

- **Join** runs `b` itself and offers `a` for stealing, then takes `a` back if no one stole it:
  the work-first principle, so an unstolen join costs about two calls and a deque push and pop.
- **ParallelFor** never creates a task for a range below `grain`. Above it, it splits in halves
  only while the split half is likely to be stolen: a range is split again when it was stolen
  (adaptive splitting), so on an idle pool work spreads quickly and on a busy pool it stays in
  large pieces.
- **Coroutines.** `co_await scheduler.schedule()` enqueues the coroutine handle and suspends; a
  worker resumes it. `co_await scope.join()` suspends until the scope's counter reaches zero, and
  the last finishing task resumes the awaiting coroutine. `Task<T>` is lazily started and resumes
  its awaiter by symmetric transfer, so chains of awaits do not grow the native stack. A
  coroutine's promise calls `std::abort()` from `unhandled_exception`, which cannot be reached in a
  `-fno-exceptions` build.
- **Errors and cancellation.** Work returns `void`; results and errors are written to
  caller-owned slots, or returned as `Task<ParseResult<T>>`. `requestStop()` sets a flag that
  queued tasks check before running and long tasks poll; stopped tasks still complete their scope
  accounting, so `wait()` always returns.

### Internals

- **Deques.** Each worker owns a Chase-Lev deque with signed 64-bit indices: the owner pushes and
  pops at the bottom, thieves take from the top, and the last element is claimed by a
  compare-and-swap on the top index. The memory orders are the C11 ones of Le et al.'s Figure 1
  (relaxed and release-acquire accesses, sequentially consistent fences in `take` and `steal`,
  sequentially consistent compare-and-swap), whose ARM counterpart they prove correct, not a port of
  the original pseudo-code. The array grows by doubling up to the per-worker share of
  `maxQueuedTasks`; past it, `spawn` runs the task inline, which keeps the program correct and
  memory bounded.
- **Injection queue.** Work submitted from a thread that is not a worker (the first spawn of a
  scope, a coroutine resumed from elsewhere) goes to one bounded multi-producer queue that idle
  workers drain before stealing.
- **Stealing.** An idle worker drains the injection queue, then tries random victims a bounded
  number of times, then prepares to sleep.
- **Sleeping without lost wakeups.** An event count: a worker that found nothing reads the count,
  checks every queue once more, and sleeps only if the count is unchanged; a producer that pushes
  work increments the count and wakes one sleeper if any are sleeping. On native platforms the
  sleep is a futex-style wait (`std::atomic::wait`); on Wasm it is `Atomics.wait` through the same
  call, which is permitted on workers.
- **Waiting.** `TaskScope::wait()` on a worker runs tasks (its own first, then stolen) until the
  scope's counter reaches zero, so waiting never idles a worker. Helping is depth-limited (64
  nested helps); past the limit the waiter sleeps on the scope's counter, which is safe because the
  tasks it waits for are on other workers' deques or running. On a thread that is not a worker,
  `wait()` helps from the injection queue, then sleeps on the counter.
- **Task storage.** A spawned closure is stored in a fixed-size slot (64 bytes inline, larger
  closures in a per-worker free list of blocks) so small tasks never call `malloc`. Coroutine frames
  allocate through a per-scheduler frame pool via the promise's `operator new`.
- **Seeded mode.** With `seed` set, the scheduler runs on the calling thread only and, whenever
  more than one task is runnable, picks the next one with a seeded generator. It reproduces task
  interleavings, not instruction interleavings; data races are ThreadSanitizer's job.
- **Shutdown.** The destructor asserts no scope is live (`UTILS_RELEASE_ASSERT`), sets the stop
  flag, wakes every worker and joins them. A coroutine suspended on `schedule()` after destruction
  began is a programming error caught by the same assertion.
- **Nested parallelism.** Scopes nest freely; nested work goes to the current worker's deque, so a
  nested parallel loop inside a task adds parallelism without adding threads.
- **Locks.** A task must not hold a lock across `wait()` or `co_await`: helping could run a task
  that takes the same lock. Debug builds record, per thread, whether it is inside a task, and the
  document access coordinator asserts that no document guard is acquired inside one, which makes
  the "tasks never take document guards" rule checkable.
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
- **No unbounded recursion.** Helping is depth-limited, coroutine chains use symmetric transfer,
  and `ParallelFor` splits iteratively.
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
  decision and the sleep, between steal and run, before a scope's final decrement). Each race test
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
