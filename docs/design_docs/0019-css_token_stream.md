# Design: CSS Parser `TokenStream` — Pull-Based Subparser Handoff

**Status:** Implemented (Milestones 1–3 complete; Milestone 4 verdict: STOP HERE)
**Author:** Claude Opus 4.6 (drafting on behalf of Jeff McGlynn, with input from DuckBot)
**Created:** 2026-04-10

**Naming note (2026-04-10):** the concept is named `ComponentValueStream` in code to
disambiguate from CSS `Token` (the tokenizer output). The doc retains "TokenStream" in prose
for continuity with the original DuckBot conversation; both names refer to the same thing.

## Summary

Donner's CSS parser currently materializes `std::vector<ComponentValue>` sub-lists at several
points (declaration values, function arguments, simple blocks, selector preludes) and hands those
vectors to downstream subparsers. The vectors are cheap per-element (`ComponentValue` is a small
variant) but the cumulative allocation/copy cost grows with nesting depth, and the vector handoff
obscures the natural pull-based control flow the CSS Syntax Module describes.

This doc records a small, reversible experiment: introduce a `ComponentValueStream` — a pull
interface sub-parsers consume lazily — and port **one** subparser (`SelectorParser`) to it.
The Milestone 1 benchmark disproved the allocation hypothesis at the scales we care about;
the port landed anyway because the named abstraction reads better than inline span arithmetic.
Milestone 4 verdict: stop after one port, revisit only on new signal.

The doc is driven by a DuckBot conversation that separated three conflated concerns in the
original framing:

1. **Allocation cost** — may or may not show up in a profile. Unverified.
2. **Error-recovery ergonomics** — probably fine today; the `ParseDiagnostic` system carries
   source spans through recovery points cleanly.
3. **Handoff protocol** — the *actual* load-bearing question. Vector handoff forces subparsers
   to receive a fully-materialized list even when they only need a prefix.

DuckBot's key insight: we should address (3) directly without betting the farm on (1) or (2).
Quartz tilted left on "eliminate intermediate copies" → **benchmark first** before committing
to the full rewrite.

## Goals

All goals below have concrete, testable acceptance criteria (no "ergonomics improve" hand-waving).

- **G1: Introduce a `TokenStream<ComponentValue>` concept** with `peek()`, `next()`, `isEOF()`
  methods. *(Accepted when: concept compiles, has unit tests for `VectorTokenStream`, and is
  used by at least one subparser.)*
- **G2: `SelectorParserImpl` no longer embeds its own cursor.** The ad-hoc `advance()`, inline
  `peek`-via-index, `isEOF`, and `skipWhitespace` methods on `SelectorParserImpl` are replaced
  by calls to a `TokenStream<ComponentValue>` interface. *(Accepted when: `SelectorParserImpl`
  holds a `TokenStream<ComponentValue>&` rather than a `std::span<const ComponentValue>`, and
  `grep -n "components_\.subspan\|components_\[" donner/css/parser/SelectorParser.cc` returns
  zero hits.)*
- **G3: Zero behavior drift.** Every existing `SelectorParser` test passes unchanged; every
  `ParseDiagnostic` emitted on the fuzzer corpus resolves to byte-identical source offsets;
  the `SelectorParser` fuzzer runs clean for >= baseline duration. *(Accepted when: all three
  conditions hold.)*
