# Design: Work-Stealing Task Scheduler

**Status:** Draft
**Author:** Claude Fable 5.2
**Drafted by:** Claude Opus 5.5
**Requested by:** Jeff McGlynn
**Created:** 2026-11-23
**Tracking:** [#2697](https://github.com/jwmcglynn/donner/issues/2697); [#2660](https://github.com/jwmcglynn/donner/issues/2660) (first client)

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
  `svg::Renderer` stays synchronous, Geode may fan CPU-only prepare work out to the scheduler, and
  the editor's render worker stays a dedicated thread with an inline mode.
- Best in class, and benchmarked: measured against named peer schedulers (oneTBB, Taskflow,
  enkiTS, Google Filament's JobSystem, libfork) with acceptance thresholds stated against the best
  peer.

Its first planned client is parallel style traversal, Part D of
[0072-css_cascade](0072-css_cascade.md). The scheduler is implemented before that work and no
milestone here depends on a 0072 milestone. Geode geometry prefetch, path-data parsing on load and
image decoding are ranked as the next clients ([Candidate Clients](#candidate-clients)) so the API
is not shaped by one user.

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
  serial loop. With threading compiled out, Donner's own object files in the default library
  configuration and in the shipped plain Wasm module reference no thread, pthread,
  condition-variable, once, static-guard or atomic-wait symbol, checked by a symbol check over
  Donner's object files; native Geode needs driver threads and is excluded
  ([Renderer threading](#renderer-threading)).
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

- [ ] Milestone S1: Core scheduler (9.5 days)
  - [ ] The base scheduler as first estimated (7 days): Chase-Lev deque with the memory orders of
        Le et al., per-worker deques, the global queue, stealing, help-while-waiting joins with
        scope watermarks, parkers, drain before sleep, the last-awake sleep rule and its publish
        protocol, the stack-floor assertion, `trySpawn`, cross-thread slot return, `TaskScope`
        and `Join`, zero-worker inline mode.
  - [ ] Changes since, each with its cost: adopted threads with the adopted publish, the
        visibility reads of adopted deques and the lifetime assertions (1.75 days); the
        parallelism wake with per-worker `searching` flags (0.5 day); worker QoS and
        `DefaultWorkerCount()` (0.25 day); the sleep and publish handshakes in read-modify-write
        form and the parker waiting bit (0.25 day); per-creator record pools and their bounds
        (0.25 day); `AsyncTaskScope` as a separate type (0.25 day); fixed-capacity deques instead
        of growable ones (-0.75 day). The mutex-protected global queue costs no more than the
        lock-free list it replaces.
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
  - [ ] `Task<T>`, `TaskScheduler::schedule()` and the `AsyncTaskScope` join awaitable, frame
        allocation from the scheduler's pool.
  - Done when: coroutine tests at 0, 1 and 4 workers, including resumption from a non-worker
    thread, registration racing completion, and nested awaits, pass under ThreadSanitizer.
- [ ] Milestone S4: ThreadSanitizer target and fuzzer (3.25 days). After S1.
  - [ ] A ThreadSanitizer test variant reachable from `//...` through a configuration transition,
        and `//donner/base/tasks:task_scheduler_fuzzer`.
  - [ ] The deque's sequentially consistent form for ThreadSanitizer builds, the stress test on
        both forms, and a red run on a deliberately weakened ordering (0.25 day). Verify here
        whether the repository's Clang ThreadSanitizer models standalone fences; the protocol does
        not depend on the answer, but the record of it decides whether the second form stays.
  - Done when: the variant runs the scheduler tests in normal CI and the fuzzer is green.
- [ ] Milestone S5: WebAssembly (4 days). After S1.
  - [ ] The inline path for Emscripten without pthreads; pool sizing, worker stack size, start-up
        and isolation probe for the editor build; size and memory measurements.
  - Done when: both Wasm builds pass their size tests, an editor smoke test records the worker
    count it obtained and the peak heap with the scheduler running, the fixed memory still has
    the headroom its build contract requires, and a test shows scheduler workers' sleeps take the
    pass-through branch of the editor's wrapped futex wait.
- [ ] Milestone S6: Peer benchmarks (6.25 days). After S2 and S3.
  - [ ] Benchmark-only pinned modules for oneTBB, Taskflow, enkiTS, Filament JobSystem and
        libfork (1.5 days); adapters and workloads W1 to W8 (2 days); the quiet-machine gate,
        one process per peer, the confirmation rule and the reporting script (1.25 days); runs on
        the three native platforms and the editor Wasm build, with results recorded in this
        document (1.5 days).
  - [ ] Verify here, before relying on them: that oneTBB builds with exceptions disabled (or
        needs `-fexceptions` on its own targets), that its registry module builds on all three
        platforms, that the Filament `libs/utils` subset needs no more files than listed, and that
        each peer builds under the repository's toolchains at all. A peer that cannot be built is
        recorded here with the reason, not dropped silently.
  - Done when: every acceptance threshold under [Peer benchmarks](#peer-benchmarks) is met or is
    recorded as an open question with its measured data, and the overhead goals are met. Tuning
    work to close a gap is not in this estimate.
- [ ] Milestone S7: Configuration and threading switch (2.25 days). After S1; the build setting
      itself can land first.
  - [ ] `//donner/base:threading` with its `donner.configure(threading = ...)` attribute, the
        CMake option `DONNER_THREADING`, the scheduler's inline implementation selected by it, and
        the preprocessor rule for Emscripten without pthreads (0.75 day).
  - [ ] The compiled-out test variant through a configuration transition, added to
        `VARIANT_SETTINGS`, with the scheduler tests (0.5 day).
  - [ ] `DefaultTaskScheduler()` and its configure and shutdown calls, and the `--threads`
        option of `donner-cli` (0.75 day).
  - [ ] Document the setting, the module attribute, the CMake option, the runtime option and the
        breaking changes of [Renderer threading](#renderer-threading) in `docs/building.md` and
        `docs/multithreading.md` (0.25 day).
  - Done when: the compiled-out variant and the default (threaded) build both run the scheduler
    tests in CI, and a consumer-module build with `threading = False` builds `@donner//:donner`.
- [ ] Milestone S8: Renderer threading and the render worker (11.5 days). After S5 and S7.
  - [ ] The Donner lock type and the conversions in the software path (`PackagedResource`,
        `SubDocumentAdmission`, `CatalogEncodedFontStore`, `SystemFontProvider`'s once flag),
        `-fno-threadsafe-statics` for Donner's sources when compiled out, and
        `ThreadingMode::ConcurrentDom` rejected when compiled out (1.5 days).
  - [ ] `svg::RendererOptions` replacing the two `Renderer` constructors, with the scheduler
        field (0.5 day).
  - [ ] `AsyncRenderer` renamed `editor::RenderWorker` with `RenderWorkerOptions`, across the
        editor, the editor-control tool, the replay tool and the tests (1.5 days).
  - [ ] The shared iteration function, `Inline` mode, `runIdleWork` slices, and the
        style-source annotation parse run inline when compiled out (4.5 days).
  - [ ] The render-worker tests parameterised by mode (preemption tests in `Dedicated` only),
        replay pixel goldens identical across modes, and a ThreadSanitizer run of the render worker
        in `Dedicated` mode with scheduler workers (2 days).
  - [ ] Native Geode and the Geode editor marked incompatible with the setting, and the symbol
        check of [Renderer threading](#renderer-threading) over Donner's own object files (1 day).
  - [ ] The plain Wasm module's target-level transition to `threading = false`, registered in
        `build_defs/check_variant_transitions.py` because it writes a `VARIANT_SETTINGS` member,
        and the symbol check run on it in the Wasm CI job (0.5 day).
  - Done when: the compiled-out variant renders the renderer's golden tests and the editor's
    replay goldens bit-identically to the threaded build, and the symbol check is green on both
    the native compiled-out variant and the plain Wasm module, and was observed red with one
    `std::mutex` reinstated.
- [ ] Follow-on, not in the committed total: Milestone S9, Geode browser module compiled out
      (2.5 days). After S8. The Geode common and browser-bridge locks of
      [Renderer threading](#renderer-threading) converted, and the symbol check extended to the
      `wasm-geode` module. Whether it ships with the first release is an open question.
- [ ] Release gate, before the first client ships with workers enabled (no estimate; checks):
  - [ ] Every peer-benchmark threshold met, or recorded as an open question the maintainer
        accepted with its data.
  - [ ] The ThreadSanitizer, AddressSanitizer and UndefinedBehaviorSanitizer lanes clean on the
        scheduler and on that client.
  - [ ] The default (threaded) build, the runtime zero-worker runs and the compiled-out variant
        all green in CI, the editor's replay goldens identical in `Dedicated` and `Inline`, and
        the symbol check green on the native compiled-out variant and the shipped plain Wasm
        module.
  - [ ] Bit-identical output between serial and threaded runs for every client that ships,
        asserted by that client's differential tests at 0, 1 and N workers.

| Milestone | Days |
| --------- | ---: |
| S1 Core scheduler | 9.5 |
| S2 Parallel loops and determinism | 3.5 |
| S3 Coroutines | 3 |
| S4 ThreadSanitizer target and fuzzer | 3.25 |
| S5 WebAssembly | 4 |
| S6 Peer benchmarks | 6.25 |
| S7 Configuration and threading switch | 2.25 |
| S8 Renderer threading and the render worker | 11.5 |
| **Committed total** | **43.25** |
| S9 Geode browser module compiled out (follow-on) | 2.5 |

Total committed estimate: 43.25 engineering days; 45.75 with S9. The Geode prepare fan-out
(Candidate client 1) is estimated with its own design, not here. Part D of
[0072-css_cascade](0072-css_cascade.md) needs S1 to S3, and S4 for its race coverage; its release
with workers on needs the release gate.

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
  (`JobSystem::adopt`); here the adopted thread runs only its own deque's entries and its pushes
  publish (see [Adopted threads](#internals)).
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
enum class WorkerQos { kInteractive, kUserInitiated, kDefault };  // Apple QoS; ignored elsewhere.

struct TaskSchedulerOptions {
  std::size_t workerCount = DefaultWorkerCount(); // 0: run everything inline on the caller.
  std::optional<std::uint64_t> seed;         // Seeded deterministic mode (tests).
  std::size_t dequeCapacity = 4096;          // Entries per deque; a power of two.
  std::size_t maxTasksPerCreator = 4096;     // Live task records per worker or adopted slot.
  std::size_t maxExternalTasks = 64 * 1024;  // Live task records for all other threads together.
  std::size_t adoptableThreads = 1;          // Slots reserved for adopt(); fixed at construction.
  std::size_t helpMarginBytes = 128 * 1024;  // Stack kept free when stealing while waiting.
  std::size_t idleSpinRounds = 1;            // Steal rounds before sleeping; set from S6's W5.
  WorkerQos workerQos = WorkerQos::kInteractive;
};

// Hardware threads available to the process (the affinity mask on Linux, the active processor
// count elsewhere) minus one for the adopting caller, at least 1 and at most 32. 0 when threading
// is compiled out.
std::size_t DefaultWorkerCount();

// The process-wide scheduler that Donner entry points use when given none. Created with the
// configured (or default) options on first use and kept until ShutdownDefaultTaskScheduler() or
// process exit. At exit it is intentionally leaked, never destroyed by a static destructor: its
// workers stay parked and it stays reachable from a static pointer, so LeakSanitizer does not
// report it. A program that wants a clean exit calls ShutdownDefaultTaskScheduler().
TaskScheduler& DefaultTaskScheduler();       // Release-asserts after shutdown; no re-creation.
bool ConfigureDefaultTaskScheduler(TaskSchedulerOptions options); // False once it exists.
void ShutdownDefaultTaskScheduler();         // Joins its workers; asserts no scope is live.

class TaskScheduler {
public:
  explicit TaskScheduler(TaskSchedulerOptions options);
  ~TaskScheduler();      // Joins every worker; asserts no scope and no AdoptedThread is live.
  std::size_t workerCount() const;           // Workers actually started (may be fewer).
  // True when threading is compiled out or the options asked for zero workers; false when workers
  // were requested, even if none started. Clients use it to tell a configured disable apart from
  // failed starts.
  bool threadingDisabledByConfiguration() const;
  ScheduleAwaitable schedule();              // co_await: continue on a worker.
  // Gives the calling thread a slot for the lifetime of the returned object (see Adopted
  // threads). Empty when no slot is free, with zero workers, in seeded mode, and with threading
  // compiled out. Asserts the thread is not a worker and has no live TaskScope.
  [[nodiscard]] AdoptedThread adopt();
};

class TaskScope {        // Joined synchronously (wait() or destructor) on its creating thread.
public:
  explicit TaskScope(TaskScheduler& scheduler);
  ~TaskScope();                              // Joins.
  template <typename F> void spawn(F&& work);   // From the creator or any task of this scope.
  template <typename F> bool trySpawn(F&& work); // False at a bound; caller keeps work.
  void wait();                               // Runs queued work while waiting.
  void requestStop();                        // Cooperative cancellation.
  bool stopRequested() const;
};

class AsyncTaskScope {   // Joined by a coroutine with co_await join().
public:
  explicit AsyncTaskScope(TaskScheduler& scheduler);
  ~AsyncTaskScope();                         // Asserts it was joined and its count is zero.
  template <typename F> void spawn(F&& work);
  template <typename F> bool trySpawn(F&& work);
  JoinAwaitable join();                      // co_await: resume when every spawn finished.
  void requestStop();
  bool stopRequested() const;
};

template <typename A, typename B> void Join(TaskScheduler& scheduler, A&& a, B&& b);
template <typename F>
void ParallelFor(TaskScheduler& scheduler, std::size_t begin, std::size_t end,
                 std::size_t grain, F&& body);  // body(std::size_t first, std::size_t last)

template <typename T> class Task;             // Lazily started coroutine; co_await yields T.
```

- **Two scope types.** How a scope is joined is fixed when it is constructed, because where its
  spawns may go and whether it has a watermark depend on it (below). `TaskScope` is joined on the
  thread that created it, which it asserts; `AsyncTaskScope` is joined only by `co_await join()`
  and has no synchronous wait. A coroutine that wants a synchronous scope uses `TaskScope` and
  must not suspend while it is live; if it resumes elsewhere and waits there, the creating-thread
  assertion fails.
- **Join** on a worker or an adopted thread pushes `b` to that thread's deque, runs `a` itself,
  then takes `b` back and runs it if no one stole it, as Rayon's `join` does; the work-first
  principle, so an unstolen join costs about two calls and a deque push and pop. If `b` was
  stolen, the caller waits as a scope's creator does (below). On any other thread, `Join` spawns
  `b` through the global queue, runs `a`, and then waits for `b` as a non-worker scope wait does;
  it cannot take `b` back.
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
  as it does at a bound, instead of recursing once per spawned level. `Join` runs `a` then `b`;
  `ParallelFor` runs one `body(begin, end)`. This holds whether zero workers came from the runtime
  option, from workers that failed to start, or from threading compiled out. Seeded mode is not
  zero-worker mode: its tasks are queued and run on the calling thread in seeded order, so
  `trySpawn` there returns true until the calling thread's `maxTasksPerCreator` records are live,
  and false after that.
- **Coroutines.** `co_await scheduler.schedule()` enqueues the coroutine handle and suspends; a
  worker resumes it. With zero workers, or in seeded mode, `schedule()` is ready immediately and
  the coroutine continues inline on the calling thread, the same rule every other operation
  follows at zero workers. `co_await scope.join()` on an `AsyncTaskScope` suspends until the
  scope's counter reaches zero (or does not suspend if it already has), and the last finishing
  task enqueues the awaiting coroutine's resumption. `Task<T>` is lazily started and resumes its
  awaiter by symmetric transfer, so chains of awaits do not grow the native stack. A coroutine's
  promise calls `std::abort()` from `unhandled_exception`, which cannot be reached in a
  `-fno-exceptions` build.
- **Errors and cancellation.** Work returns `void`; results and errors are written to
  caller-owned slots, or returned as `Task<ParseResult<T>>`. `requestStop()` sets a flag that
  queued tasks check before running and long tasks poll; stopped tasks still complete their scope
  accounting, so `wait()` always returns.

### Configuration

Multithreading is on by default. Two controls change that, one at run time and one at build time.

- **Runtime option.** `TaskSchedulerOptions::workerCount` sets the worker count; 0 disables
  workers, and every client then runs serially on the calling thread with identical results (the
  zero-worker rules above). Donner entry points that run a parallel client take a
  `tasks::TaskScheduler*` in their options (`svg::RendererOptions::scheduler`,
  `editor::RenderWorkerOptions::scheduler`, below); null means `DefaultTaskScheduler()`. To turn
  threading off for a whole process at run time, an embedder calls
  `ConfigureDefaultTaskScheduler({.workerCount = 0})` before first use, and the editor's render
  worker then runs inline ([Renderer threading](#renderer-threading)). The default scheduler lives
  until `ShutdownDefaultTaskScheduler()` or exit, so serial library calls do not create and join
  threads per call. Its workers take the QoS class in `workerQos`, not the class of whichever
  thread happened to create it first. Tests always pass their own scheduler, so their worker counts
  are explicit, and never use the default.
- **Build setting.** One Donner-wide setting, `//donner/base:threading`, compiles threading out of
  the scheduler and the renderer together ([Renderer threading](#renderer-threading)). It follows
  the convention of the existing renderer settings (`renderer_backend`, `filters`, `text`): a
  public `bool_flag` whose default comes from the `donner_config` repository that the `donner`
  module extension writes, with a `config_setting`, `//donner/base:threading_disabled`, that
  build rules select on.
  - Values: `true` (the default, threading compiled in) and `false`.
  - What `false` does: the scheduler API stays source-compatible, and `TaskScheduler`, both scope
    types, `Join`, `ParallelFor`, `Task<T>` and `adopt()` compile to the inline implementation.
    `workerCount()` and `DefaultWorkerCount()` are 0, `adopt()` returns an empty object, every
    client takes its serial path, and the renderer changes as listed under
    [Renderer threading](#renderer-threading). Donner's own sources also compile with
    `-fno-threadsafe-statics`, so function-local statics take no guard calls.
  - How a consumer module sets it: on the command line,
    `--@donner//donner/base:threading=false`; or in its `MODULE.bazel`, through the existing
    extension, `donner.configure(threading = False)`, which only the root module's tag sets, as
    for the other attributes. Inside this repository it is `--//donner/base:threading=false`.
  - Relation to the runtime option: compiling out wins. The runtime option is accepted and
    ignored, and the scheduler reports zero workers.
  - Emscripten without pthreads: one macro, `DONNER_THREADING_ENABLED`, is false when the
    setting is `false` or when an Emscripten build lacks `__EMSCRIPTEN_PTHREADS__`. The scheduler,
    the Donner lock type and the once flag all key on it, so every existing downstream non-pthread
    Emscripten consumer keeps building with no change and no error, and gets the inline scheduler
    and no-op locks. Only `-fno-threadsafe-statics`, a compiler flag, needs the setting itself;
    such a consumer keeps Emscripten's guard calls, which are stubs without pthreads, unless it
    sets `threading = False`.
  - The shipped plain Wasm module sets the flag itself: `//donner/svg/renderer/wasm:donner_wasm`
    builds its binary through a target-level transition to `threading = false`, so it gets the
    full compiled-out form, including `-fno-threadsafe-statics`. Setting the flag in `.bazelrc`
    was rejected: the Wasm builds are configured by `common:wasm` flags, and `common:editor-wasm`
    inherits them through `--config=wasm-geode`, so `threading=false` there would also compile
    threading out of the threaded editor. The editor's Wasm module keeps the default.
  - Targets that cannot work with the flag off are marked incompatible with
    `//donner/base:threading_disabled` through `target_compatible_with`, so a broad build skips
    them instead of failing: native Geode (Metal and Vulkan call back on driver threads) and so
    the editor built with Geode. The editor built with tiny-skia stays buildable, with an inline
    render worker.
  - CMake: the Bazel-to-CMake generator, which today maps `DONNER_TEXT`, `DONNER_TEXT_FULL` and
    `DONNER_FILTERS` and has no thread handling, gains `option(DONNER_THREADING ... ON)` and links
    `Threads::Threads` only when it is on. The CMake mirror builds the libraries only, which is
    what the setting covers.
  - CI: the default build runs threaded. One test variant, a configuration transition in the
    single native Bazel invocation, with the setting added to `VARIANT_SETTINGS` in
    `build_defs/variant_settings.bzl` (the reset list), builds and tests with threading compiled
    out and runs the native symbol check. The Wasm half does not run there: the plain module needs
    the `common:wasm` flags (`enable_wasm`, `renderer_backend=tiny_skia`, `text=false`), which only
    `--config=wasm` sets, so the existing Wasm CI job, which builds with `--config=wasm`, runs the
    symbol check on the module it ships. The W6 compiled-out benchmark binary is built through the
    native transition, so the peer benchmarks still build in one invocation.
- **Defaults per shipped target.**
  - Library: `DefaultWorkerCount()` workers in the default scheduler, created on first use.
  - `donner-cli`: the default scheduler, with a `--threads=N` option (`0` serial).
  - Editor, native: `DefaultWorkerCount()` minus one, at least 1, because the UI thread also
    runs; the render worker is a dedicated thread, not counted, and adopts a slot for the style
    pass. An editor setting changes the count.
  - Editor, threaded Wasm: 2 workers (raising `PTHREAD_POOL_SIZE` from 2 to 4), dropping to the
    workers that started; see [WebAssembly](#webassembly).
  - Renderer Wasm without pthreads: `threading = false` through its own transition, 0 workers.

The scheduler's tests run in both modes: every scheduler test at 0, 1 and 4 workers and in
seeded mode in the default build, and again in the compiled-out variant, where they run at zero
workers and assert `workerCount() == 0` whatever the options asked for. Each client's
differential tests compare serial and threaded output in the default build and run serially in
the compiled-out variant.

### Renderer Threading

The same build setting and the same runtime configuration govern the renderer. This section is
the recommended design; the decisions it leaves to the maintainer are under
[Open Questions](#open-questions). The maintainer has decided that the tiny-skia backend stays
single threaded inside and that the renderer's multithreading is focused on Geode.

**`svg::Renderer` stays synchronous.** It keeps its contract: one thread at a time, everything on
the calling thread. A new `svg::RendererOptions` replaces its two constructors (today
`Renderer(bool verbose)` and `Renderer(std::shared_ptr<geode::GeodeDevice>, bool verbose)`) and
adds `tasks::TaskScheduler* scheduler`. Given workers, the Geode backend may spread CPU-only
prepare work inside one `draw` over the scheduler: the static, pure
`GeodePathEncoder::encode(const Path&, FillRule, double, Limits)`, `Path::strokeToFill` and clip
encodes. Tasks take `const` inputs captured before the fork and return values; the owning thread
joins them, then does every cache insertion, budget charge, slot allocation, buffer write, command
recording and submission in one ordered stream. That keeps the ordering invariant 0061 records as
"Do not parallelize Geode command encoding" (parameter buffers written ahead of the draws in one
submit, so the last write to a slot wins for every draw). The fan-out itself is Candidate client 1
below, and its design belongs with 0061. With zero workers or threading compiled out, the prepare
work runs inline in the same order. No public renderer API changes behavior.

**The editor's render worker stays a dedicated thread.** `AsyncRenderer`
(`donner/editor/AsyncRenderer.{h,cc}`, 1,419 and 2,467 lines) is the editor's render pipeline, not
a wrapper around `svg::Renderer`: it owns the compositor, drag previews, overlay capture, tile
publication, a thumbnail lane, compositor warmup and GPU idle maintenance. It is renamed
`editor::RenderWorker`, takes a `RenderWorkerOptions` (a `RenderThreadMode` and a scheduler), and
is not a scheduler task and not counted in `workerCount()`, because:

- it owns a GPU device used from one thread at a time, which in the browser is pinned to the
  thread that created it (`docs/gpu_runtime.md`); on Emscripten its worker loop builds its own
  `svg::Renderer` for that reason;
- it holds `DocumentWriteAccess` for a whole render, which tasks may never acquire;
- it blocks in browser GPU-owner dispatch and can suspend through Asyncify, which tasks may not;
- it runs for a whole frame, cancellable only at compositor safe points;
- its waits must never pick up another client's task while it holds the document guard;
- it keeps compositor and device state across frames.

It is a client of the scheduler from outside: it adopts a slot when it starts, so the passes it
forks (the style pass, the Geode prefetch) run their first task on it, and it spawns and waits as
a non-worker when no slot is available.

**Inline mode.** `RenderThreadMode` is `Dedicated` or `Inline`. Both run one iteration function,
extracted from today's worker loop. `Inline` is forced when threading is compiled out and is
selected when threading is disabled at run time by configuration, which `RenderWorker` reads from
its scheduler's `threadingDisabledByConfiguration()` at construction (an explicit
`RenderWorkerOptions::mode` overrides it). Zero workers for any other
reason (workers that failed to start, for example in the threaded editor Wasm build) leaves the
render worker `Dedicated`: it is still a thread and still works, and its forks run inline. In the
threaded editor Wasm build the render worker is always `Dedicated`, even when threading is
disabled at run time, until the open question on `Inline` there is settled. In `Inline` mode
`requestRender` runs the foreground iteration on the caller's thread before it returns and stages
the result for `pollResult`; `isBusy()` is false whenever control is with the caller; the
thumbnail and warmup lanes run in bounded slices from a new `runIdleWork(budget)`, which still
polls the cancellation token between slices; supersede and `cancelInFlight` have nothing to
act on, and `cancelledRenderCount()` stays 0. The document stays in
`ThreadingMode::SingleThreaded`, since the guard is taken and released on the caller's thread. The
cost of disabling threads is that the UI thread is blocked for a whole render. This changes
`RenderWorker`'s public behavior in that mode (requests complete synchronously), which is the one
asynchronous-by-contract renderer API in the tree; it is flagged as a decision under Open
Questions. No API in `donner/svg/renderer` is asynchronous by contract.

**Inventory, and what `threading = false` does.** Read from the tree at `origin/main`:

| Site | Today | With threading compiled out |
| ---- | ----- | --------------------------- |
| Scheduler | workers | inline implementation |
| `AsyncRenderer` `std::thread` | render worker | `RenderWorker` in `Inline`; no thread compiled |
| `EditorShell` `std::async` | detached style-source annotation parse | runs inline at the request point |
| `DocumentState` | `ConcurrentDom` mutex and condition variables | removed; selecting `ConcurrentDom` fails a release assertion |
| `SystemFontProvider` | `std::call_once` (`pthread_once` on libstdc++) | a plain once flag |
| `PackagedResource`, `SubDocumentAdmission`, `CatalogEncodedFontStore` | mutexes on process-wide state | the Donner lock type, a no-op |
| Function-local statics | guard calls | none (`-fno-threadsafe-statics`) |
| Relaxed counters, `thread_local` roles, `CompositorController` cancel flag | atomics without wait | unchanged |
| Geode common: `RendererGeode` texture-pool registry, `HeadlessDevicePool`, `GeodeFilterEngine`, `GeodeHandleRetirement`, `GeodeDevice` (a mutex and a `timed_mutex`); `gpu/DeviceLost`, `gpu/TextureExport.h`, `gpu/shader/LinkedProjection.cc` | mutexes | the lock type, in Milestone S9 |
| Browser GPU bridge: `BrowserGpuOwner` (futex wait and wake, `pthread_self`; partly under `__EMSCRIPTEN_PTHREADS__` today), `EmscriptenBrowserBridge` and `BrowserShareReleaseQueue` mutexes | GPU-owner dispatch | the lock type, in Milestone S9 |
| `GeodeWgpuAdapterDevice` | condition variable for completion callbacks | native Geode: incompatible |
| Metal | completion handlers on a queue Metal creates | incompatible: the driver owns that thread |
| Vulkan: `VulkanDevice` (debug messenger "may invoke the callback from any thread", queue and execution mutexes), `VulkanResourceState`, `VulkanSwapchain`, `VulkanQueueProgress` | mutexes, driver callbacks | incompatible: the driver owns that thread |
| Editor Wasm: `PROXY_TO_PTHREAD`, GPU-owner pthread, proxy queues | browser architecture | no threadless editor Wasm build (not a goal now) |
| Wasm owner-wait and abort probes (`pthread_create`, `std::thread`) | test probes | not in compiled-out targets |
| `EditorShell` catalog font wake target | a mutex (`catalogFontWakeTarget_->mutex`) | unchanged; the editor is not in the symbol check |
| `NativeMenuMac.h` | `pendingMutex_` (native macOS editor) | unchanged; the editor is not in the symbol check |

With threading compiled out, the threading library dependency is gone from the scheduler, the
library's caches, `DocumentState` and the render worker. A symbol check verifies it, scoped to
Donner's own object files: an aspect over the target collects the object files of `cc_library`
targets whose labels are in this repository's `//donner/...` packages, and the check lists their
undefined symbols. Objects of third-party targets (external repositories and `//third_party`)
and of the toolchain's standard and runtime libraries are not collected, so what they link is
outside the check; the full-text tier's HarfBuzz, for example, is built with thread support. It
runs on `@donner//:donner` in its default configuration (tiny-skia, base text, filters) on Linux
and macOS in the native compiled-out variant, and on the shipped plain Wasm module in the Wasm CI
job. No collected object may reference `pthread_create`, `pthread_once`, `pthread_cond_*`,
`pthread_mutex_*`, `std::thread`, `std::condition_variable`, `__cxa_guard_acquire`, the standard
libraries' atomic wait and notify entry points, or `emscripten_futex_wait` and
`emscripten_futex_wake`. The editor is not in the check: even built with tiny-skia it needs the
full text tier and links GLFW, both of which keep threads, and two of its own UI mutexes (listed
above) stay. With threading off it is checked by its tests, the render worker running `Inline`.
Milestone S8 lands this with the scheduler's first release. Milestone S9 converts the Geode
common and browser-bridge locks so the Geode browser renderer module (`wasm-geode`) passes the
same check; native Geode cannot, because its drivers create threads.

### Internals

**Terms.** A *worker* is a thread the scheduler created. A worker is *awake* when its state word
(below) says awake; `awake` is the count of those. An *adopted thread* is a thread that is not a
worker and holds a slot from `adopt()`; it has a deque and a parker, no state word, and the word
awake does not apply to it. A *non-worker* is any other thread. A *task* is a spawned closure in a
task record, or a coroutine resumption; it is *queued* while it is on a deque or in the global
queue, and *started* once a thread begins running it.

- **Deques.** Each worker and each adoptable slot owns a Chase-Lev deque with signed 64-bit
  logical indices that only grow and never wrap or reset (the array slot is the index modulo the
  capacity), so watermarks stay comparable for the life of the scheduler: the owner pushes and pops
  at the bottom, thieves take from the top, and the last element is claimed by a compare-and-swap
  on the top index. The memory orders are the C11 ones of Le et al.'s Figure 1 (relaxed and
  release-acquire accesses, sequentially consistent fences in `take` and `steal`, sequentially
  consistent compare-and-swap), whose ARM counterpart they prove correct, not a port of the
  original pseudo-code; slots are relaxed atomics so a thief's read of a slot the owner is
  rewriting is not a data race. Each deque is a fixed ring of `dequeCapacity` entries (a power of
  two; 4,096 by default, 1,024 in the editor Wasm build), allocated once and never resized, as in
  Filament's `WorkStealingDequeue`. When a push of a task record would exceed it, `spawn` runs the
  task inline and `trySpawn` returns false; a resumption that finds its deque full goes to the
  global queue instead, since it cannot run inline. There is no growth path and so no old array a
  thief could still be reading. Builds with ThreadSanitizer compile the deque with every index
  access sequentially consistent and no fences (Filament's deque uses sequentially consistent
  indices over a plain item array), because whether ThreadSanitizer models standalone fences is to
  be verified in Milestone S4; the deque stress test runs on both forms, and the production form is
  the Le et al. one.
- **Global queue.** One intrusive first-in, first-out list linked through the task records (and,
  for coroutine resumptions, through the awaiter in the suspended frame), protected by a mutex, so
  pushing needs no allocation and there is no ABA or reclamation question: an entry is owned by
  the queue from push to pop, and its record or frame cannot be freed while it is linked. Beside
  the list is `globalQueued`, a count changed under the mutex with sequentially consistent
  read-modify-writes, which the sleep and publish handshake reads without the mutex. The queue is
  off the fast path: it receives spawns from non-workers, coroutine resumptions enqueued by
  non-workers and adopted threads, resumptions that find a full deque, and drains. There are no
  per-scope queues: a queue that belongs to a scope would expose the scope's memory to idle workers
  looking for work while the scope's creator completes and destroys it. Fairness: entries leave in
  push order, and a worker with work on its own deque still makes one outside take after every 61
  tasks it runs from its deque: one global-queue entry, or if that is empty, one steal attempt on
  each adopted deque. The check is made only where a worker may already take outside work, in its
  idle loop and in rule 3 within its margin; it is not made while a worker runs rule-1 entries of
  a scope it waits on, nor past its margin, so it never adds stack depth past the margin or runs
  another client's task inside a wait where helping would not. So a global-queue or adopted-deque
  entry waits behind a bounded number of tasks while some worker is in its idle loop or helping
  within its margin. That is a bound in tasks, not in time, and it does not cover entries deep in
  a worker's own deque: the owner pops newest first, so its oldest entries leave only when a
  thief takes them, as in every work-stealing scheduler.
- **Task storage and bounds.** A spawned closure is stored in a fixed-size slot (64 bytes inline,
  larger closures in a heap block owned by the slot) so small tasks never call `malloc`. Records
  come from a pool per *creator*: each worker and adoptable slot has its own pool of at most
  `maxTasksPerCreator` live records, and every other thread shares one pool of at most
  `maxExternalTasks`, allocated and freed under the global queue's mutex. A record is live from
  allocation until the thread that ran it returns it: a thief returns it to the creator's lock-free
  return stack (multi-producer, single-consumer), the owner moves returned records to its free list
  when that list runs empty, and the owner counts live records with a plain counter it alone
  changes. When a creator's pool is exhausted, `spawn` runs inline and `trySpawn` returns false.
  The bound is on records wherever they are queued, so a drain, which moves records to the global
  queue, does not let a creator exceed it. Live task records in the whole scheduler are therefore
  at most `(workerCount() + adoptableThreads) * maxTasksPerCreator + maxExternalTasks`, a number
  known at construction. Coroutine frames allocate through a per-scheduler frame pool via the
  promise's `operator new` and are returned the same way; the pool keeps at most a configured
  number of free bytes, so the bound limits retained memory, not live coroutines, which their
  clients bound.
- **Where spawned tasks go.** A spawn from a worker goes onto that worker's own deque, whether it
  comes from a scope's creator or from one of the scope's tasks (a flat spawn), so a tree traversal
  gets ordinary deque-to-deque work stealing. A spawn from an adopted thread goes onto its deque,
  with the publish protocol for adopted pushes (below). A spawn from a non-worker goes to the
  global queue. A finisher that enqueues a coroutine resumption puts it on its own deque if it is a
  worker and into the global queue otherwise, so an adopted thread's deque never holds another
  client's resumption. Every task record carries what a finisher needs: the address of its scope's
  counter and the parker the scope recorded at construction, both copied into the record at spawn.
- **Scopes record their creator.** A `TaskScope` records, when it is constructed, its creating
  thread, that thread's parker (a worker's, an adopted slot's, or a `ParkerPool` slot) and, on a
  worker or adopted thread, the deque and its bottom index (the watermark). Its waits use exactly
  those. Because the record must stay true while the scope lives, `adopt()` and the release of an
  `AdoptedThread` assert that the thread has no live `TaskScope` (a thread-local count), and the
  scheduler's destructor asserts that no `AdoptedThread` is live. `TaskScope`s nest last in, first
  out on one thread, because each is joined before its creator returns, so everything the creator
  spawns from its own stack lies above the watermark. An `AsyncTaskScope` has no watermark and no
  parker: its creator is a coroutine whose frame leaves the thread's stack when it suspends, so its
  spawns are ordinary deque or global-queue entries and its completion enqueues the resumption.
- **Waiting.** A creator that waits on a `TaskScope` applies, in order:

  1. If its deque holds an entry above the scope's watermark, pop and run it.
  2. If the scope's counter, read with acquire ordering so the creator sees its tasks' results,
     is zero, return.
  3. Otherwise, on a worker, help: take a task from the global queue or steal one, while the stack
     has more than the margin left. When it is past the margin, or finds nothing, it tries to sleep
     with the protocol below, which keeps it awake if it is the last awake worker and work is
     queued; it then takes queued work even past the margin. An adopted thread or a non-worker
     does not help: it sleeps on the scope's parker.

  A worker never runs a deque entry below its innermost live watermark; those entries leave only
  by being stolen from the top or by the drain step before a sleep. An idle worker runs its own
  deque, then the global queue, then steals, for `idleSpinRounds` rounds before it tries to sleep.

  A steal round takes from the global queue first, then probes every adopted slot's deque in slot
  order, then probes every other worker exactly once, starting at an offset chosen by a per-worker
  generator seeded from the worker index (a minimal-standard linear congruential or xorshift
  generator, never `std::random_device`, with a multiply-shift range reduction) and going round
  in index order. A round that finds nothing has looked at every queue once, so a worker woken
  for work it was meant to find cannot miss it by bad luck. The seeded offset also makes victim
  sequences reproducible per worker.
- **Sleeping.** Every worker sleeps on its own parker (below), whether it is idle or a waiting
  creator. The scheduler keeps the `awake` count and a per-worker state word: awake, asleep
  shallow (asleep while within its margin, for example in its top-level idle loop) or asleep deep
  (asleep past its margin). Every transition from either asleep state to awake is one
  compare-and-swap on that word, and exactly the thread whose swap succeeds increments `awake`; a
  thread whose swap fails does not increment, because the thread that won already counted the
  worker. Every waker changes the worker's parker whether or not its swap succeeded, which at worst
  causes a spurious wake, and notifies it only if the parker's waiting bit was set (below). This
  rule covers every wake: a sleeper waking itself in step 4, a publisher, a finisher waking an
  asleep creator, the hand-off below, the parallelism wake, and shutdown. *Queued work is visible*
  means `globalQueued > 0` or some adopted slot's deque has bottom greater than top, each read
  with a sequentially consistent load. To sleep, a worker:

  1. drains its own deque into the global queue, removing entries with the owner's Chase-Lev
     `take` (including its compare-and-swap on the last element), never by reading the array;
  2. sets the waiting bit in its parker word with a sequentially consistent `fetch_or`, which
     returns the old word `w`; the value it will wait on is `w | waitingBit`;
  3. stores asleep shallow or asleep deep in its state word with a sequentially consistent store
     and decrements `awake` with a sequentially consistent read-modify-write. No standalone fence
     is used: these and the publishers' and destructor's sequentially consistent operations below
     are the two halves of each store-load pairing (the form Filament's JobSystem uses), which
     ThreadSanitizer models as ordinary atomics;
  4. re-checks: every worker checks the stop flag, a waiting creator re-checks rules 1 and 2, and
     every worker checks whether queued work is visible. An idle worker that sees the stop flag
     wakes itself with the swap rule and exits. If its scope completed, or if queued work is
     visible and it is not past its margin, it wakes itself (the swap on its own state word,
     incrementing `awake` only if the swap succeeds) and goes back to work. If queued work is
     visible, it is past its margin and it was the last awake worker (the decrement returned 1 or
     less, so that a transiently low count errs toward staying awake), it first hands the work
     off: it looks for a worker asleep shallow and wakes it with the swap rule. If the swap
     succeeds, the woken worker is counted awake and will take the work from its shallow stack; if
     it fails, the worker was already woken and counted by another thread. Either way an awake
     worker exists that will re-check before it next sleeps, and this worker continues to step 5.
     Only when no worker is asleep shallow does it wake itself and run the work past its margin;
  5. otherwise waits on its parker for a change from `w | waitingBit`. Whenever that wait returns,
     for any reason (a change, a change that happened before the wait began, or a spurious
     return), the worker first wakes itself with the swap on its own state word, incrementing
     `awake` only if the swap succeeds, clears its waiting bit, and only then does anything else
     (every path out of steps 4 and 5 clears the bit the same way): a waiting creator goes back to
     rules 1 to 3, an idle worker exits if the stop flag is set and otherwise looks for work, and
     either one that finds nothing sleeps again from step 1.

  Steps 3 to 5 touch only the stop flag, the scope counter, the visibility reads, state words and
  parkers; the worker runs, pops and steals nothing while its state is asleep. So a worker
  executing anything other than those steps is counted awake. Without the self-wake on leaving
  step 5 that fails: a waker whose swap failed because the worker had not yet reached step 3 still
  changes the parker, the worker then stores asleep and decrements, its wait returns at once, and
  it would run on counted asleep and decrement again at its next sleep.

  `awake` is a signed count. The transitions that change either side of it are exactly these: a
  worker storing an asleep value in its own state word in step 3, followed by its one decrement;
  and a successful swap from asleep to awake on some worker's word, followed by one increment by
  the thread that won the swap. A failed swap changes neither side. Workers start awake with
  `awake` equal to the worker count. So `awake` equals the number of workers whose state word is
  awake, except for transitions in flight, each of which is one store or swap followed by its
  single matching decrement or increment. That clause allows a transiently negative count: a
  publisher can win the swap on a worker and be delayed before its increment while the worker,
  woken by another waker's parker change, fails its own swap, finds nothing and decrements again.
  So the equality is asserted only at quiescence: the destructor, after joining every worker,
  asserts it, and the scheduler tests assert it at their quiescent points.

  **Publishing.** Two kinds of push must publish: a push to the global queue (under its mutex,
  ending with the sequentially consistent increment of `globalQueued`), and a push by an adopted
  thread to its own deque (the Chase-Lev push with its bottom-index store made sequentially
  consistent). After either, the publisher reads `awake` with a sequentially consistent load. If
  no worker is awake, tested as `awake <= 0` so that a count driven below zero by a defect still
  leads to a wake rather than to silence, it picks an asleep worker, preferring one asleep shallow,
  switches its state to awake with a compare-and-swap, increments `awake` if its swap succeeded,
  and changes that worker's parker, notifying it if the waiting bit was set. The sleeper's step 3
  decrement and step 4 visibility reads, and the publisher's push and its read of `awake`, are all
  sequentially consistent, so they fall in one total order and at least one sees the other: either
  the sleeper sees the queued work in step 4, or the publisher sees `awake` at zero and wakes
  someone. The thread whose swap succeeds increments `awake` before it changes the woken worker's
  parker, so the count is never low while a woken worker is still starting, and never counts one
  wake twice. The hand-off keeps this: the deep worker's own decrement happened before it wakes
  the shallow worker, and the shallow worker is counted awake by the successful swap before the
  deep worker waits; in the window between the two, a concurrent publisher reads `awake` as zero
  and wakes another worker, which only adds parallelism. If the shallow worker was waking itself
  at the same moment, exactly one of the two swaps succeeds and only that one counts it.

  On native platforms a parker wait is `std::atomic::wait`; in the editor Wasm build that reaches
  `emscripten_futex_wait`, which the build wraps (`-Wl,--wrap=emscripten_futex_wait`) for the
  GPU-owning application pthread. The wrapper passes every other thread straight to the real
  futex wait, so scheduler workers sleep through `Atomics.wait`, which is permitted on workers;
  Milestone S5 tests that branch.
- **Local pushes and the parallelism wake.** A worker pushing onto its own deque skips the publish
  protocol: it is awake, and it runs that entry, loses it to a thief, or drains it before it can
  sleep. It does not skip waking for parallelism. Each worker owns a `searching` flag that only it
  changes: it sets it when it starts looking for work (after waking, or on finding its own deque
  empty) and clears it when it starts a task or reaches sleep step 1, incrementing and
  decrementing a shared `searching` count with each change. Because only the owner changes its
  flag and every set is paired with one clear, the count cannot leak; at quiescence it is 0, which
  the tests assert. After a local push the worker reads `awake` and `searching` with relaxed
  loads, and if some worker is asleep and none is searching it wakes one asleep worker with the
  swap rule. A searcher that starts a task and was the only searcher does the same, so a fork
  spreads one worker at a time without every push paying a wake. These reads are unfenced, so a
  wake can be missed; that costs parallelism only. Adopted pushes take the same parallelism check
  after their publish. Filament checks its sleeping count on every `put`, Rayon wakes sleepers when
  work is posted, and Taskflow keeps one thief awake while any worker is busy, for the same reason:
  without it, a fork from the one awake worker stays on that worker while the others sleep. The
  cost on a worker's spawn path is two relaxed loads of lines written only on sleep, wake and
  search transitions, expected to fit the spawn goal and measured by S6.
- **The invariant.** *Whenever a task is queued, at least one worker is awake.*
  - On a worker's deque: only its owner pushes there, the owner is awake when it pushes, and it
    drains its deque before its state can say asleep.
  - In the global queue or on an adopted thread's deque: every such push publishes, and the
    publish pairing gives either a worker that sees the work before it sleeps or a publisher that
    wakes a worker. No assumption about the adopted thread is needed: it may block outside the
    scheduler, return to its caller or sleep, and its entries are still covered.
  - An awake worker always makes progress: it runs a task, pops one by rule 1, steals, or tries to
    sleep; it cannot sleep while it is the last awake worker and queued work is visible, and a
    steal round looks at every queue. So queued work always has a runner, and the margin decides
    only which worker runs it.
  - The parallelism wake is not part of this argument, so missing or skipping it cannot leave work
    without a runner.
- **Termination.** For a program with finitely many tasks, every task finishes. The unit of the
  argument is a *segment*: a spawned closure from its start to its return, or one stretch of a
  coroutine from a resumption (or its first start) to its next suspension or completion. Each
  segment has its own start time, the moment a thread begins running it; a coroutine resumed three
  times is four segments.

  First, every queued segment starts. By the invariant some worker is awake while anything is
  queued. Every awake worker is, at any moment, either running finite segment code between
  scheduler calls, or inside a scheduler loop (its idle loop, or a `TaskScope` wait applying rules
  1 to 3). In its idle loop it takes visible work or sleeps. In a wait it pops its own entries
  (rule 1), returns (rule 2), or helps within its margin (rule 3); past its margin it tries to
  sleep, and the last-awake rule keeps the last awake worker taking visible work even past its
  margin. An entry below a waiting worker's watermark is stolen, or drained to the global queue
  when that worker sleeps. With finitely many segments, each of these steps makes progress, so
  each queued segment is eventually started.

  Second, a running segment can be held up only by two kinds of edge, and both point to segments
  that started later. A wait inside a segment waits for tasks of its scope, which that segment or
  its scope's tasks spawned after it started. A segment stacked on top of a waiting segment, by
  rule 1 or by helping (including a resumption run by rule 1), started after the segment it is
  stacked on, because it started on that stack while the lower one was already waiting. An
  `AsyncTaskScope` join is not an edge: the coroutine has suspended, ending its segment, and its
  resumption is a new segment enqueued only after the scope's tasks finished. Suppose some started
  segment never finishes, and take the one with the latest start time. Every segment it depends on
  through either edge started later, so it finished; the scope's last finisher changed the recorded
  parker; so its waits return, the segments above it have returned, and its own code is finite. It
  finishes, a contradiction; and each resumption a finisher enqueues starts by the first part.
  Waits made by an adopted thread's or a non-worker's own code are not segments; each returns once
  its scope's segments have finished and the last of them has changed the recorded parker, which
  the argument above establishes.

  For an unbounded stream of work, such as the editor's, the first part holds only as far as the
  fairness rule of the global queue reaches: global-queue and adopted-deque entries are taken
  within a bounded number of tasks while some worker reaches its idle loop or rule 3, and entries
  deep in a worker's own deque wait for a thief. No bound in time is claimed.
- **Parkers and scope lifetime.** A scope lives in its creator's frame and is destroyed as soon as
  its creator sees it complete, so no thread may touch the scope after the change that lets the
  creator return. Every worker and adoptable slot has a parker for the scheduler's lifetime; each
  non-worker that creates a `TaskScope` gets one from scheduler-owned stable storage. The rules:

  - A finishing task reads the recorded parker pointer from its task record, then decrements the
    scope's counter with acquire-release ordering. That decrement is its last access to the scope.
    If it reached zero, the finisher changes the parker (and, if the creator is an asleep worker,
    first swaps its state to awake, incrementing `awake` only if that swap succeeds), touching only
    the parker and the scheduler's own state, and notifies only if the value its change replaced
    had the waiting bit set, so a creator that is still running pays no system call. Filament's
    JobSystem keeps waiter bits beside its running count for the same reason. Whether
    `std::atomic::notify_one` elides the system call when nothing waits is implementation-defined,
    so the scheduler does not rely on it.
  - The parker is one 32-bit atomic word, a waiting bit and a 31-bit epoch: 32 bits is the width
    of the Linux futex and of Wasm `Atomics.wait`. On Apple platforms libc++ may wait on a 64-bit
    contention type and send a 32-bit wait through its proxy table; S1 measures that and may use a
    64-bit word there. Every change is a release read-modify-write that advances the epoch and
    returns the old word, and the sleeper's `fetch_or` in step 2 is a sequentially consistent
    read-modify-write on the same word, so both are in the word's single modification order. A
    change ordered before the `fetch_or` is read by it, with everything the waker wrote before the
    change, so the sleeper's step 4 re-check sees the completed scope or the queued work and it
    does not wait. A change ordered after it returns a word with the bit set, so the waker
    notifies. A notify is therefore never skipped for a thread that will wait. A sleeper could miss
    a change only if exactly 2^31 changes landed between its step 2 and its wait, which is
    accepted. The lost-wakeup tests cover this and must be observed red with the bit check broken
    (notify skipped unconditionally).
  - The counter word is 64 bits, a 63-bit count and, for an `AsyncTaskScope`, the join flag in the
    top bit. A scope's count is at most the scheduler's live-record bound, which is known at
    construction, and the constructor asserts that bound is below 2^62. A test spawns into one
    scope until `trySpawn` returns false and asserts the scope is not seen complete until every
    task ran (Filament shipped a defect where a parent with its maximum child count read as
    finished).
  - `co_await scope.join()` writes its coroutine handle into the scope, then sets the join flag in
    the counter word with a compare-and-swap loop using release ordering, so the handle is
    published with the flag. If the loop observes a count of zero, `await_suspend` returns false
    and the coroutine continues without suspending. Otherwise the finisher whose acquire-release
    decrement reaches zero sees the flag in the value it replaced, which also acquires the handle,
    and enqueues the resumption as described under where tasks go, rather than resuming inline, so
    a resumption never nests on the finisher's stack. The scope is not destroyed in between,
    because its creator is that suspended coroutine. Milestone S3's ThreadSanitizer tests cover
    registration racing completion.
  - A creator about to sleep, worker or not, sets its parker's waiting bit, re-checks, and waits
    on `w | waitingBit`, as in the sleep steps. A notify without a change would let
    `std::atomic::wait` block again, so every wake is a change of the parker; a creator woken by an
    event for an outer scope re-checks its current scope and sleeps again, which is a spurious wake
    and nothing more.
  - Parker storage for non-workers lives in a `ParkerPool`, a separate object the scheduler
    creates and shares by `std::shared_ptr`: it holds the slots (stable addresses, allocated in
    fixed blocks) and a mutex-protected free list. Each such thread keeps a small thread-local map
    from a scheduler's unique generation number (not its address, which could be reused) to an
    entry holding a `std::shared_ptr` to that scheduler's pool and its slot index. The scheduler
    holds one more reference and, when it is destroyed, marks the pool closed after joining every
    worker. When a thread exits, its thread-local destructor returns each slot to the pool its
    entry references and drops the reference, so the pool outlives every thread that holds one of
    its slots, and a scheduler destroyed first (one local to `main()`, destroyed before the main
    thread's thread-local cleanup, for example) leaves nothing dangling. Whenever a thread
    registers with a scheduler (its first lookup for a new generation misses), it first sweeps its
    map and drops every entry whose pool is closed; a closed pool has no live scope, so none of its
    slots is in use. A thread therefore holds pools only for schedulers that were open at its last
    registration, plus the one it registers with. A finisher that read a parker pointer before the
    slot was reused can still change it; the slot's next owner then gets one spurious wake, which
    is harmless because slot storage is stable while any reference to the pool lives and every wake
    is followed by a re-check. Storage is bounded by the largest number of non-workers that hold
    the scheduler's scopes at the same time.

  The injected-delay tests cover a delay between a creator's re-check and its parker wait, between
  a publish and the publisher's read of `awake` (for the global queue and for an adopted push),
  between a sleeper's decrement and its re-check, and between the final decrement and the parker
  change with the creator returning and destroying the scope at once; that last test runs under
  AddressSanitizer and ThreadSanitizer and must be observed red with the wake word placed inside
  the scope before it counts.
- **Liveness tests.** Each is a test in Milestone S1, run at 1, 2 and 4 workers:

  - One worker recursing through nested synchronous scopes, the next child still on its own deque
    above the watermark: rule 1 runs the child; recursion depth is the program's own nesting.
  - Every worker past its margin with work queued: exactly one worker, the last awake, runs it,
    asserted by a counter of tasks run past the margin.
  - The last awake worker past its margin with work queued and another worker asleep shallow: the
    shallow worker runs the work and the counter of tasks run past the margin stays at zero.
  - A sleeper's self-wake racing a publisher's wake of the same worker: `awake` ends equal to the
    number of workers whose state is awake. The injected delay holds the sleeper between its step
    4 decision and its swap, and the test must be observed red with an unconditional increment.
  - A finisher's swap failing just before the creator's step 3: a worker waits on an inner scope
    inside an outer one, and the injected delay holds it between steps 2 and 3 while the outer
    scope's last task finishes. At quiescence `awake` must equal the number of workers whose state
    is awake, and a following schedule with every worker past its margin must complete. Must be
    observed red with the self-wake swap after the parker wait removed.
  - Shutdown racing an idle worker's sleep: the injected delay holds an idle worker between steps
    1 and 2 while the destructor sets the stop flag. The destructor's join must return. Must be
    observed red with the stop-flag re-check in step 4 removed.
  - Two workers past the margin, each holding the other's scope's flat task below its own
    watermark: both drain into the global queue and the last awake worker runs both tasks.
  - An `AsyncTaskScope` whose task is drained into the global queue while every worker but one is
    past its margin and asleep: the last awake worker runs the task, the finisher enqueues the
    resumption, and the coroutine and every waiter blocked on it complete. `co_await join()` on a
    scope whose count is already zero does not suspend.
  - Adopted pushes: with every worker asleep, an adopted thread spawns one task and then blocks on
    a condition variable that only that task signals; the task runs and the thread wakes. Must be
    observed red with the adopted push's publish removed, and red with adopted deques left out of
    the step 4 visibility reads.
  - A coroutine running on an adopted thread creates an `AsyncTaskScope`, spawns, suspends on
    `join()`, and the thread returns to its caller: the task is stolen, the coroutine resumes on a
    worker and completes.
  - An adopted thread waiting on its scope never runs a task of another scope queued at the same
    time, asserted by per-scope run counters; slot exhaustion falls back to the non-worker path;
    releasing an `AdoptedThread` whose deque still holds `AsyncTaskScope` entries drains them to the
    global queue and they complete. Death tests: `adopt()` with a live `TaskScope`, and an
    `AdoptedThread` released or a scheduler destroyed with a live one, each fail their assertion.
  - The parallelism wake: with 4 workers asleep, a non-worker spawns one root that spawns 64
    children; the first child blocks, under a hang guard, until a child has started on a second
    worker. Must be observed red with the parallelism wake removed. At quiescence the `searching`
    count is 0.
  - Bounds: one worker spawns `maxTasksPerCreator` tasks into a scope, waits on an inner scope so
    it drains and sleeps, is woken and spawns again: the extra spawns run inline and the live-record
    counter never exceeds its bound. The same with an adopted creator and with a non-worker
    against `maxExternalTasks`.
  - The global queue is first in, first out, and a worker with endless local work in its idle
    loop still takes a global entry within 61 tasks, or, with the global queue empty, an entry
    from an adopted deque whose owner is blocked outside the scheduler.
  - One non-worker creating, using and destroying 1,000 schedulers in turn: after each registration
    the number of live `ParkerPool` objects (a test-only counter) is at most 2, and after the last
    scheduler is destroyed and the thread registers with a fresh one it is 1. Must be observed red
    with the sweep removed.
  - A scheduler destroyed before a non-worker that used it exits, and a test binary whose `main()`
    creates a scheduler, waits on a scope from the main thread and destroys the scheduler before
    returning. Both run under AddressSanitizer with no use-after-free, and must be observed red
    with the thread-local entry holding a raw pointer to the scheduler's free list.
  - A stolen task that waits; `Join` stolen and not stolen, and `Join` from a non-worker; a spawn
    at each bound (`spawn` inline, `trySpawn` returning false); a non-worker scope with zero idle
    workers; zero workers at run time and compiled out (`trySpawn` false, `spawn` inline); seeded
    mode (`trySpawn` true until the calling thread's bound).
- **Blocking inside tasks.** A worker that blocks inside a task on anything other than the
  scheduler (a condition variable, an `std::atomic::wait` in client code, a lock held by a thread
  that waits on scheduler work) is counted awake while it holds queued work it cannot run, so the
  invariant does not give that work a runner. The rule is therefore that tasks may block only
  through the scheduler: `TaskScope::wait()`, the join and schedule awaitables, and `Join`. A short
  critical section under a mutex is allowed when no holder of that mutex ever waits on scheduler
  work while holding it. The rule is documented in the API, and debug builds check it where Donner
  itself blocks: the per-thread in-task flag that the document access coordinator already checks
  is also checked by Donner's own waiting helpers, which assert when called from a task. The rule
  is about tasks; an adopted thread's or non-worker's own code may block, because what it queued
  is covered by the publish protocol.
- **Worker stacks.** Workers are created with an explicit stack size (512 KiB by default, an
  option) instead of the platform default, which differs between platforms and toolchains. The
  margin (`TaskSchedulerOptions::helpMarginBytes`, 128 KiB by default) is a heuristic that decides
  which worker helps; it is not part of correctness. Because the last awake worker runs queued
  work even past its margin, native stack depth is bounded by policy instead. Each client declares
  its tasks' native stack bound and whether its tasks wait (with the declaration below for a client
  whose tasks wait); a task that waits can, through rule 1 and helping, nest further tasks on the
  same stack, bounded by the program's own fork-join structure, as in other work-stealing
  runtimes. A hard limit turns exhaustion into a named failure: before running any task, a worker
  compares the stack pointer with its stack floor plus a reserve and fails with a release assertion
  naming the scheduler, instead of overflowing the stack. An adopted thread runs only rule 1, which
  nests only the program's own structure; the same check applies to it where `adopt()` can read the
  thread's stack bounds from the platform (to be verified per platform in S1). The style traversal
  of 0072 Part D declares one task frame and tasks that never wait (one flat scope, an explicit
  heap stack for the inline part of a subtree, `trySpawn` with the heap stack as the fallback), so
  each help by a style task adds one frame.

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
  stack-floor assertion is then still what stops the process.

  **Worker QoS.** On Apple platforms each worker sets the QoS class in
  `TaskSchedulerOptions::workerQos` (`pthread_set_qos_class_self_np`; user-interactive by
  default), and an adopted thread keeps its own; elsewhere workers inherit the creator's
  scheduling. This is not a task-priority feature: it keeps interactive work off efficiency cores
  as far as the operating system honors the class, as Filament runs all its threads at one
  interactive class. `DefaultWorkerCount()` subtracts one for a caller that adopts and does not
  halve for simultaneous multithreading unless S6 measures that it helps.
- **Adopted threads and non-worker waits.** A non-worker (the render worker holding document
  access, before it adopts, for example) never runs tasks while workers exist: its spawns go to
  the global queue with the publish protocol, and when it waits it re-checks its scope's counter
  and sleeps on the scope's recorded parker, which the finisher of the scope's last task changes.
  It therefore never runs another client's task while it holds a guard. With zero workers, every
  spawn runs inline on the spawning thread.

  A thread may instead call `adopt()`, which claims one of `adoptableThreads` preallocated slots (a
  fixed deque, a record pool and a parker, claimed by a compare-and-swap on a slot mask, as
  Filament's `JobSystem::adopt` does) and releases it when the returned object is destroyed. An
  adopted thread is not a worker: it is not counted in `awake`, has no state word, never steals
  and never takes from the global queue. What changes is where its spawns go and what its waits
  run. Its spawns go to its own deque with the adopted publish, so it runs its first task with no
  wake on the critical path while idle workers steal the rest. When it waits on a `TaskScope` it
  applies rule 1 and rule 2 and otherwise sleeps on the scope's parker, with no drain: its
  remaining entries are already covered by the publish protocol, and workers steal them. Every
  entry on its deque was pushed by the thread itself, as a scope's creator or by a task it ran,
  and resumptions never go there, so it runs only its own work and its descendants, never another
  client's task while it holds document access. Releasing an `AdoptedThread` asserts that the
  thread has no live `TaskScope`, drains any remaining entries (from `AsyncTaskScope`s) into the
  global queue with the publish protocol, and only then frees the slot, so a slot never has two
  owners. `adopt()` returns an empty object, and the thread keeps the non-worker path, when no slot
  is free, with zero workers, in seeded mode, or with threading compiled out. An adopted thread
  that helps with any task, as a worker does, is not offered: its stack is not the scheduler's and
  the cross-client rule would be lost.

  What adoption costs and buys: each adopted push pays one sequentially consistent store and one
  sequentially consistent load more than a worker's push, and every sleeping worker's re-check
  reads one more deque per adoptable slot (1 by default). In return the first task of a fork runs
  at once on the forking thread, instead of after a worker's wake. If S6's W5 and W8 show that
  saving is too small to matter, adoption is removed and the creator's thread takes part only
  through the non-worker wait path, at the cost of one worker wake per fork; that is recorded
  under Open Questions.
- **Seeded mode.** With `seed` set, the scheduler runs on the calling thread only and, whenever
  more than one task is runnable, picks the next one with a seeded generator. It reproduces task
  orders, not steals: the deque's compare-and-swap race and real interleavings never happen in
  this mode, and are covered by the stress tests at real worker counts and by ThreadSanitizer. Its
  queue is bounded by the calling thread's `maxTasksPerCreator`.
- **Shutdown.** The destructor asserts no scope and no `AdoptedThread` is live
  (`UTILS_RELEASE_ASSERT`), sets the stop flag with a sequentially consistent exchange, then wakes
  every worker with the same swap rule (the swap, then the parker change, whether or not the swap
  succeeded) and joins them. With no scope live, every worker is idle. A worker's step 3 store and
  its step 4 read of the stop flag are sequentially consistent, as are the destructor's exchange
  and its swap, so either the worker's re-check sees the flag and it exits, or the swap sees the
  worker asleep and succeeds; the parker change then follows the worker's step 2, and the wait in
  step 5 returns. A worker that has not reached step 3 sees the flag at its next re-check. A
  coroutine suspended on `schedule()` after destruction began is a programming error caught by
  the same assertion.
- **Nested parallelism.** Scopes nest freely; nested work goes to the current thread's deque, so a
  nested parallel loop inside a task adds parallelism without adding threads.
- **Locks.** A task must not hold a lock across `wait()` or `co_await`: helping could run a task
  that takes the same lock. Debug builds record, per thread, whether it is inside a task, and the
  document access coordinator asserts that no document guard is acquired inside one. That
  assertion fires only when a guard is acquired; reading shared state without a guard is prevented
  by the clients' own rules (0072 Part D gives workers a `const Registry&` and handles taken before
  the fork), not by the scheduler.
- **Waits on threads with obligations.** The editor's application pthread must keep servicing GPU
  dispatch while blocked. Such a thread never calls `wait()`; it either awaits an `AsyncTaskScope`
  from a coroutine or routes the wait through its existing owner-wait adapter, which the options
  accept as a wait hook. A brief lock of the global queue's mutex from that thread goes through
  the same wrapped futex wait. The style pass runs on the render worker, not on the application
  pthread.

### WebAssembly

- **Non-threaded builds** (the renderer's `wasm` configuration, and any downstream Emscripten
  build without pthreads): without `__EMSCRIPTEN_PTHREADS__` the scheduler, the Donner lock type
  and the once flag compile to their inline forms whatever `//donner/base:threading` says, so
  `workerCount()` is 0 and every client runs its serial path through the same calls. The shipped
  plain module also sets `threading = false` through its own target-level transition and so gets
  the full compiled-out form; a downstream consumer keeps Emscripten's guard calls unless it sets
  the flag ([Configuration](#configuration)). No `.bazelrc` change is made, so the threaded
  `editor-wasm` configuration, which inherits `wasm`, is unaffected.
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
  build), task record pools and the frame pool's retained bytes. Milestone S5 measures the
  editor's peak heap with the scheduler running and keeps the build's headroom contract; if it
  does not fit, the worker count or the memory size changes together with a new measurement,
  never by enabling growth.
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
- **W7, scaling.** W2 (25 ns body), W3 and W8 at 1, 2, 4 and 8 participating threads and at the
  machine's full count. On Apple silicon, counts above the number of performance cores are
  reported but not gated.
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
only, recorded with the results. The W6 compiled-out binary is built through the compiled-out
configuration transition, so it is part of the same invocation. Each repetition of each case
launches a fresh process for that peer, which creates its pool outside the timed region, warms
up, runs once and exits, so only one pool exists at a time and an idle peer's spinning workers
cannot interfere. Thread counts are *participating
threads*: pool workers plus the calling thread when the peer lets it run tasks (Donner with an
adopted thread, Filament with an adopted thread, oneTBB's thread in `task_group::wait`, enkiTS's
main thread, Taskflow's `corun`). Every peer is configured to the same participating count, and
every threshold below applies at 8 participating threads unless it names another count. Each case
warms up for at least 1 s in every process, runs 15 repetitions and reports the median and
median absolute deviation with the minimum and maximum; repetitions are interleaved across peers
(each peer's first process, then each peer's second) so thermal drift affects all of them
alike. A quiet-machine
gate runs before and after: the run is rejected when the 1-minute load average exceeds 0.5 per
core, when another process used more than 5 percent of a core during the run, or when the median
of a fixed calibration loop moved by more than 3 percent between the two gates. On Linux the CPU
frequency governor is set to performance where permitted and the setting recorded. macOS offers
no way to pin a thread to a performance core: workers of every peer run at the user-interactive
QoS class, which is a placement hint, and the run records the power source, the thermal state and
the performance and efficiency core counts. The sleep primitive each peer actually uses on each
platform is recorded beside its results.

**Platforms.** Apple silicon macOS on a machine with at least 8 performance cores, and Linux
AArch64 and Linux x86-64 on machines with at least 8 cores, with the hardware model, kernel,
compiler and every peer's pin recorded beside the results. The editor's threaded Wasm build
runs W1, W2, W5 and W8 for Donner only, in a cross-origin isolated headless browser, as
information, not gated.

**Acceptance.** Medians are compared per platform. The best peer is chosen per metric and per
workload, excluding the `std::thread` baseline: the lowest time per task, the highest throughput,
the lowest p50 and, separately, the lowest p99, the lowest idle CPU, and the highest scaling
efficiency. A comparison that misses its threshold by less than the larger of the two median
absolute deviations is rerun once with 15 more repetitions, and the medians of all 30 decide; a
miss larger than that is a miss.

- W1, W2 at grains 1 to 16, and W3 with cutoff 0 (the tiny-task workloads): Donner is faster than
  the best peer, or within 5 percent of it with the reason recorded here.
- W2 at larger grains, W3 with cutoff 12, W4 and W7: Donner is within 10 percent of the best
  peer, and its scaling efficiency at 8 threads is within 10 points of the best peer's.
- W5: Donner's p50 is within 10 percent and its p99 within 25 percent of the best peer's, and its
  idle CPU is no higher than the best peer's plus 1 percent of one core.
- W6, the disabled mode: with zero workers at run time and with threading compiled out, Donner is
  within 2 percent of the plain serial loop; at 1 worker, within 10 percent.
- W8: Donner with an adopted waiter is within 3 percent of the fastest peer's natural form, or
  faster; the non-worker form's result is recorded, both for 0072's threshold calibration and to
  decide whether adoption stays (see [Open Questions](#open-questions)).
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

- **Bounded queues.** Live task records are capped per creator, wherever they are queued:
  `maxTasksPerCreator` per worker and adoptable slot, and `maxExternalTasks` for all other threads
  together, so the scheduler holds at most `(workerCount() + adoptableThreads) *
  maxTasksPerCreator + maxExternalTasks` records, known at construction. Past a cap work runs
  inline (or `trySpawn` returns false), and draining work into the global queue does not reset a
  cap, so a client that spawns per document element cannot exhaust memory through the queues.
  Coroutine frames are bounded by their clients, and the frame pool bounds only retained memory.
- **Bounded recursion, or a named stop.** Helping past the margin is handed to a shallow worker
  when one exists; otherwise the last awake worker stacks queued work, which for waiting tasks is
  bounded by how many are queued at once, as described under worker stacks, and the stack-floor
  check turns exhaustion into a named release assertion instead of an overflow. Coroutine chains
  use symmetric transfer, and `ParallelFor` splits iteratively.
- **Memory pressure.** Task records come from the per-creator pools above and are reused; frame-pool
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
  `awake`, between an adopted push and its read of `awake`, between a sleeper's decrement and its
  re-check, between the final decrement and the parker change with the scope destroyed at once,
  and between a drain and the sleep). Each race test
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
  client's tests with threading compiled out, and the symbol check over Donner's object files of
  [Renderer threading](#renderer-threading). Client differential tests assert bit-identical
  output between their serial and threaded runs.
- **No deadlines.** Tests assert ordering and counts; generous hang guards turn a deadlock into a
  named failure.

## Candidate Clients

A survey of `origin/main` for places the scheduler applies, besides style, ranked below. The
maintainer has decided that tiny-skia's internals stay single threaded and that the renderer's
multithreading is focused on Geode, so the ranking covers Geode and the stages both renderers
share (parsing, style, shape computation, image decode, text). **Every benefit below is
unmeasured** unless it quotes a number the tree already records, and every size is the survey's
estimate in engineering days, including tests and review fixes. Each client keeps a correct serial
path and runs the same code inline at zero workers. Each one is designed in its own document;
none is in this document's estimate.

| Rank | Client | Unit of work | Shared state to handle | Days | Benefit |
| ---- | ------ | ------------ | ---------------------- | ---: | ------- |
| 0 | Style traversal (0072 Part D) | a subtree of about 64 elements | solved in 0072 | per 0072 | 0072 measures style at 3 to 159 ms per pass |
| 1 | Geode geometry prefetch | one draw whose path-cache lookup would miss | cache-component emplace, geometry budgets, encode counters, lazy caches on `ComputedPathComponent` | 1 to 2, then 8 to 12 | unmeasured; scales with cache misses (first frame, zoom bucket change, new shapes) |
| 2 | Shape-pass path-data parse on load | one `<path>` whose spline is stale | `ComputedPathComponent` emplace and listeners, geometry budget, warning order, `RcString` | 4 to 6 | unmeasured; the shape pass's own comment calls the parse its most expensive step |
| 3 | Image decode | one admitted `<image>` | fetch cache, budgets, `LoadedImageComponent` emplace | 3 to 5 | unmeasured; documents with several raster images |
| 4 | Glyph outline encode | one new unique glyph | FreeType face per font, glyph cache, text budget | 4 to 6 | unmeasured; likely small, residency already dedups |
| 5 | Web font decode at load | one `@font-face` source | font registry entities, lazy loading, font budgets | 4 to 6 | unmeasured; would decode faces never used |
| 6 | Style-source annotation parse | one detached parse | none new; needs a pollable completion | 1 to 2 | no speedup; removes the editor's `std::async` thread |
| 7 | Animated GIF export pipelining | quantize and encode frame N while N+1 renders | encoder frame order | 2 to 3 | unmeasured; niche export path |
| 8 | Thumbnail and font-preview prepare | one preview document | document owner-thread binding, one renderer and device | 5 to 8 | low: the lane is GPU and pipeline-compile bound |

**First clients after style**, in order:

1. **Geode geometry prefetch.** On a frame with path-cache misses, compute the missing fill and
   clip encodes and stroke outlines with their encodes on workers before the serial draw walk,
   which then consumes them instead of computing them inline. `GeodePathEncoder::encode` is static
   and reads only its arguments, and `Path` is documented immutable, so results are bit-identical
   whichever thread computes them; recording stays serial on the render thread. Tasks read through
   a `const Registry&` into a side table, the serial walk does every emplace and budget charge in
   order and drops results it refuses, the prefetch is bounded by the frame's remaining geometry
   allowance, and draws whose transform a pre-pass cannot know (pattern tiles, markers, masks)
   simply miss and encode inline. It starts with an encode-time counter and a cold-load and
   zoom-bucket benchmark, because no CPU timing of encode or stroke expansion exists in the tree;
   0061 records `pathEncodes == 0` on an unchanged frame, so the steady state gains nothing and
   the choice must rest on that measurement.
2. **Path-data parse on load.** Serial collection of each stale `<path>` with a view of its data,
   parallel parse into per-entity slots, serial commit in the existing storage order with the same
   budget calls and warnings, so listeners and warning order do not change.
3. **Image decode.** Fetch, header probe and budget admission stay serial in document order; the
   `stbi_load_from_memory` decodes run in parallel into slots; the emplace and the failure records
   are committed serially. Whether stb_image's failure-reason global is thread-local in this build
   is to be verified first.

**Unsafe, or not worth it:**

- **Geode command encoding, residency, uploads and pipeline creation**: unsafe. A device is used
  from one thread at a time, parameter writes ahead of the draws in one submit mean the last write
  wins for every draw (0061, "Do not parallelize Geode command encoding"), and
  `GeodeResidentSlab`, `GeodeBufferPool` and `GeodeGlyphCache` are documented not thread safe.
- **Compositor layer and tile raster**: unsafe for the raster, which is GPU work on one device
  with renderer state carried across composes; it is fed by client 1's prefetch over the union of
  the dirty layers instead.
- **Text shaping and glyph outline extraction**: an `FT_Face` is not safe for concurrent use, and
  HarfBuzz font entries are created lazily with a registry emplace; per-worker faces and a cache
  redesign are not worth it without a measurement.
- **Layout, paint-server, filter-graph and shadow-tree systems**: lazily memoized components,
  structural emplaces and small per-entity costs; not worth a parallel region.
- **XML and stylesheet parsing**: parsing creates entities as it goes, and sheets are small.
- **Hit testing and marquee selection**: a linear scan with early exit; a spatial index is the
  fix, not threads.
- **Tests and tools**: process-level parallelism (Bazel sharding, a forked child per suite case,
  libFuzzer jobs) is already the right tool.
- **Out of scope by maintainer decision**: tiny-skia internals, including 0061's CPU filter row
  bands (whose evidence is strong but benefits only the CPU backend), the prepare stage of 0060,
  and row-band rasterization, composite and mask passes. Listed so the reasoning is not lost.

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
  which exposes a completing scope's memory, and leaves an `AsyncTaskScope`, whose creator is
  not running, without a runner. One global queue and the last-awake rule cover every scope the
  same way.

## Open Questions

- **Ownership.** Recommendation: the application or renderer creates a scheduler and passes it by
  pointer, which keeps tests deterministic and Wasm pool sizing explicit; because threading is on
  by default, entry points given none use `DefaultTaskScheduler()`, the one process-wide instance,
  configurable before first use and kept until shutdown or exit. Tests never use it.
- **Editor worker count.** Recommendation: start with 2 scheduler workers in the editor Wasm
  build (raising `PTHREAD_POOL_SIZE` from 2 to 4); with the render worker, a dedicated thread that
  adopts a slot, that is 3 threads doing style. Revisit with W8 and Part D's measurements.
- **Keep adoption or cut it.** Adoption is kept in a deliberately simple form: adopted pushes
  publish like global-queue pushes, sleeping workers check adopted deques, and the adopted thread
  never helps. Its cost is one sequentially consistent store and load per adopted push and one
  more deque read per adoptable slot in each sleeper's re-check; it buys a fork whose first task
  runs at once on the forking thread. Recommendation: keep it if W8's adopted form beats the
  non-worker form by more than the run's noise; otherwise remove `adopt()` and let the creator's
  thread take part only through the non-worker wait path, which costs one worker wake (W5's
  latency) at the start of every fork. The rest of the design does not depend on it.
- **Defer the help margin.** Milestone S1 could ship the stack-floor assertion without
  `helpMarginBytes`, the shallow and deep sleep states and the hand-off: a waiting worker would
  help until the stack floor, as Filament, Rayon and oneTBB do without a margin, and the sleep
  protocol would have two states. That removes the hand-off, two liveness tests and the deep
  state, saving an estimated 1.5 days of S1. The cost is that clients whose tasks wait could no
  longer share a scheduler under the declaration rule of [Worker stacks](#internals) until the
  margin is reintroduced, and the reintroduction would revisit the sleep protocol and its proofs.
  No planned client waits inside tasks. Recommendation: keep the margin as designed, because the
  API allows waiting tasks from the first release; take the saving only if the maintainer prefers
  to restrict waiting tasks until a client needs them.
- **Merge `AsyncRenderer` into `svg::Renderer` (option A) or not.** Option A gives `Renderer` an
  asynchronous `submit` with a latest-wins slot, an inline mode and lazy construction of the
  backend on its owning thread, and deletes `AsyncRenderer`; the editor's pipeline stays editor
  code as the submitted job. Estimated 12 to 18 days. It puts a state-dependent threading contract
  ("call on one thread" until `submit`, then "only the render thread") on the library's most used
  object for one client, and does nothing for Geode's parallelism. Recommendation: the design in
  [Renderer threading](#renderer-threading) (a synchronous `Renderer` plus an editor
  `RenderWorker` with an inline mode), which keeps one contract per class; adopt option A only
  when a second client needs an asynchronous renderer, and then also run tiny-skia off the
  caller's thread for one uniform contract.
- **The switch's name, and earlier designs' thread flags.** Recommendation:
  `//donner/base:threading` as specified, in the lowest package every user of it depends on;
  `//donner:threads` was the other candidate. Either way it replaces the pool flags drafted in 0061
  (`--//donner/svg/renderer:render_worker_pool`) and 0060
  (`--//donner/svg/renderer:tiny_skia_prepare_threads`), and those drafts are updated to use it
  rather than land their own.
- **What runtime disable means.** The design takes a configured disable
  (`threadingDisabledByConfiguration()`) to mean both zero scheduler workers and an `Inline` render
  worker, so the runtime switch and the build switch produce the same behavior, with two
  exceptions: zero workers after failed starts keeps the render worker `Dedicated`, and the
  threaded editor Wasm build always keeps it `Dedicated` (next question). Undecided: whether the
  maintainer prefers that a configured disable set the worker count only and leave the render
  thread dedicated everywhere. Recommendation: keep the design as written.
- **Inline mode in the threaded editor Wasm build, and a threadless editor Wasm build.** In the
  threaded editor build, `Inline` would run on the application pthread, which owns the GPU device
  and must keep servicing dispatch; whether every GPU-owner call then takes the owner-local branch
  and no blocking readback waits on itself is not verified. Recommendation: do not allow `Inline`
  there until an editor Wasm smoke test asserts the owner-local branch; and do not make an editor
  Wasm build without threads a goal now (the editor depends on `PROXY_TO_PTHREAD`, and the browser
  main thread cannot `Atomics.wait`).
- **`RenderWorker` completing synchronously.** In `Inline` mode `requestRender` completes before
  it returns, `isBusy()` is false whenever the caller has control, and cancel and supersede do
  nothing; the blocking source-pane edit can no longer wait behind a render. That is a change to
  public behavior (there are no compatibility guarantees before 1.0). Recommendation: accept it;
  the alternative of keeping requests asynchronous in `Inline` mode would need a second thread.
  Whether every `isBusy()` call site in `EditorShell.cc` (41 today) behaves correctly when busy is
  never true across a frame is to be verified in S8.
- **The new class name.** Recommendation: `editor::RenderWorker` with `RenderWorkerOptions`, and
  `svg::RendererOptions` replacing both `Renderer` constructors, with no aliases kept.
- **Where the Geode fan-out design lives.** Recommendation: in an amended 0061, which already owns
  the renderer pool and the "do not parallelize Geode command encoding" decision, starting with
  the encode-time counter of Candidate client 1; not in this document.
- **Native Geode with threading compiled out.** Native Geode is incompatible with
  `threading = false`: Metal runs completion handlers on a queue it creates, and Vulkan's
  debug-utils messenger may call back from any thread, besides the queue, execution and resource
  mutexes in `VulkanDevice.cc`, `VulkanResourceState.cc` and `VulkanSwapchain.h`. Recommendation:
  leave it incompatible; a consumer that needs a threadless GPU build uses the browser backend.
- **When the Geode browser module is compiled out.** Milestone S9 (2.5 days) converts the Geode
  common and browser-bridge locks so the `wasm-geode` module passes the symbol check.
  Recommendation: ship the scheduler's first release with S8 (the software path and the render
  worker) and land S9 next, because no consumer has asked for a threadless Geode browser module
  yet; take S9 into the first release if one does.
- **Single-threaded contract with threading compiled out.** S8 turns the library's locks into
  no-ops in that configuration, so an embedder that calls Donner from several threads must
  serialize those calls itself, and `ThreadingMode::ConcurrentDom` fails with a release
  assertion. Recommendation: accept both and document them with the setting; keeping real mutexes
  would keep a thread-runtime dependency and contradict the symbol check.
- **Third-party threading.** The full-text tier's HarfBuzz is built with thread support, so the
  symbol check covers Donner's own code and the default configuration, not every third-party
  library. Recommendation: leave third-party libraries out of the check and document which ones
  keep threads, rather than patch them.
- **Peer benchmark run time.** A fresh process per repetition adds start-up and page-fault
  effects that each process's warm-up should absorb, and it multiplies run time: 15 repetitions
  times warm-up, cases, peers and platforms. Recommendation: S6 records the wall-clock cost of a
  full run, and allows fewer repetitions on cases whose results are clearly separated (no overlap
  between the peers' minimum-to-maximum ranges), keeping 15 wherever a threshold is close.
- **0061.** Recommendation: update 0061 to use this scheduler, the shared switch and the Geode
  fan-out above when its work starts, rather than editing it in this change.
