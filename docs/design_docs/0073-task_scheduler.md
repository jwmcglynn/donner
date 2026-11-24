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
- On by default, configurable: the default configuration runs workers, a runtime option sets the
  worker count or disables them, and one Donner-wide build setting, exposed to consumer modules,
  compiles threading out of the scheduler and the renderer for targets that cannot have threads.
- Best in class, and benchmarked: measured against named peer schedulers (oneTBB, Taskflow,
  enkiTS, Google Filament's JobSystem, libfork) with acceptance thresholds stated against the best
  peer.

Its first planned client is parallel style traversal, Part D of
[0072-css_cascade](0072-css_cascade.md). The scheduler is implemented before that work and no
milestone here depends on a 0072 milestone. Filter row bands, layer preparation, stylesheet parsing
and resource decoding are named as later clients so the API is not shaped by one user.

## Goals

- **Small-task overhead.** Targets, not yet measured: spawning and running an empty task that
  the owner itself pops costs at most 50 ns, and a `Join` of two empty closures that is not stolen
  costs at most 100 ns, on the benchmark in [Peer benchmarks](#peer-benchmarks) at `-c opt`. The
  work-first principle (below) is why these are plausible: the owner's fast path has no
  read-modify-write atomic.
- **Best in class.** On every workload of the [peer benchmarks](#peer-benchmarks) the scheduler
  meets the acceptance thresholds stated there against the fastest of the named peers on the same
  platform, in one build with one toolchain. A missed threshold changes the design until it is met,
  or is recorded as an open question with the measured data; it is never waived silently.
- **On by default, off at no cost.** The default configuration of every shipped target that can
  have threads runs workers ([Configuration](#configuration)). With zero workers at run time, or
  with threading compiled out, every client runs serially on the calling thread with identical
  results, and the peer benchmarks' single-thread workload costs at most 2 percent over a plain
  serial loop. With threading compiled out, neither the scheduler nor the renderer links a thread,
  pthread, condition-variable or atomic-wait dependency, checked at link time.
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
- No change to the document access model in threaded builds: tasks never acquire document guards.
  (With threading compiled out, concurrent document access is unavailable; see
  [Renderer threading](#renderer-threading).)
- No GPU command scheduling; Geode keeps its own submission model.
- No client designs here beyond naming their fit; each client is designed in its own document.
- No benchmark against schedulers that cannot be built in the same C++ Bazel build with the same
  toolchain (Rayon, language runtimes, operating-system thread pools); they are cited, not
  compared. Peer libraries are benchmark-only and never linked into a shipped target.
- No guarantee that threading compiled out removes threads the platform itself creates (GPU
  driver callback queues, the browser's worker model); targets that need those stay threaded.

## Next Steps

- Land Milestone S1 (deque, workers, scopes, join, inline mode, sleep and wake, adoption) with its
  stress tests, then S4's ThreadSanitizer target so every later milestone runs under it.
- Land S7's build setting early, so the compiled-out lane runs from the first client onward.

## Implementation Plan

Estimates are engineering days, including tests and review fixes. No milestone here depends on a
milestone of [0072-css_cascade](0072-css_cascade.md); the scheduler lands first.

- [ ] Milestone S1: Core scheduler (8.75 days)
  - [ ] Chase-Lev deque with the memory orders of Le et al. on a fixed-capacity ring,
        per-worker deques, one intrusive global queue, bounded random-victim stealing,
        help-while-waiting joins with scope watermarks, parkers with the waiting bit, drain
        before sleep, the last-awake sleep rule and its publish protocol in read-modify-write
        form, the stack-floor assertion, `trySpawn`, cross-thread slot return, `TaskScope` and
        `Join`, zero-worker inline mode.
  - [ ] Adopted threads with the own-deque-only help policy (1.5 days); the wake after a local
        push and the `searching` count (0.5 day); worker QoS on Apple platforms and
        `DefaultWorkerCount()` (0.25 day). Fixed-capacity deques save 0.75 day against growable
        ones.
  - Done when: deque linearization stress tests, scope and join tests at 0, 1 and 4 workers, the
    liveness tests listed under [Internals](#internals), and the lost-wakeup injected-delay test
    (red with the protocol broken) are green. Each liveness test must be observed red with the
    last-awake rule disabled, through a test hook, before it counts.
- [ ] Milestone S2: Parallel loops and determinism (3.5 days). After S1.
  - [ ] `ParallelFor` with grain and adaptive splitting, its const-body check and chunk cap, and
        the shared-cursor variant for S6 to decide between (0.5 day); seeded deterministic mode;
        test hooks.
  - Done when: the inline-below-grain counter test, the seeded-mode replay test and the scaling
    benchmark target exist and pass.
- [ ] Milestone S3: Coroutines (3 days). After S1.
  - [ ] `Task<T>`, `TaskScheduler::schedule()` and scope-join awaitables, frame allocation from the
        scheduler's pool.
  - Done when: coroutine tests at 0, 1 and 4 workers, including resumption from a non-worker
    thread and nested awaits, pass under ThreadSanitizer.
- [ ] Milestone S4: ThreadSanitizer target and fuzzer (3.25 days). After S1.
  - [ ] A ThreadSanitizer test variant reachable from `//...` through a configuration transition,
        and `//donner/base/tasks:task_scheduler_fuzzer`.
  - [ ] The deque's all-sequentially-consistent form for ThreadSanitizer builds, the stress test
        on both forms, and a red run on a deliberately weakened ordering (0.25 day). Verify here
        whether the repository's Clang ThreadSanitizer models standalone fences; the protocol does
        not depend on the answer, but the record of it decides whether the second form stays.
  - Done when: the variant runs the scheduler tests in normal CI and the fuzzer is green.
- [ ] Milestone S5: WebAssembly (4 days). After S1.
  - [ ] Inline-only build for Wasm without pthreads; pool sizing, worker stack size, start-up and
        isolation probe for the editor build; size and memory measurements.
  - Done when: both Wasm builds pass their size tests, an editor smoke test records the worker
    count it obtained and the peak heap with the scheduler running, the fixed memory still has
    the headroom its build contract requires, and a test shows scheduler workers' sleeps take the
    pass-through branch of the editor's wrapped futex wait.
- [ ] Milestone S6: Peer benchmarks (6 days). After S2 and S3.
  - [ ] Benchmark-only pinned modules for oneTBB, Taskflow, enkiTS, Filament JobSystem and
        libfork (1.5 days); adapters and workloads W1 to W8 (2 days); the quiet-machine gate,
        repetition and reporting script (1 day); runs on the three native platforms and the
        editor Wasm build, with results recorded in this document (1.5 days).
  - [ ] Verify here, before relying on them: that oneTBB builds with exceptions disabled (or
        needs `-fexceptions` on its own targets), that its registry module builds on all three
        platforms, that the Filament `libs/utils` subset needs no more files than listed, and that
        each peer builds under the repository's toolchains at all. A peer that cannot be built is
        recorded here with the reason, not dropped silently.
  - Done when: every acceptance threshold under [Peer benchmarks](#peer-benchmarks) is met or is
    recorded as an open question with its measured data, and the overhead goals are met. Tuning
    work to close a gap is not in this estimate.
- [ ] Milestone S7: Configuration and threading switch (2 days). After S1; the build setting
      itself can land first.
  - [ ] `//donner/base:threading` with its `donner.configure(threading = ...)` attribute, the
        CMake option `DONNER_THREADING`, and the scheduler's inline implementation selected by it
        (0.75 day).
  - [ ] The compiled-out test variant through a configuration transition, added to the variant
        reset list, with the scheduler tests (0.5 day).
  - [ ] The shared default scheduler and the runtime options of `donner-cli` and the native
        editor (0.5 day).
  - [ ] Document the setting, the module attribute, the CMake option and the runtime option in
        `docs/building.md` and `docs/multithreading.md` (0.25 day).
  - Done when: the compiled-out variant and the default (threaded) build both run the scheduler
    tests in CI, and a consumer-module build with `threading = False` builds `@donner//:donner`.
- [ ] Milestone S8: Renderer threading switch (3.5 days). After S7.
  - [ ] The no-op lock type and the conversions listed under
        [Renderer threading](#renderer-threading) (1.5 days), `ThreadingMode::ConcurrentDom`
        rejected when compiled out (0.5 day), incompatibility of the editor and native Geode
        targets with the setting (0.5 day), and the link-time symbol check on Linux, macOS and
        the renderer Wasm module (1 day).
  - Done when: the compiled-out variant renders the renderer's golden tests bit-identically to
    the threaded build, and the symbol check is green and was observed red with one
    `std::mutex` reinstated.
- [ ] Release gate, before the first client ships with workers enabled (no estimate; checks):
  - [ ] Every peer-benchmark threshold met, or recorded as an open question the maintainer
        accepted with its data.
  - [ ] The ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer lanes clean on the
        scheduler and on that client.
  - [ ] The default (threaded) build, the runtime zero-worker runs and the compiled-out variant
        all green in CI.
  - [ ] Bit-identical output between serial and threaded runs for every client that ships,
        asserted by that client's differential tests at 0, 1 and N workers.

Total committed estimate: 34 engineering days (S1 8.75, S2 3.5, S3 3, S4 3.25, S5 4, S6 6,
S7 2, S8 3.5). Part D of [0072-css_cascade](0072-css_cascade.md) needs S1 to S3, and S4 for its
race coverage; its release with workers on needs the release gate.

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

### Prior Art: Filament JobSystem

Google Filament's `utils::JobSystem` was compared with this design from its source at release tag
`v1.81.0` (`libs/utils/include/utils/JobSystem.h`, `libs/utils/src/JobSystem.cpp` and
`WorkStealingDequeue.h`). Its 2026 redesign uses a pooled, fixed-size job (one 64-byte record with
48 bytes of inline storage, from one preallocated pool behind a lock-free free list) and a
`parallel_for` in which one root job and at most one helper per thread claim chunks from a shared
atomic cursor, replacing recursive splitting. The redesign's commit message reports
`BM_JobSystemParallelFor` falling from 3.71 ms to 0.10 ms on a 16-core Apple silicon Mac and from
4.02 ms to 0.63 ms on a Pixel 8 Pro; that is the commit's own claim, not reproduced here.

Donner adopts:

- Caller participation: a thread may adopt a worker slot so its spawns go to its own deque
  (`JobSystem::adopt`), restricted here to its own deque (see [Non-worker waits](#internals)).
- A wake check on every local push when threads sleep, as Filament's `put` does.
- Notifying a parker only when its owner may be asleep, as Filament keeps waiter bits beside the
  running count.
- Sequentially consistent read-modify-write pairs instead of standalone fences for the sleep
  handshake, and an all-sequentially-consistent deque form for ThreadSanitizer builds.
- Fixed-capacity deques, a stated bound on steal probes with a seeded per-worker generator, a
  const-invocable `ParallelFor` body, a cap on chunk count, and the shared-cursor loop as a
  measured variant.
- Worker QoS on Apple platforms, which Filament sets to one interactive class for every thread.

Donner keeps different:

- Per-worker slot free lists, not one global pool: no shared compare-and-swap per task.
- No global count of queued jobs: Filament updates `mActiveJobs` on every put, pop and steal;
  the owner path here has no read-modify-write.
- No one-slot `nextJob` cache: it would hold a stealable entry outside the deque's index space and
  break the watermark argument.
- Bounded helping: a Filament waiter runs any job with no stack guard; here watermarks, the margin
  and the stack-floor assertion bound it.
- Capacity failure runs work inline or returns false from `trySpawn`; Filament's `create` returns
  null and running a null job does nothing, so work can be dropped.
- A global queue for threads that are not workers, coroutines, cooperative stop and the seeded
  deterministic mode, none of which Filament has.

## Proposed Architecture

### API

All types live in `namespace donner::tasks`, in `donner/base/tasks/` (a new `donner_cc_library`
target, so code that never schedules work does not link it).

```cpp
struct TaskSchedulerOptions {
  std::size_t workerCount = DefaultWorkerCount(); // 0: run everything inline on the caller.
  std::optional<std::uint64_t> seed;         // Seeded deterministic mode (tests).
  std::size_t dequeCapacity = 4096;          // Entries per worker deque; a power of two.
  std::size_t maxExternalTasks = 64 * 1024;  // Bound on live tasks spawned by non-workers.
  std::size_t adoptableThreads = 1;          // Slots reserved for adopt(); fixed at construction.
  std::size_t helpMarginBytes = 128 * 1024;  // Stack kept free when stealing while waiting.
  std::size_t idleSpinRounds = 1;            // Steal rounds before sleeping; set from S6's W5.
};

// Hardware threads available to the process (the affinity mask on Linux, the active processor
// count elsewhere) minus one for the adopting caller, at least 1 and at most 32. 0 when threading
// is compiled out.
std::size_t DefaultWorkerCount();

class TaskScheduler {
public:
  explicit TaskScheduler(TaskSchedulerOptions options);
  ~TaskScheduler();                          // Joins every worker; asserts no scope is live.
  std::size_t workerCount() const;           // Workers actually started (may be fewer).
  ScheduleAwaitable schedule();              // co_await: continue on a worker.
  // Gives the calling thread a slot for the lifetime of the returned object: its spawns go to
  // its own deque, and its synchronous waits run only that deque's entries. Empty when no slot is
  // free, with zero workers, in seeded mode, and with threading compiled out.
  [[nodiscard]] AdoptedThread adopt();
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
  large pieces. The body is one shared instance invoked concurrently, so `ParallelFor`
  static_asserts that it is invocable as `const F&` (Filament's `parallel_for` enforces the same).
  The effective grain is at least `ceil((end - begin) / 4096)`, so a small grain over a large range
  cannot create millions of tasks. Milestone S2 also implements Filament's shared-cursor scheme
  (one root, at most `workerCount()` helpers, each claiming `grain`-sized chunks by a relaxed
  `fetch_add`) behind the same API; Milestone S6 keeps whichever wins workloads W2 and W4, and the
  other is deleted, not kept as an option.
- **Zero workers.** `spawn` runs the work inline. `trySpawn` returns false without running it, so a
  client with its own explicit work stack (0072 Part D) keeps its native stack flat at zero workers
  as it does at the queue bound, instead of recursing once per spawned level. `Join` runs `a` then
  `b`; `ParallelFor` runs one `body(begin, end)`. This holds whether zero workers came from the
  runtime option, from workers that failed to start, or from threading compiled out. Seeded mode is
  not zero-worker mode: its tasks are queued and run on the calling thread in seeded order, so
  `trySpawn` there returns true as it does with workers.
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

### Configuration

Multithreading is on by default. Two controls change that, one at run time and one at build time.

- **Runtime option.** `TaskSchedulerOptions::workerCount` sets the worker count; 0 disables
  workers, and every client then runs serially on the calling thread with identical results (the
  zero-worker rules above). Donner entry points that run a parallel client accept the scheduler to
  use; given none, they use a shared default scheduler created with default options on first use
  and released with its last user. An embedder that wants other options configures the default
  before its first use, or passes its own scheduler. Tests always pass their own, so their worker
  counts are explicit.
- **Build setting.** One Donner-wide setting, `//donner/base:threading`, compiles threading out of
  the scheduler and the renderer together ([Renderer threading](#renderer-threading)). It follows
  the convention of the existing renderer settings (`renderer_backend`, `filters`, `text`): a
  public `bool_flag` whose default comes from the `donner_config` repository that the `donner`
  module extension writes, with a `config_setting`, `//donner/base:threading_disabled`, that
  build rules select on.
  - Values: `true` (the default, threading compiled in) and `false`.
  - What `false` does: the scheduler API stays source-compatible, and `TaskScheduler`,
    `TaskScope`, `Join`, `ParallelFor`, `Task<T>` and `adopt()` compile to the inline
    implementation. `workerCount()` and `DefaultWorkerCount()` are 0, `adopt()` returns an empty
    object, and every client takes its serial path. No thread, pthread, condition-variable or
    atomic-wait code is linked by the scheduler or the renderer.
  - How a consumer module sets it: on the command line,
    `--@donner//donner/base:threading=false`; or in its `MODULE.bazel`, through the existing
    extension, `donner.configure(threading = False)`, which only the root module's tag sets, as
    for the other attributes. Inside this repository it is `--//donner/base:threading=false`.
  - Relation to the runtime option: compiling out wins. The runtime option is accepted and
    ignored, and the scheduler reports zero workers.
  - Targets that force it: the non-threaded Wasm builds (the renderer's `wasm` configuration)
    set it to `false` through their existing transitions, and a compile-time check rejects
    threading compiled in for an Emscripten build without `__EMSCRIPTEN_PTHREADS__`. Targets
    that cannot work without threads (the editor, native Geode, below) are incompatible with
    `false` through `target_compatible_with`, so a broad build skips them instead of failing.
  - CMake: the Bazel-to-CMake generator maps the setting to `option(DONNER_THREADING ... ON)`
    beside `DONNER_FILTERS` and `DONNER_TEXT`, and links `Threads::Threads` only when it is on.
    The CMake mirror builds the libraries only, which are exactly what the setting covers.
  - CI: the default build runs threaded. One test variant, a configuration transition in the
    single native Bazel invocation and listed with the other variant settings in the variant
    reset list, builds and tests with threading compiled out, and runs the link-time symbol check.
- **Defaults per shipped target.**
  - Library: `DefaultWorkerCount()` workers in the shared default scheduler.
  - `donner-cli`: the shared default, with a `--threads=N` option (`0` serial).
  - Editor, native: `DefaultWorkerCount()` minus one, at least 1, because the UI thread also
    runs; the render worker adopts a slot for the style pass. An editor setting changes it.
  - Editor, threaded Wasm: 2 workers (raising `PTHREAD_POOL_SIZE` from 2 to 4), dropping to the
    workers that started; see [WebAssembly](#webassembly).
  - Renderer Wasm without pthreads: threading compiled out, 0 workers.

The scheduler's tests run in both modes: every scheduler test at 0, 1 and 4 workers and in
seeded mode in the default build, and again in the compiled-out variant, where they run at zero
workers and assert `workerCount() == 0` whatever the options asked for. Each client's
differential tests compare serial and threaded output in the default build and run serially in
the compiled-out variant.

### Renderer Threading

The build setting covers the renderer as well as the scheduler. Read from the tree today, with
what `threading = false` does to each:

- **Software renderer and compositor.** `RendererTinySkia` and `donner/svg/compositor` create no
  threads. `CompositorController` holds an `std::atomic<bool>` cancellation flag; plain atomics
  without wait or notify need no thread runtime and stay.
- **Document access.** `DocumentState` implements `ThreadingMode::ConcurrentDom` with a mutex and
  condition variables. Compiled out, that code is removed, the enumerator stays for source
  compatibility, and selecting it fails with a release assertion naming the setting. Decision for
  the maintainer, because it changes public behavior: the alternative, removing the enumerator,
  breaks source compatibility for code that names it.
- **Process-wide caches and registries.** Mutexes guard shared state in
  `donner/base/PackagedResource.cc`, `donner/svg/components/resources/SubDocumentAdmission.cc`,
  `donner/svg/resources/CatalogEncodedFontStore.cc`, the Geode texture-pool registry in
  `RendererGeode.cc`, `HeadlessDevicePool.h`, `GeodeFilterEngine.cc`'s resource cache,
  `GeodeHandleRetirement.h` and `donner/gpu/vulkan/VulkanQueueProgress.h`. They change to one
  Donner lock type that is a mutex when threading is compiled in and a no-op when it is out.
  Decision for the maintainer: with threading compiled out the library may be called from one
  thread at a time only, which an embedder that runs its own threads must serialize; that is a
  documented contract change for that configuration.
- **Counters and published pointers.** Relaxed atomics for identifiers and statistics
  (`ImageComponent`, `ResourceManagerContext`, `XMLParser`, `HeapSizeHistogram`,
  `MemoryAttribution`, the font manager's default provider, Geode encoder and device identifiers)
  and `thread_local` thread roles in `FatalAbort.cc` stay; they need no thread runtime.
- **Native Geode.** On Metal, command-buffer completion handlers run on a queue Metal creates
  (`MetalDevice.h`), and `GeodeWgpuAdapterDevice` waits on a condition variable for completion
  callbacks. The setting cannot remove driver-created threads, so native Geode targets are
  incompatible with `threading = false`. The Vulkan device polls fences from its owning thread
  with no cross-thread callbacks, so it could be made compatible later. Geode's browser backend
  already runs in the non-threaded renderer Wasm module and stays available there.
- **Editor.** `AsyncRenderer` owns a render worker `std::thread`, `EditorShell` loads documents
  with `std::async`, and the browser build relies on `PROXY_TO_PTHREAD`, the GPU-owner thread
  and Emscripten proxy queues. The editor's architecture requires those threads, so editor targets
  are incompatible with the setting rather than given a synchronous path.

No public renderer API in `donner/svg/renderer` is asynchronous by contract (none returns a
future or completes on another thread), so none needs a synchronous replacement. The one
asynchronous-by-contract renderer is the editor's `AsyncRenderer`, which is unavailable at compile
time with threading compiled out: a synchronous emulation would change its foreground and
thumbnail lane semantics, and its only users are editor targets that are already incompatible.

The compiled-out CI variant verifies the claim by a link-time symbol check of `@donner//:donner`
on Linux and macOS and of the renderer Wasm module: no `pthread_create`, `pthread_cond_*`,
`pthread_mutex_*`, `std::thread`, `std::condition_variable` or atomic wait and notify symbols
(the standard libraries' atomic-wait entry points and `emscripten_futex_wait`) are referenced.
Milestone S8 lands this with the scheduler's first release for the software renderer, the
library's caches and `DocumentState`; making native Geode on Vulkan compatible is an open question
below, not part of S8.

### Internals

- **Deques.** Each worker owns a Chase-Lev deque with signed 64-bit logical indices that only grow
  and never wrap or reset (the array slot is the index modulo the capacity), so watermarks stay
  comparable for the life of the scheduler: the owner pushes and pops at the bottom, thieves take
  from the top, and the last element is claimed by a compare-and-swap on the top index. The memory
  orders are the C11 ones of Le et al.'s Figure 1 (relaxed and release-acquire accesses,
  sequentially consistent fences in `take` and `steal`, sequentially consistent compare-and-swap),
  whose ARM counterpart they prove correct, not a port of the original pseudo-code. Each deque is a
  fixed ring of `dequeCapacity` entries (a power of two; 4,096 by default, 1,024 in the editor Wasm
  build), allocated once with its worker or adoptable slot and never resized, as in Filament's
  `WorkStealingDequeue`. When a push would exceed it, `spawn` runs the task inline and `trySpawn`
  returns false, which keeps the program correct and memory bounded. There is no growth path and
  so no old array a thief could still be reading. Builds with ThreadSanitizer compile the deque
  with every access sequentially consistent and no fences (the form Filament's deque uses),
  because whether ThreadSanitizer models standalone fences is to be verified in Milestone S4; the
  deque stress test runs on both forms, and the production form is still the Le et al. one.
- **Global queue.** Besides the per-worker deques there is one global queue: a multi-producer,
  multi-consumer list linked through the task records themselves (and, for coroutine resumptions,
  through the awaiter in the suspended frame), so pushing needs no allocation and the queue cannot
  overflow; its length is bounded by the live task records. Records that workers create are
  bounded by the deques (a full deque runs work inline), and records that threads which are not
  workers create are counted, one atomic increment on that already fenced path and one decrement
  when the record is freed, against `maxExternalTasks`; past it their `spawn` runs inline and
  `trySpawn` returns false. Drains move existing records and create none. It receives
  spawns from threads that are not workers, coroutine resumptions scheduled from outside a worker,
  and everything a worker drains before it sleeps. There are no per-scope queues: a queue that
  belongs to a scope would expose the scope's memory to idle workers looking for work while the
  scope's creator completes and destroys it.
- **Where spawned tasks go.** A spawn from a worker goes onto that worker's own deque, whether it
  comes from a scope's creator or from one of the scope's tasks (a flat spawn), so a tree traversal
  gets ordinary deque-to-deque work stealing. A spawn from a thread that is not a worker goes to
  the global queue, except from an adopted thread into a synchronously joined scope, which goes to
  that thread's own deque (see [Non-worker waits](#internals)). Every task record carries what a
  finisher needs: the address of its scope's counter and the scope creator's parker, both copied
  into the record at spawn time.
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
  deque, then the global queue, then steals, for `idleSpinRounds` rounds before it tries to sleep.

  A steal attempt takes from the global queue first, then probes at most
  `workerCount() + adoptableThreads - 1` victims (the other workers and the adopted slots), chosen
  uniformly with replacement by a per-worker generator seeded from the worker index (a
  minimal-standard linear congruential or xorshift generator, never `std::random_device`) with a
  multiply-shift range reduction. Finding nothing after the bound counts as finding nothing. The
  seeded generator also makes real-worker victim sequences reproducible per worker.
- **Sleeping.** Every worker sleeps on its own parker (below), whether it is idle or a waiting
  creator. The scheduler keeps an `awake` count of workers not asleep and a per-worker state word:
  awake, asleep shallow (asleep while within its margin, for example in its top-level idle loop) or
  asleep deep (asleep past its margin). Every transition from either asleep state to awake is one
  compare-and-swap on that word, and exactly the thread whose swap succeeds increments `awake`; a
  thread whose swap fails does not increment, because the thread that won already counted the
  worker. Every waker changes the worker's parker whether or not its swap succeeded, which at worst
  causes a spurious wake, and notifies it only if the parker's waiting bit was set (below). This
  rule covers every wake: a sleeper waking itself in step 4, a publisher, a finisher waking an
  asleep creator, the hand-off below, and shutdown. To sleep, a worker:

  1. drains its own deque into the global queue, removing entries with the owner's Chase-Lev
     `take` (including its compare-and-swap on the last element), never by reading the array;
  2. sets the waiting bit in its parker word with a sequentially consistent read-modify-write and
     keeps the value it returns;
  3. stores asleep shallow or asleep deep in its state word with a sequentially consistent store
     and decrements `awake` with a sequentially consistent read-modify-write. No standalone fence
     is used: these and the publisher's and destructor's sequentially consistent operations below
     are the two halves of each store-load pairing (the form Filament's JobSystem uses), which
     ThreadSanitizer models as ordinary atomics;
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
  5. otherwise waits on its parker for a change from the value kept in step 2. Whenever that wait
     returns, for any reason (a change, a change that happened before the wait began, or a
     spurious return), the worker first wakes itself with the swap on its own state word,
     incrementing `awake` only if the swap succeeds, clears its waiting bit, and only then does
     anything else (in every path out of steps 4 and 5 the bit is cleared the same way): a waiting
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

  A thread that publishes to the global queue pushes with a sequentially consistent
  compare-and-swap, then reads `awake` with a sequentially consistent load. If no worker is awake,
  tested as `awake <= 0` so that a count driven below zero by a defect still reads as no worker
  awake and leads to a wake rather than to silence, it picks an asleep worker, switches that
  worker's state to awake with a compare-and-swap, increments `awake` if its swap succeeded, and
  changes that worker's parker, notifying it if the waiting bit was set. It may also wake one
  worker when some are asleep and none is searching, for parallelism (see
  [Local pushes](#internals) for `searching`). The sleeper's decrement in step 3 and its step 4
  read of the global queue, and the publisher's push and its read of `awake`, are all
  sequentially consistent, so they fall in one total order, and at least one of them sees the
  other: either the sleeper sees the queued task in step 4, or the publisher sees `awake` at zero
  and wakes someone. A woken worker that is past its margin may go
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
  at least one worker is awake, or the task is on an adopted thread's deque and that thread is
  awake and will run or drain it. Worker deque entries exist only on awake workers, because a
  worker drains its deque before it sleeps and a worker that pushes is awake. Adopted-deque entries
  exist only while their adopted thread is awake, for the same two reasons: it pushes only while
  awake, and it drains its deque into the global queue, through the publish protocol, before it
  sleeps ([Non-worker waits](#internals)). Global-queue entries are covered by the sequentially
  consistent pairing above. An awake worker always makes progress: it runs a task, pops one by
  rule 1, helps, or tries to sleep; it cannot sleep while it is the last awake worker and work is
  queued. An adopted thread's entries all belong to synchronously joined scopes it is the creator
  of, or to their descendants that it pushed, so it reaches a wait on each of them, where rule 1
  runs them or the drain hands them to a worker. So queued work always has a runner, and the
  margin decides only which worker runs it. Adopted threads are never counted in `awake`, so a
  thread that is outside the scheduler doing its own work can never be the awake worker the
  invariant relies on. The wake after a local push (below) only adds parallelism; nothing in this
  argument uses it, so a missed or skipped one cannot leave work without a runner.

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
- **Local pushes.** A worker or adopted thread pushing onto its own deque skips the publish
  protocol, because it is awake and runs, loses to a thief, or drains that entry before it can
  sleep, so the invariant holds without it. It does not skip waking for parallelism: after the
  push it reads, with relaxed ordering, `awake` and a `searching` count, and if some worker is
  asleep and none is searching it wakes one asleep worker with the swap rule. A worker is
  searching from the moment a waker wakes it for parallelism, or it finds its own deque empty,
  until it runs a task or reaches sleep step 1; a searcher that finds a task and was the last
  searcher wakes one more asleep worker the same way, so a fork spreads one worker at a time
  without every push paying a wake. The reads are unfenced, so a wake can be missed; a missed wake
  costs parallelism only. Filament checks its sleeping count on every `put`, Rayon wakes sleepers
  when work is posted, and Taskflow keeps one thief awake while any worker is busy, for the same
  reason: without it, a fork from the one awake worker stays on that worker while the others
  sleep. The cost on the spawn path is two relaxed loads of lines written only on sleep, wake and
  search transitions, expected to fit the spawn goal and measured by S6. Pushes into the global
  queue (spawns from non-workers, drains, coroutine resumptions from outside a worker) always take
  the publish protocol.
- **Parkers and scope lifetime.** A scope lives in its creator's frame and is destroyed as soon as
  its creator sees it complete, so no thread may touch the scope after the change that lets the
  creator return. Every worker has a parker, an atomic epoch word, for the scheduler's lifetime;
  each other thread that creates a scope gets one from scheduler-owned stable storage. The rules:

  - A finishing task reads the creator's parker pointer from its task record, then decrements the
    scope's counter with release ordering. That decrement is its last access to the scope. If it
    reached zero, the finisher changes the parker (and, if the creator is an asleep worker, first
    swaps its state to awake, incrementing `awake` only if that swap succeeds), touching only the
    parker and the scheduler's own state, and notifies only if the value its change replaced had
    the waiting bit set, so a creator that is still running pays no system call. Filament's
    JobSystem keeps waiter bits beside its running count for the same reason. Whether
    `std::atomic::notify_one` elides the system call when nothing waits is implementation-defined,
    so the scheduler does not rely on it.
  - The parker is one `std::atomic<std::uint32_t>`, the width every platform's native wait uses:
    a waiting bit and a 31-bit epoch. Every change is a release read-modify-write that advances the
    epoch and returns the old word, and the sleeper's bit-set in step 2 is a sequentially
    consistent read-modify-write on the same word. Both are in the word's single modification
    order. A change ordered before the bit-set is read by it, with everything the waker wrote
    before the change, so the sleeper's step 4 re-check sees the completed scope or the queued
    work and it does not wait; a change ordered after the bit-set returns a word with the bit set,
    so the waker notifies. A notify is therefore never skipped for a thread that will wait. The
    existing lost-wakeup tests cover this and must be observed red with the bit check broken
    (notify skipped unconditionally).
  - The counter word is 64 bits, a 63-bit count and the coroutine-join flag in the top bit, and
    the constructor asserts that the scheduler's bound on live tasks (`workerCount() *
    dequeCapacity`, plus `maxExternalTasks`, plus the running tasks) is below 2^62, since the bound
    is known only at run time. A test spawns into one scope until `trySpawn` returns false and
    asserts the scope is not seen complete until every task ran (Filament shipped a defect where a
    parent with its maximum child count read as finished).
  - `co_await scope.join()` registers its coroutine handle in the scope and sets a flag in the
    counter word with a compare-and-swap loop. If the loop observes a count of zero,
    `await_suspend` returns false and the coroutine continues without suspending. Otherwise the
    finisher whose decrement reaches zero sees the flag in the value it replaced and enqueues the
    resumption (onto its own deque if it is a worker, otherwise into the global queue), rather
    than resuming inline, so a resumption never nests on the finisher's stack. The scope is not
    destroyed in between, because its creator is that suspended coroutine.
  - A creator about to sleep, worker or not, sets its parker's waiting bit and keeps the value,
    re-checks, and waits on that value, as in the sleep steps. A notify without a change would
    let `std::atomic::wait` block again, so every wake is a change of the parker; a creator woken by an event for an outer scope re-checks its
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
    before the main thread's thread-local cleanup, for example) leaves nothing dangling. A
    long-lived thread that uses many schedulers in turn would never look up a destroyed
    scheduler's generation again, so dropping closed entries only on lookup would keep every old
    pool until thread exit. Instead, whenever a thread registers with a scheduler (its first
    lookup for a new generation misses), it first sweeps its map and drops every entry whose pool
    is closed. A closed pool has no live scope, so none of its slots is in use, and dropping the
    entry releases the thread's reference. A thread therefore holds pools only for schedulers that
    were open at its last registration, plus the one it registers with. A scope that a coroutine
    created on that thread can still be live after the thread exits, if the coroutine was resumed
    elsewhere, and a finisher that read the parker before the slot was reused can still change it.
    Either way the slot's next owner gets one spurious wake: slot storage is stable while any
    reference to the pool lives, and every wake is followed by a re-check, so that is harmless.
    Storage is therefore bounded by the largest number of non-worker threads that hold the
    scheduler's scopes at the same time, and is freed when the scheduler and every thread that took
    a slot from it have released the pool.

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
  - One non-worker thread creating, using and destroying 1,000 schedulers in turn: after each
    registration the number of live `ParkerPool` objects (a test-only counter) is at most 2, and
    after the last scheduler is destroyed and the thread registers with a fresh one it is 1. Must
    be observed red with the sweep removed (the count grows with the iteration count).
  - A scheduler destroyed before a non-worker thread that used it exits: the thread waits on a
    scope, the scheduler is destroyed, then the thread exits; and a small test binary whose
    `main()` creates a scheduler, waits on a scope from the main thread and destroys the
    scheduler before returning, so the main thread's thread-local cleanup runs after it.
    Both run under AddressSanitizer with no use-after-free, and must be observed red with the
    thread-local entry holding a raw pointer to the scheduler's free list.
  - A stolen task that waits; `Join` stolen and not stolen; a spawn at the queue bound
    (`spawn` inline, `trySpawn` returning false); a non-worker scope with zero idle workers.
  - An adopted thread whose children are all stolen sleeps after its drain and is woken by the
    last finisher; an adopted thread that drains before sleeping while every worker is past its
    margin, whose drained entries the last awake worker runs; slot exhaustion falls back to the
    non-worker path; and an adopted thread waiting on its scope never runs a task of another
    scope queued at the same time, asserted by a per-scope run counter.
  - The local-push wake: with 4 workers asleep, a non-worker spawns one root that spawns 64
    children, each a fixed busy loop; at least two workers ran children, asserted by per-worker
    run counters, not by time. Must be observed red with the local-push wake removed.
  - Zero workers: `trySpawn` returns false and `spawn` runs inline, at runtime zero workers and in
    the compiled-out variant; in seeded mode `trySpawn` returns true.
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

  **Worker QoS.** On Apple platforms each worker sets its QoS class to the creating thread's
  (`pthread_get_qos_class_np` on the creator before creation, `pthread_set_qos_class_self_np` in
  the worker), and an adopted thread keeps its own; elsewhere workers inherit the creator's
  scheduling. This is not a task-priority feature: it keeps interactive work off efficiency
  cores, as Filament runs all its threads at one interactive class, and it keeps benchmarks from
  measuring efficiency cores by accident. `DefaultWorkerCount()` subtracts one for a caller that
  adopts and does not halve for simultaneous multithreading unless S6 measures that it helps.
- **Non-worker waits.** A thread that is not a worker and has not adopted a slot (the render
  worker holding document access, before it adopts, for example) is not counted in `awake` and
  never runs tasks while workers exist: its spawns go to the global queue with the publish
  protocol, and when it waits it re-checks its scope's counter and sleeps on its parker, which the
  finisher of its scope's last task changes. It therefore never runs another client's task while
  it holds a guard, and needs no way to tell its own tasks apart in the global queue. With zero
  workers, every spawn runs inline on the spawning thread as before.

  A thread may instead call `adopt()`, which claims one of `adoptableThreads` preallocated slots (a
  fixed deque and a parker, claimed by a compare-and-swap on a slot mask, as Filament's
  `JobSystem::adopt` does) and releases it when the returned object is destroyed. An adopted
  thread is still not a worker: it is not counted in `awake`, has no state word, never steals and
  never takes from the global queue. What changes is where its spawns go and what its waits run.
  Its spawns into a scope it joins synchronously go to its own deque, with the local-push wake, so
  it runs its first task with no wake on the critical path and idle workers steal the rest; its
  spawns into a coroutine-joined scope, and coroutine resumptions it enqueues, go to the global
  queue, because no watermark covers them. When it waits synchronously it applies rule 1 (pop its
  own entries above the scope's watermark) and rule 2 only, and then sleeps as a non-worker does,
  after first draining its deque into the global queue with the publish protocol so that its
  remaining entries have a worker. Every entry on its deque was pushed by itself, as a scope's
  creator or by a task of that scope it ran, so it runs only its own scope's tasks and their
  descendants, never another client's task while it holds document access. The stack-floor
  assertion applies to it where `adopt()` can read the thread's stack bounds from the platform
  (to be verified per platform in S1); rule 1 alone nests only the program's own fork-join
  structure on its stack. `adopt()` returns an empty object, and the thread keeps the plain
  non-worker path, when no slot is free, with zero workers, in seeded mode, or with threading
  compiled out. An adopted thread helping with any task, as a worker does, is not offered: its
  stack is not the scheduler's and the cross-client rule above would be lost; a thread that wants
  that should be a worker.
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
  flag with a sequentially consistent exchange, then wakes every worker with the same swap rule
  (the swap, then the parker change, whether or not the swap succeeded) and joins them. With no
  scope live, every worker is idle. A worker's step 3 store and its step 4 read of the stop flag
  are sequentially consistent, as are the destructor's exchange and its swap, so
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

- **Non-threaded builds** (the renderer's `wasm` configuration): these targets set
  `//donner/base:threading` to `false` ([Configuration](#configuration)), so the library compiles to
  the inline implementation: no thread, atomic wait or `<thread>` code is linked, `workerCount()`
  is 0, and every client runs its serial path through the same calls. An Emscripten build without
  `__EMSCRIPTEN_PTHREADS__` and with threading left on fails to compile.
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
  scheduler allocates comes out of that heap: worker stacks (2 workers at 512 KiB), deques (1,024
  entries each, 4 KiB per worker or adoptable slot, and `maxExternalTasks` lowered to 4,096 in this
  build), task slot blocks and the frame pool's retained bytes. Milestone S5 measures the
  editor's peak heap with the scheduler running and keeps the build's headroom contract; if it does not fit, the worker count
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

### Peer Benchmarks

The scheduler is measured against its peers, not only against its own targets.
`//donner/benchmarks/tasks:task_scheduler_peer_bench` (Google Benchmark, `-c opt`, perf lane
only, never a correctness test) runs every workload below on Donner and on each peer through a
thin adapter per library, in one Bazel build with one toolchain and one set of optimization flags.
Wall-clock goals are checked there, never in correctness tests. The acceptance thresholds apply to
the enabled mode (workers running); the disabled mode has its own threshold under W6.

**Peers.** Every peer is a benchmark-only dependency: a `dev_dependency` module or an
`http_archive` or `http_file` with an overlay build file, pinned to a release tag and
content-pinned by `sha256`, never a commit, and never linked into a shipped target (a dependency
audit test on the shipped targets enforces that).

| Peer | Pin | License | How it is built |
|------|-----|---------|-----------------|
| oneTBB (`task_group`, `parallel_for`, `task_arena`) | `v2023.2.0` | Apache-2.0 | registry module as a `dev_dependency`; exceptions disabled if it builds that way, otherwise `-fexceptions` on its own targets only (to verify in S6) |
| Taskflow (`Executor`, `Subflow` or `Runtime`, `for_each_index`) | `v4.1.0` | MIT | header-only archive with an overlay `cc_library`; its executor uses `try` and `catch`, so its targets add `-fexceptions` |
| enkiTS (`TaskScheduler`, `ITaskSet`) | `v1.12` | Zlib | archive with an overlay `cc_library` of `src/TaskScheduler.cpp` |
| Filament JobSystem (`runAndWait`, child jobs, `jobs::parallel_for`) | `v1.81.0` | Apache-2.0 | the needed `libs/utils` sources and headers fetched as individual files at the tag, each with `sha256`, and an overlay `cc_library` with tracing compiled out (the file list to verify in S6) |
| libfork (coroutine fork-join) | `v3.8.0` | MPL-2.0 (benchmark binary only, unmodified, not distributed) | header-only archive; coroutine workloads only |
| `std::thread` pool baseline | in tree | Donner's | one mutex, one condition variable, one shared FIFO; and a plain serial loop |
| Rayon | none | MIT or Apache-2.0 | Rust, another toolchain; cited, not benchmarked |

**Workloads.** Each runs on every peer that can express it.

- **W1, empty spawn and join.** One creator spawns K empty tasks (K = 1, 64 and 4,096) into one
  scope, or the peer's equivalent (`task_group::run`, child jobs, a subflow, a task set of size
  K), and joins; reports time per task. Includes the shape of Filament's
  `BM_JobSystemAsChildren4k`, and an unstolen and a stolen `Join` of two empty closures.
- **W2, `ParallelFor` over tiny bodies.** 1,000,000 iterations of a body costing about 5, 25 and
  100 ns, at grains 1, 16, 256 and 4,096 and the peer's automatic grain.
- **W3, recursive fork-join.** `fib(30)` with a serial cutoff of 0 and 12, and a balanced and an
  unbalanced (geometric, UTS-style) tree walk; closure and coroutine forms where the peer has
  coroutines (Donner `Task<T>`, libfork).
- **W4, uneven work.** A loop and a task set whose item cost is heavy-tailed (99 percent at 50 ns,
  1 percent at 50 us, fixed seed), measuring steal effectiveness.
- **W5, wake latency from idle.** After the pool has been idle for 10 ms, a thread that is not a
  pool worker submits one task that records a timestamp; reports submit-to-start latency,
  repeated 10,000 times with idle gaps; and the same from a worker's local push to a sleeping
  thief.
- **W6, single-thread overhead.** The W2 and W3 bodies with zero workers at run time, with
  threading compiled out, and at 1 worker, against a plain serial loop.
- **W7, scaling.** W2 (25 ns body), W3 and W8 at 1, 2, 4, 8 and all available cores (performance
  cores only on Apple silicon).
- **W8, a Donner-shaped style pass.** A synthetic tree with the element counts of the 0072
  baseline documents (622, 4,425 and 5,137 elements, and 200 and 1,000 for the threshold), a
  per-element busy body calibrated to 5 us and to 1 us, parents before children, subtrees below
  64 elements walked with an explicit heap stack and larger ones spawned with `trySpawn`, started
  by a thread that is not a pool worker. It needs no 0072 code: the tree and the body are
  synthetic. Donner runs it in two forms (plain non-worker waiter, adopted waiter), and each peer
  in its natural form (oneTBB `task_group` from an arena-attached thread, Filament from an adopted
  thread, Taskflow `corun`, enkiTS task sets with a dependency on the parent). Reports pass time
  against the serial walk.

**Metrics.** Nanoseconds per task (W1, W3); items per second (W2, W4); p50, p99 and maximum wake
latency (W5); speedup and scaling efficiency, speedup divided by thread count (W7, W8); overhead
relative to serial (W6); idle CPU, the process CPU time consumed during a 1 s idle window after a
burst, from `getrusage` (W5); peak resident memory per peer (W3 unbalanced).

**Method.** All peers and Donner build in one invocation at `-c opt` with the same compiler,
standard library and flags; peers that need exceptions get `-fexceptions` on their own targets
only, recorded with the results. Every peer uses the same thread count, and each pool is created
once per process outside the timed region. Each case warms up for at least 1 s, runs 15
repetitions and reports the median and median absolute deviation with the minimum and maximum;
peers are interleaved per repetition so thermal drift affects all of them alike. A quiet-machine
gate runs before and after: the run is rejected when the 1-minute load average exceeds 0.5 per
core, when another process used more than 5 percent of a core during the run, or when the median
of a fixed calibration loop moved by more than 3 percent between the two gates. On Linux the CPU
frequency governor is set to performance where permitted and the setting recorded; on macOS the
run records the power source and thermal state and scales on performance cores only. The sleep
primitive each peer actually uses on each platform is recorded beside its results.

**Platforms.** Apple silicon macOS, Linux AArch64 and Linux x86-64, each on a machine with at
least 8 cores, with the hardware model, kernel, compiler and every peer's pin recorded beside the
results. The editor's threaded Wasm build runs W1, W2, W5 and W8 for Donner only, in a
cross-origin isolated headless browser, as information, not gated.

**Acceptance.** Medians are compared per platform. The best peer is the fastest peer on that
platform and workload, excluding the `std::thread` baseline.

- W1, W2 at grains 1 to 16, and W3 with cutoff 0 (the tiny-task workloads): Donner is faster than
  the best peer, or within 5 percent of it with the reason recorded here.
- W2 at larger grains, W3 with cutoff 12, W4 and W7: Donner is within 10 percent of the best
  peer, and its scaling efficiency at 8 threads is within 10 points of the best peer's.
- W5: Donner's p50 is within 10 percent and its p99 within 25 percent of the best peer's, and its
  idle CPU is no higher than the best peer's plus 1 percent of one core.
- W6, the disabled mode: with zero workers at run time and with threading compiled out, Donner is
  within 2 percent of the plain serial loop; at 1 worker, within 10 percent.
- W8: Donner with an adopted waiter is no slower than the fastest peer's natural form; the
  non-worker form's result is recorded for 0072's threshold calibration.
- The absolute goals under [Goals](#goals) still hold.

**When a threshold is missed.** A miss is a finding, not a footnote. The design changes (a
protocol, a data structure or a default) until the threshold is met, or the miss is recorded under
[Open Questions](#open-questions) with the measured data, the suspected cause and what was tried.
Nothing is waived silently, and S6 is not done while a miss is in neither state. Peer numbers in
this document come only from these runs or from cited sources, labelled as the source's own
claim.

## Security / Privacy

The scheduler runs only code the process supplies; its untrusted-input exposure comes from
clients that size work from documents.

- **Bounded queues.** Queued tasks are capped: `dequeCapacity` per worker and adoptable slot, and
  `maxExternalTasks` for spawns from other threads; past a cap work runs inline, so a client that
  spawns per document element cannot exhaust memory through the queues.
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
  the scheduler tests and the deque stress test on both deque forms, and each client's parallel
  tests as that client lands (the first planned is 0072's
  `//donner/svg/components/style:style_parallel_tests`; the variant does not wait for it).
- **Both modes.** The default build runs threaded. The compiled-out variant
  ([Configuration](#configuration)) runs the scheduler tests, the renderer's tests and every
  client's tests with threading compiled out, and the link-time symbol check of
  [Renderer threading](#renderer-threading). Client differential tests assert bit-identical
  output between their serial and threaded runs.
- **No deadlines.** Tests assert ordering and counts; generous hang guards turn a deadlock into a
  named failure.

## Candidate Clients

This section lists only the clients this design already names. A survey of the rest of Donner for
further clients follows and will extend this list; nothing here waits for it.

- **Parallel style traversal** ([0072](0072-css_cascade.md) Part D): the first planned client,
  designed there.
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
  here (a deque, a sleep protocol, scopes and two awaitables) is small. They remain the yardstick:
  the [peer benchmarks](#peer-benchmarks) build them as benchmark-only dependencies, so the
  decision to build rather than adopt is held to measured thresholds.
- **Adopting Filament's JobSystem as is.** Fast and recently redesigned, but it has no coroutines,
  no submission from threads that are not adopted, no deterministic mode, unbounded helping and a
  null-on-exhaustion contract; see [Prior Art: Filament JobSystem](#prior-art-filament-jobsystem)
  for what is taken from it.
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

- **Ownership.** Recommendation: the application or renderer creates a scheduler and passes it by
  pointer, which keeps tests deterministic and Wasm pool sizing explicit; because threading is on
  by default, entry points given none use the shared default scheduler of
  [Configuration](#configuration), which is the one process-wide instance and is configurable
  before first use. Tests never use it.
- **Editor worker count.** Recommendation: start with 2 scheduler workers in the editor build
  (raising `PTHREAD_POOL_SIZE` from 2 to 4); with the render worker adopting a slot for the pass,
  that is 3 threads doing style. Revisit with W8 and Part D's measurements.
- **Defer the help margin.** Milestone S1 could ship the stack-floor assertion without
  `helpMarginBytes`, the shallow and deep sleep states and the hand-off: a waiting worker would
  help until the stack floor, as Filament, Rayon and oneTBB do without a margin, and the sleep
  protocol would have two states. That removes the hand-off, two liveness tests and the deep
  state, saving an estimated 1.5 days of S1. The cost is that clients whose tasks wait could no
  longer share a scheduler under the declaration rule of [Worker stacks](#internals) until the
  margin is reintroduced, and the reintroduction would revisit the sleep protocol and its proofs.
  No planned client waits inside tasks (style, filter bands and layer preparation do not).
  Recommendation: keep the margin as designed, because the API allows waiting tasks from the
  first release; take the saving only if the maintainer prefers to restrict waiting tasks until a
  client needs them.
- **Native Geode with threading compiled out.** Native Geode targets are incompatible with
  `threading = false` in Milestone S8. The Vulkan device polls fences on its owning thread and
  could be made compatible by converting its locks and the wgpu adapter's completion wait
  (estimated 1 to 2 days, not in the total); Metal cannot, because its completion handlers run on
  a queue Metal creates. Recommendation: ship S8 with the software renderer, the library's caches
  and `DocumentState`, and make Vulkan compatible only when a consumer asks for it.
- **Single-threaded contract with threading compiled out.** S8 turns the library's locks into
  no-ops in that configuration, so an embedder that calls Donner from several threads must
  serialize those calls itself, and `ThreadingMode::ConcurrentDom` fails with a release
  assertion. Recommendation: accept both and document them with the setting; the alternative of
  keeping real mutexes would keep a thread-runtime dependency and contradict the link-time check.
- **0061.** Recommendation: update 0061 to build its row bands on this scheduler when its pool
  milestone starts, rather than editing it in this change.
- **Memory ordering of the join handle.** A coroutine registering `co_await scope.join()` publishes
  its handle and sets a flag in the scope's counter with one compare-and-swap, and the finisher
  whose decrement reaches zero reads the handle and enqueues the resumption. As written, the
  finisher's decrement uses release ordering only, which does not acquire the handle that the
  registration published, so the read can race with the write when the two run on different
  workers. Recommendation: the registration's compare-and-swap publishes with release ordering
  after the handle is written, the completion path's decrement is acquire-release, and Milestone
  S3's ThreadSanitizer tests cover registration racing completion. To be settled when S3 is
  implemented, together with a review of every other ordering in the protocol against the same
  rule: any thread that reads state written by another thread acquires it.