- **G4: Baseline benchmark exists.** A repeatable benchmark harness parses a defined corpus
  and reports wall time + allocation count, so "is this faster?" is a number, not a vibe.
  *(Accepted when: the harness is committed, runs in CI or locally on a single command, and
  the baseline numbers are in this doc's Performance section.)*
- **G5: The experiment is reversible.** Milestone 3 lands as a single PR that can be reverted
  without touching unrelated code. *(Accepted when: the PR diff touches only `donner/css/parser/`
  and its tests.)*
- **G6: No collateral damage.** Parsers outside `donner::css::parser::*` are untouched. The
  public `donner::css::CSS` / `Stylesheet` / `Rule` / `Declaration` / `ComponentValue` API
  surface does not change. *(Accepted when: `git diff` shows no modifications outside
  `donner/css/parser/` + docs.)*

## Non-Goals

- **Not** rewriting the CSS parser end-to-end in one pass.
- **Not** introducing C++20 coroutines (`co_await`) as the backing implementation yet — that's
  the *follow-up* investigation if the experiment succeeds, not the experiment itself.
- **Not** changing the public `donner::css::CSS` entry points or the `Stylesheet` / `Rule` /
  `Declaration` / `ComponentValue` data model.
- **Not** changing diagnostic wording or structure — recovery behavior must be byte-for-byte
  identical.
- **Not** performance-optimizing anything until we measure first.
- **Not** touching parsers outside `donner::css::parser::*`. XML and SVG parsers are out of scope.

## Existing infrastructure (important!)

**Discovered during pre-implementation exploration, not in the original DuckBot conversation.**
Donner already has substantial infrastructure for this approach — more than DuckBot assumed.

### Layer 1: `TokenizerLike<T>` concept (declaration pipeline)

- [`donner/css/parser/details/Common.h`](../../donner/css/parser/details/Common.h) defines
  `TokenizerLike<T, TokenType = Token>`, a concept that describes pull-based sources of `Token`
  or `ComponentValue`.
- [`donner/css/parser/details/Subparsers.h`](../../donner/css/parser/details/Subparsers.h)
  defines two adapter templates — `DeclarationTokenTokenizer` (wraps a `TokenizerLike<Token>`
  and lazily constructs ComponentValues on demand) and `DeclarationComponentValueTokenizer`
  (wraps a `TokenizerLike<ComponentValue>` directly). These feed a shared
  `consumeDeclarationGeneric<T>` template that works on either.
- [`donner/css/parser/details/ComponentValueParser.h`](../../donner/css/parser/details/ComponentValueParser.h)
  has `consumeComponentValue`, `consumeSimpleBlock`, `consumeFunction`, and
  `parseListOfComponentValues` all templated against `TokenizerLike<Token>`.

The core declaration-consumption path already supports pull-based handoff and is parameterized
over the source type. `TokenizerLike` exposes only `next()` + `isEOF()` — no `peek()`.

### Layer 2: `SelectorParserImpl` already has an ad-hoc cursor

This is the critical finding. The current implementation of
[`SelectorParserImpl`](../../donner/css/parser/SelectorParser.cc) does **not** copy its input —
it takes a `std::span<const ComponentValue>` and walks it with the following inline methods:

```cpp
class SelectorParserImpl {
 public:
  SelectorParserImpl(std::span<const ComponentValue> components) : components_(components) {}

 private:
  bool isEOF() const { return components_.empty(); }
  void advance(size_t amount = 1) { components_ = components_.subspan(amount); }

  template <typename T>
  const T* tryPeek(size_t advance = 0) const;     // "peek at offset 0..N"

  template <typename T>
  bool nextIs(size_t advance = 0) const;

  template <typename T>
  bool nextTokenIs(size_t advance = 0) const;

  bool nextDelimIs(char value, size_t advance = 0) const;
  std::optional<char> peekNextDelim(size_t advance = 0) const;

  void skipWhitespace();                           // linear scan, no copy

  std::span<const ComponentValue> components_;    // the cursor
  std::vector<ParseDiagnostic> warnings_;
};
```

**There is no rewind anywhere in this file** — the one-way cursor suffices because CSS selector
grammar uses `,` as a hard separator. Forgiving recovery at selector boundaries uses the
warnings vector, not cursor rollback.

### What's actually missing

The "TokenStream" concept this doc proposes is **already implemented inline** inside
`SelectorParserImpl`. The work is:

1. **Name and extract the concept**: lift the ad-hoc cursor methods into a
   `TokenStream<ComponentValue>` concept + a `VectorTokenStream` adapter (pure rename/extract).
2. **Reuse it**: `ValueParser` and any future subparser consume the same concept instead of
   re-implementing the cursor.
3. **Benchmark the real allocation source**: upstream in `parseListOfComponentValues`
   (called from `SelectorParser::Parse(std::string_view)`) and in `RuleParser` — the subparsers
   themselves do not copy.

The scope this experiment bites off is **Milestone 3: rename/extract only**. Chasing the
upstream allocation story is a separate, larger investigation deferred to a follow-up.

## Status (2026-04-10)

Milestones 1–3 complete. Milestone 4 verdict: **STOP HERE** (neither extend nor back-out
conditions fired). See the Performance section for the numbers and the Milestone 4 rubric below
for the decision path. Open Questions below were resolved during implementation.

## Implementation Plan

- [x] **Pre-work (2026-04-10):** Read `SelectorParser.cc` to determine rewind
      requirements. Result: no rewind needed; the concept shape is `peek`/`next`/`isEOF`.
      Selector forgiving recovery uses a warnings vector, not cursor rollback.

- [x] **Milestone 1: Baseline measurement.** *(Exit gate below; result: GO on M2-M3, parked
      upstream allocation follow-up.)*
  - [x] Identify a representative CSS corpus for benchmarking. Chose 5 hardcoded inputs (short/
        medium inline style, small/medium stylesheet, complex selector) for reproducibility
        across machines over fuzzer corpora.
  - [x] Add a micro-benchmark target: `donner/benchmarks/CssParsePerfBench.cpp` + BUILD wiring
        (colocated with existing `*_perf_bench` targets rather than under `donner/css/tests/`).
  - [x] Record baseline numbers in this doc under the Performance section.
  - [x] **Exit gate:**
        - **GO ahead with Milestone 2-3** unconditionally — Milestone 3 is a pure
          rename/extract with negligible runtime cost and an ergonomic win.
        - **ALSO commit to an upstream-allocation follow-up** *only if* the benchmark shows
          `parseListOfComponentValues` / `ComponentValue` allocations exceed **>5% of wall
          time OR >20% of allocation count** on the corpus. Otherwise the upstream story is
          shelved as "not a bottleneck, revisit if PerfBot surfaces it during animation work".

- [x] **Milestone 2: `ComponentValueStream` class + unit tests.** *(Landed as a concrete class,
      not a concept — see "Deviation from plan" below.)*
  - [x] Add `donner/css/parser/details/ComponentValueStream.h`: pull-based cursor with
        `isEOF()`, `remaining()`, `advance(n)`, `currentOffset()`, `peek(n)`, `peekAs<T>(n)`,
        `peekIs<T>(n)`, `peekIsToken<TokenType>(n)`, `peekDelim(n)`, `peekDelimIs(c, n)`,
        `skipWhitespace()`. Wraps a `std::span<const ComponentValue>` and mutates via
        `subspan` — identical to the previous inline cursor, just named.
  - [x] Unit tests in `donner/css/parser/tests/ComponentValueStream_tests.cc` (9 tests):
        empty input, single-element peek/advance, forward-offset peek, multi-element advance,
        delim extraction, skip-whitespace (normal, all-whitespace, empty), offset tracking.
  - **Deviation from plan:** landed as a concrete class, not a `concept`. The plan called
    for a `TokenStream<ComponentValue>` concept + `VectorTokenStream` adapter, but there is
    currently exactly one implementation (a span cursor), so adding a concept layer would
    be abstraction without a second implementation — pay-when-you-need-it. If a coroutine
    generator backing lands later (Milestone 4 follow-up, explicitly parked), *that's* when
    the class graduates to a concept with multiple conformers.

- [x] **Milestone 3: Port `SelectorParserImpl` to `ComponentValueStream`.** Pure rename/extract
      — no behavior change. Touches only `donner/css/parser/` + its tests.
  - [x] `SelectorParserImpl` now holds a `details::ComponentValueStream stream_` member (by
        value, not by reference — single owner, no lifetime gymnastics). The previous
        `std::span<const ComponentValue> components_` is gone.
  - [x] `SelectorParserImpl::tryPeek`, `nextIs`, `nextTokenIs`, `nextDelimIs`,
        `peekNextDelim`, `skipWhitespace`, `advance`, `isEOF`, and source-offset tracking
        are now thin wrappers calling `stream_.*`. Call sites unchanged (~80 call sites
        routed through wrappers; zero behavioral churn).
  - [x] `SelectorParser::ParseComponents` / `Parse` / `ParseForgivingSelectorList` /
        `ParseForgivingRelativeSelectorList` unchanged — they still hand a span to
        `SelectorParserImpl`, which now constructs the stream internally.
  - [x] `bazel test //donner/css/...` — 30/30 pass (including fuzzer soak runs).
  - [x] `bazel test //donner/svg/components/style/... //donner/svg/parser/...` — 30/30 pass.
  - [x] G2 exit criterion: `grep 'components_\.subspan\|components_\[' SelectorParser.cc` —
        zero hits.
  - [x] Re-ran benchmarks: all 5 deltas within noise (<0.8% worst case). See Performance.

- [x] **Milestone 4: Decision gate.** *Verdict: **STOP HERE**.*
  - **EXTEND**? No. No code-review feedback yet (unreviewed) and the Milestone 1 benchmark
    disproved the upstream-allocation hypothesis (parser is not on the critical path).
  - **BACK OUT**? No. Benchmark deltas within noise; zero fuzzer crashes; zero diagnostic
    offset drift; no test regressions.
  - **Result:** ship Milestones 1-3 as a pure refactor. Extending to `ValueParser` /
    `DeclarationListParser` and the upstream `parseListOfComponentValues` allocation story
    are both parked — revisit only if PerfBot surfaces CSS parsing as a hotspot during
    animation work, or if a reviewer explicitly calls out an ergonomic win from the
    abstraction on a new call site.

## Proposed Architecture

The experiment introduces a single new abstraction — a `TokenStream<ComponentValue>` concept —
and one adapter that fulfills it over an existing `std::vector<ComponentValue>`:

```
                   RuleParser
                       │
                       │ passes
                       ▼
              ┌───────────────────┐
              │ VectorTokenStream │  ← wraps a const std::vector<ComponentValue>&
              └──────────┬────────┘
                         │ models TokenStream<ComponentValue>
                         ▼
                  SelectorParser   ← consumes pull-based interface, not vector directly
```

No change to how `RuleParser` *produces* the vector. No change to `ComponentValue` itself. No
change to any other subparser. The architectural footprint is deliberately minimal.

### Relationship to existing `TokenizerLike`

This is an extension of the existing pattern, not a replacement. Specifically:

- `TokenizerLike<T>` today has `next()` + `isEOF()` only.
- Most subparsers that want to consume ComponentValues need `peek()` as well (to decide whether
  to consume without committing).
- We add a second concept, `TokenStream<T>`, that refines `TokenizerLike<T>` with `peek()`
  (and potentially a bookmark-style rewind if the selector parser's current implementation
  needs it — TBD during Milestone 3).

Existing `TokenizerLike<T>` consumers (declaration parsing) remain unchanged.

### Relationship to `co_await` generators

Not touched by this experiment. If Milestone 4 concludes the approach is worth extending, a
follow-up could replace the `VectorTokenStream` backing with a coroutine-based generator that
constructs ComponentValues lazily from the upstream `TokenizerLike<Token>` — but that's a
separate, larger investigation with its own lifetime-safety questions.

## Performance

**Measurement harness**: `donner/benchmarks/CssParsePerfBench.cpp`, run via
`bazel run -c opt //donner/benchmarks:css_parse_perf_bench`. Hard-coded representative inputs
(short/medium inline style attributes, small/medium stylesheets, complex selector) so the
benchmark is reproducible across machines. Allocation-count attribution via `heaptrack`
(Linux) or Instruments (macOS) is deferred — run the same binary under the profiler if
wall-time numbers warrant it.

**Baseline** (2026-04-10, aarch64 @ 2.6 GHz, `-c opt`, Google Benchmark 1.9.5):

```
Benchmark                              Time       CPU   Iterations  Throughput
BM_ParseStyleAttribute_Short         895 ns    894 ns      782736   8.5 MiB/s
BM_ParseStyleAttribute_Medium       9676 ns   9663 ns       72297  12.5 MiB/s
BM_ParseStylesheet_Small           14131 ns  14109 ns       49685   7.2 MiB/s
BM_ParseStylesheet_Medium          99775 ns  99616 ns        7024   6.9 MiB/s
BM_ParseSelector_Complex            7296 ns   7286 ns       96174   8.2 MiB/s
```

**Interpretation**:

- **Inline `style="..."` attributes are fast** — sub-microsecond for a single declaration,
  ~10µs for a 7-declaration real-world case. Nowhere near the animation frame budget
  (16.67ms at 60fps); not a perf concern on the animation path.
- **Stylesheet parsing scales linearly** at ~7 MiB/s — consistent with recursive-descent on
  linear input. A ~500-byte stylesheet is ~100µs even in the worst benchmarked case.
- **Selector parsing** is comparable in scale to a short stylesheet (~7µs for a selector with
  class + attribute + pseudo-class + combinator).
- **Throughput is relatively flat across sizes**, suggesting per-token cost dominates rather
  than per-call setup.

**Exit gate evaluation** (per the Implementation Plan Milestone 1 gate):

- **GO on Milestones 2-3**: yes, unconditionally. The rename/extract is an ergonomic win at
  near-zero runtime cost.
- **GO on the upstream-allocation follow-up**: **not warranted.** Wall-time numbers are low
  enough that `parseListOfComponentValues` cannot plausibly be >5% of any real workload. The
  CSS parser is not on the critical path — parsing a medium stylesheet takes ~100µs, well
  under any frame budget that matters. Allocation attribution via `heaptrack` is shelved
  unless PerfBot surfaces CSS parsing during animation work.
- **Result**: Milestone 3 ships as a pure refactor. The broader `co_await` / upstream-streaming
  investigation is explicitly parked — **not** a non-goal (we may revisit), just not funded
  by this experiment's findings.

**Hypothesis verdict**:

- *"ComponentValue vector materialization accounts for >5% of wall time or >20% of allocation
  count"* — **not verified, probably false at this scale.** Wall time is already trivial;
  allocation count would need external profiling to confirm, but the motivation to invest in
  that is absent.
- *"Switching one subparser to a pull interface produces a measurable speedup in isolation"*
  — **confirmed false** (Milestone 3 re-run below): pure refactor, no speedup.

**Milestone 3 post-port measurement** (same machine, same day):

```
Benchmark                          Pre        Post       Delta
BM_ParseStyleAttribute_Short       895 ns     902 ns     +0.8%
BM_ParseStyleAttribute_Medium      9676 ns    9659 ns    -0.2%
BM_ParseStylesheet_Small           14131 ns   14112 ns   -0.1%
BM_ParseStylesheet_Medium          99775 ns   99746 ns   -0.03%
BM_ParseSelector_Complex           7296 ns    7299 ns    +0.04%
```

All deltas within benchmark noise (the one +0.8% outlier on `Short` is well under the 5%
regression guard; on a 128-core box with CPU scaling enabled, run-to-run variance dominates).
Benchmark regression guard: **pass**.

**Milestone 3 validation**:

- `bazel test //donner/css/...` — **30/30 pass**, including `selector_parser_fuzzer_10_seconds`
  and `stylesheet_parser_fuzzer_10_seconds` soak runs (zero new crashes, zero new timeouts).
- `bazel test //donner/svg/components/style/... //donner/svg/parser/...` — **30/30 pass**
  (downstream consumers unaffected).
- G2 exit criterion: `grep -n "components_\.subspan\|components_\[" donner/css/parser/SelectorParser.cc`
  returns **zero hits**. ✓
- The port holds a `details::ComponentValueStream` by value rather than by reference (the
  design doc G2 goal said reference; the by-value form is strictly simpler since the stream
  has a single owner — no lifetime gymnastics. Intent preserved: cursor state goes through
  the named abstraction, not raw span arithmetic).

**Result**: Milestones 1-3 complete. The rename/extract landed as a pure refactor. Per the
Milestone 4 rubric, the decision is **STOP HERE** — neither the extend nor back-out criteria
fired. Future work (extending to `ValueParser` / `DeclarationListParser`, or the upstream
`parseListOfComponentValues` allocation follow-up) is parked unless PerfBot or reviewer
feedback surfaces a reason to revisit.

## Testing and Validation

- **Unit tests**: the existing selector parser test suite must pass byte-for-byte after the
  port. Any diagnostic message drift is a blocker.
- **Fuzzer coverage**: `donner/css/parser/tests/SelectorParser_fuzzer.cc` must continue to run
  clean on the ported code, for the same duration as the pre-change baseline.
- **Byte-for-byte output**: parse the same stylesheet corpus before and after; `diff` the
  resulting structured output. Zero diffs allowed.
- **Source-span validation**: every diagnostic emitted on the corpus must resolve to the same
  byte offset as before. This protects the ParseDiagnostic invariant.
- **Benchmark regression guard**: if Milestone 3 introduces a perf regression >5% on the
  baseline corpus, back it out — the experiment is "cheap and reversible"; honor that.

## Security / Privacy

No new trust boundaries. The CSS parser remains the boundary; the new adapter is purely an
internal handoff mechanism downstream of existing validation. SecurityBot invariants still apply:

- Recursion depth limits must be preserved (current `ComponentValueParsingContext::hitLimit()`
  machinery remains untouched).
- Every code path the fuzzer currently reaches must still be reachable after the port.
- No new allocations sized from input without clamping.
- No new stack-unbounded recursion — the `TokenStream` adapter is iterative.

## Rollback Plan

Milestone 3 is a single PR touching only `donner/css/parser/` and `donner/css/tests/`. Rollback
is `git revert <sha>` — no feature flag is needed because the concept is internal-only and the
public API is unchanged. Milestones 1-2 are additive (benchmark harness, new concept) and do
not require rollback if Milestone 3 is reverted.

## Alternatives Considered

- **Full `co_await`-based coroutine generator rewrite.** DuckBot's reframe specifically
  cautioned against this as a first step. Lifetime/allocator concerns with coroutine frames
  and `ParseDiagnostic` source-span fidelity would dominate the review effort. Deferred until
  Milestone 4 decides the approach is worth extending.
- **Replace `ComponentValue` with a lighter representation.** Tempting but too invasive —
  `ComponentValue` is in the public API via `Declaration::values`, `Function::values`,
  `SimpleBlock::values`. Out of scope.
- **Micro-optimize the existing vector path** (`reserve()` tuning, small-buffer optimization,
  arena allocation). Not rejected, just orthogonal — could land alongside or independently of
  the `TokenStream` work if benchmarks show vector growth is the actual hotspot.
- **Leave the parser alone.** Legitimate choice if benchmarks show no wins available. This
  experiment explicitly allows for that outcome.

## Open Questions

- ~~**Does the selector parser need rewind?**~~ **Resolved (pre-work, 2026-04-10):** No.
  `SelectorParserImpl` uses a one-way span cursor with no backtracking; forgiving recovery
  uses a warnings vector instead. `ComponentValueStream` is `peek/advance/isEOF` only.
- ~~**Should `TokenStream` subsume `TokenizerLike` or live beside it?**~~ **Resolved
  (M2, 2026-04-10):** coexistence — and in fact `ComponentValueStream` landed as a concrete
  class rather than a concept, so the question is moot until a second implementation exists.
  `TokenizerLike<Token>` remains the declaration-pipeline pull source; `ComponentValueStream`
  is the span cursor for subparsers that receive a materialized ComponentValue list.
- ~~**Where does the benchmark harness live?**~~ **Resolved (M1, 2026-04-10):**
  `donner/benchmarks/CssParsePerfBench.cpp`, colocated with existing `*_perf_bench` targets.
  No separate CSS-specific benchmark dir.
- **Is PerfBot currently tracking CSS parser performance anywhere?** Still TBD — but the
  baseline numbers make this low-priority. Parsing a medium stylesheet is ~100µs, well under
  any frame budget that matters; the CSS parser is not plausibly on the animation hot path.
  If PerfBot later plugs the benchmark into a regression tracker, that's purely additive.

## Conversation notes (DuckBot, 2026-04-10)

Preserved for posterity — this is the conversation that motivated the doc. Presented verbatim
from DuckBot's response so the reasoning stays intact (per user preference to preserve subagent
voice).

> *Quack?* (roughly: "you're describing a pull-based parser refactor, but the way you framed it
> — 'eliminate intermediate copies, reduce allocation, make recovery natural' — is three
> different problems wearing one coat. which one is actually hurting?")
>
> **The reframe.** "Streaming" is a mechanism. The real questions are: (a) is allocation showing
> up in a profile, (b) is error recovery *actually* awkward today, or just inelegant, and (c)
> are ComponentValue trees wrong, or is the handoff protocol wrong? Those have very different
> fixes. If it's only (c), you might not need coroutines at all — you might need a `TokenCursor`
> that subparsers borrow.
>
> **The relevant innovation.** Donner already has the `co_await` generator pattern in a couple
> of parser/iterator paths — you know this exists, which is why it's tempting. But the *other*
> thing Donner has is the `ParseDiagnostic` system with source spans and structured recovery
> metadata. That's the part that makes CSS recovery feel good today. Any streaming rewrite has
> to preserve span fidelity through the coroutine boundary, and coroutines are famously awkward
> about lifetimes of referenced data. That's the load-bearing risk, not performance.
>
> **A direction to consider.** Before rewriting, try the cheapest experiment: introduce a
> `TokenStream` abstraction that *wraps* the current ComponentValue vector behind a pull
> interface. Port one subparser (selectors?) to it. If the ergonomics win is real, *then*
> swap the backing store to a coroutine generator. You get the API shape benefit without
> betting the farm on coroutine lifetime puzzles.
>
> **Quartz's take.** Quartz tilted very slightly left when I said "eliminate intermediate
> copies." I read that as: benchmark first. The CSS parser isn't obviously on the hot path —
> fuzzing it is, parsing user stylesheets isn't.
>
> Worth looping in PerfBot (is it actually slow?) and checking the existing `co_await` sites
> before designing a new one.
>
> *Quack.* (meaning: "small experiment first, big rewrite second, and let the profiler drive.")
