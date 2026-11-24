# Design: CSS Cascade Performance, Custom Properties, Layers, Live Editing and Parallelism

**Status:** Draft
**Author:** Claude Fable 5.2
**Drafted by:** Claude Opus 5.5
**Requested by:** Jeff McGlynn
**Created:** 2026-11-23
**Tracking:** [#2660](https://github.com/jwmcglynn/donner/issues/2660)
**Depends on:** [0073-task_scheduler](0073-task_scheduler.md) for Part D

## Summary

Donner resolves style by testing every rule of every applicable stylesheet against every element,
then re-parsing each matched declaration into the element's `PropertyRegistry`. Any edit to a class,
`id`, `style` attribute, presentation attribute or stylesheet restyles the whole document. Each
element holds its own computed-style record, a 4,840-byte `PropertyRegistry`, even when hundreds of
elements compute the same style. Custom properties (`var()`) and cascade layers (`@layer`) are not
supported. Typing in a `<style>` element reparses the whole stylesheet and restyles the whole
document, and the source editor has no CSS-aware highlighting or value completion.

This design measures that cost, surveys how browser engines make style resolution fast and how
they implement the missing features, and commits to
four parts:

**Part A: Performance.**

1. Cascade each element once per full restyle (Milestone A2).
2. A per-stylesheet rule index keyed on the rightmost compound selector, so an element tests only
   the rules that can match it (Milestone A3).
3. Constant-factor fixes in the selector matcher, each kept only if its benchmark moves.
4. Narrow style invalidation for single-element edits (class, `id`, `style`, presentation
   attributes) from the whole document to the elements whose style can change.
5. An ancestor Bloom filter that rejects descendant and child selectors without walking the tree,
   built on a reusable Bloom filter in `donner/base/`.
6. Shared immutable computed-style records and a matched properties cache, so elements with the
   same cascade inputs share one computed style.

**Part B: Cascade features.** CSS custom properties with `var()` substitution (cycles, fallback,
guaranteed-invalid values, inheritance, hostile-input limits) and cascade layers (`@layer`
blocks and statements, nested and anonymous layers, `@import ... layer()`, importance reversal).

**Part C: Live stylesheet editing.** Rule-level incremental stylesheet reparse, so a keystroke in
a `<style>` element restyles only the elements matched by the rules it changed, and a scoped plan
for CSS syntax highlighting and completion in the editor's source pane.

**Part D: Parallel style traversal.** Cascade independent subtrees on worker threads, as the first
client of the work-stealing task scheduler designed in
[0073-task_scheduler](0073-task_scheduler.md), with results bit-identical to the serial path and a
measured threshold below which style stays serial.

Computed style stays bit-identical for documents that use only today's features. On the measured
documents style is 47 to 89 percent of preparing the first frame without rasterization, and 80 to
95 percent of preparing the frame after a one-element edit.

## Goals

All numbers are against the baseline in [Measured Baseline](#measured-baseline), measured with the
same harness, machine class and method. Each goal names the counter or timer that decides it.

### Part A: Performance

- **One cascade per element per full restyle.** A frame that follows a full restyle cascades each
  styled entity once. Met: after a one-element class edit the splash document cascades its 622
  styled entities and the bootstrap-icons sprite its 5,137.
- **Rules tested per element.** On `donner_splash.svg` (89 author rules, 4 user agent rules),
  rules tested per first frame fall from 57,838 to at most 5,000 (from 93 per element to at most
  8). On the synthetic 2,000-element, 200-rule document they fall from 408,608 to at most 20,000.
  Met: 1,384 on the splash document (2.2 per element) and 3,543 on the synthetic document.
- **First-frame style time.** On `donner_splash.svg` style time falls from a 10.8 ms median to at
  most 5 ms. On the synthetic 2,000-element, 200-rule document it falls from 77.6 ms to at most
  15 ms. Style time on the rule-free synthetic and bootstrap-icons documents does not regress by
  more than 5 percent before the matched properties cache lands, and on bootstrap-icons falls from
  31.5 ms to at most 15 ms after it. Met at the median with the rule index on the baseline machine
  class under load (one-minute load average 30 to 59): 3.60 ms on the splash and 14.94 ms on the
  synthetic document, whose four runs range from 14.60 to 15.48 ms, so the synthetic goal holds at
  the median but not in every run on a loaded host; the rule-free and bootstrap-icons documents are
  faster (see the A3 entry in the [Implementation Plan](#implementation-plan)).
- **Edit cost.** A class, `style` or presentation-attribute write on one leaf element whose new
  and old values no selector depends on in a combinator position cascades at most the element, its
  descendants and its `<use>` instances, and, when none of the restyled elements is text or
  references a paint server, marker, clip path, mask or filter before or after the edit, the frame
  takes the retained update instead of the full rebuild. On the splash document the style time of
  the next frame falls from 18.8 ms to at most 1 ms, and on the bootstrap-icons sprite from 63 ms to
  at most 1 ms.
- **Ancestor filter.** On the synthetic descendant-only document (1,000 elements, 100 rules), the
  new counter of candidates rejected by the filter reaches at least 857 of the 902 candidates that
  today fail after an ancestor walk (95 percent), with no false negative (enforced by the
  differential tests). Target, not yet measured: style time on the real documents does not
  regress by more than 2 percent, checked by quiet-machine runs of the perf benchmark (the
  baseline was taken under load, so a 2 percent bound needs a quieter measurement than this
  document's). On the real documents only one candidate per document fails in an ancestor walk,
  so no time gain is expected there (see
  [Ancestor Filter Opportunity](#ancestor-filter-opportunity)).
- **Shared computed styles.** With the matched properties cache, the share of elements whose
  computed style is reused reaches at least 60 percent on the splash document (opportunity
  measured at 66 percent) and at least 99 percent on bootstrap-icons (measured at 99.8 percent).
  Retained computed-style records on bootstrap-icons fall from 5,137 (24.9 MB of inline
  `PropertyRegistry` storage) to at most 50.
- **Budget headroom.** The rule-element match budget charges rules actually tested, so the
  synthetic 4,000-element, 1,000-rule document, which would exhaust the 1,048,576-match budget if
  every rule were tested, styles completely under unchanged limits. Met: it tests 7,102 candidates
  and matches 6,555 rules.
- **No behavior change.** For every document whose cascade completes within its budgets today,
  every computed style is identical before and after each Part A step, enforced by differential
  tests and fuzzers (see [Testing and Validation](#testing-and-validation)). A document whose
  cascade ends budget-rejected today can style further, because the rule index and the ancestor
  filter skip work for rules that cannot match; that is the one intended behavior change (see
  [Decisions](#decisions)). A full-restyle frame computes the first frame's styles, so a `:has()`
  on an ancestor of a `<use>` element does not match the instanced content on either; a frame
  that takes the retained update can still match it
  ([#2715](https://github.com/jwmcglynn/donner/issues/2715); see
  [Single Cascade per Full Restyle](#single-cascade-per-full-restyle)).
- **Bounded memory.** The rule index costs at most 16 bytes per selector-list entry plus shared
  key strings; the ancestor filter costs 4 KiB per style traversal; both and the cache are
  charged to existing document budgets.

### Part B: Cascade Features

- **Custom properties.** `--*` declarations cascade and inherit, and `var()` substitutes them in
  stylesheet rules, `style` attributes and presentation attributes with fallback, cycle detection
  and guaranteed-invalid handling as specified. Done when the SVG conformance cases ported from the
  web-platform-tests files named in [Part B tests](#part-b-tests) pass.
- **Cascade layers.** `@layer` blocks and statements, nested and anonymous layers and
  `@import ... layer` order declarations as Cascade Level 5 specifies, including importance
  reversal and presentation attributes below every author layer. Done when the ported layer cases
  pass.
- **Hostile input.** A substituted value is capped at 65,536 component values and 1 MiB of source
  bytes; past the cap the value is the guaranteed-invalid value. The exponential-expansion and
  long-reference cases finish within the existing style budgets.
- **No cost when unused.** On documents with no custom properties and no layers, computed styles
  are unchanged. Target, not yet measured: style time does not regress by more than 2 percent, in
  quiet-machine perf runs.
- **Intended behavior changes, stated.** Documents that use these features today get different
  results, by design: a declaration containing `var()` is dropped today, so
  `fill: red; fill: var(--missing)` renders red, and after Part B it is invalid at computed-value
  time and `fill` inherits; rules inside `@layer` blocks, dropped today, start applying; and imports
  declared with `layer` or `layer()`, dropped today, start loading.

### Part C: Live Stylesheet Editing

- **Keystroke cost.** A one-character edit inside a declaration value of the splash document's
  `<style>` element restyles at most the elements the edited rule matched before or after the
  edit, plus their descendants and `<use>` instances. Next-frame style time falls from 19.2 ms to
  at most 1 ms and the next null-backend frame from 23.2 ms to at most 5 ms; the source edit call
  stays at or under its 1.3 ms median.
- **Equal to a full parse.** After any edit, the incrementally updated stylesheet equals a fresh
  parse of the new text, enforced by a differential fuzzer; the document is never reparsed.
- **Highlighting and completion.** Every byte of CSS inside `<style>` elements and `style`
  attributes is classified from the CSS tokenizer, and completion offers selectors, property
  names, keyword values, `var()` names and supported at-rules from live data sources (see
  [CSS Highlighting and Completion](#css-highlighting-and-completion)).

### Part D: Parallel Style Traversal

- **Speedup where it pays.** Target, not yet measured: with four scheduler workers and the
  shared computed-style storage of Milestone A8, first-frame style time on the bootstrap-icons
  sprite (5,137 elements) and the synthetic `<use>` document (4,425 elements) is at most half of
  the same build's serial style time. Without A8 the serial placeholder step caps the speedup near
  2.3x (see [What Must Hold](#what-must-hold-for-parallel-cascading)), which is why Part D depends
  on A8.
- **No cost where it does not.** Passes below the serial threshold (an initial target of 1,000
  elements to cascade, calibrated in Milestone D3) run the serial path. Target, not yet measured:
  style time on the splash and tiger documents does not regress by more than 2 percent with a
  scheduler present, in quiet-machine perf runs.
- **Bit-identical.** Computed styles, matched-rule lists, warnings and budget outcomes are the
  same at 0, 1, 2, 4 and 8 workers and under the scheduler's seeded-schedule mode, enforced by the
  differential tests.

## Non-Goals

- No change to computed style, cascade order or selector semantics for documents that use only
  features supported today. Known deviations found while doing this work are reported separately
  (see [Decisions](#decisions)).
- No selector JIT or code generation.
- No parallel traversal of anything but style in this design: the scheduler's other clients are
  named in [0073-task_scheduler](0073-task_scheduler.md) and designed separately. No parallel
  style for animated documents or external-use documents, which keep the serial path.
- No `@property` registration. Registered custom properties add typed values, computed-value
  resolution of lengths and their font-size dependency cycles, and non-inherited custom
  properties; untyped custom properties cover `var()` without them. A later design can add it.
- No `env()`, `attr()` or other substitution functions, and no `@media`, `@supports`,
  `@container` or `@scope` (unsupported today and unchanged).
- No SMIL animation of custom properties.
- No change to the public SVG DOM API (`SVGDocument`, `SVGElement` and the element classes). The
  rule index, ancestor filter, cache, counters and invalidation summaries are internal to
  `donner/svg/components/style/`. Two public library surfaces do change, and are reviewed as API:
  Part A adds the header `donner/base/BloomFilter.h` to the public `//donner/base` target, and
  Part B changes public `donner/css` types (`css::Specificity` gains a layer rank and accessor,
  `css::SelectorRule` a layer id, `css::Stylesheet` a layer table and `css::Declaration` a
  `containsSubstitution` flag). The task scheduler adds `donner/base/tasks/`, designed in
  [0073-task_scheduler](0073-task_scheduler.md).
- No full document reparse, and no new trigger for one.
- Narrowing invalidation for element insertion or removal; listed under
  [Future Work](#future-work).
- In the editor, no visual design beyond the classification and completion data, and no CSS
  formatting, linting, hover documentation, color pickers or browser-compatibility data.
- Raising the XML parser's default 8,192-node cap, which bounds the element count of documents
  parsed with default options.

## Next Steps

- Land narrowed invalidation (Milestone A5) and the matcher constant factors (Milestone A4), which
  follows the rule index.

## Implementation Plan

Each milestone is one independently reviewable PR unless noted. Estimates are engineering days,
including tests and review fixes. "After" names the milestones that must land first.

### Part A Milestones

- [x] Milestone A1: Cascade counters and benchmark (1.5 days,
      [#2699](https://github.com/jwmcglynn/donner/pull/2699))
  - [x] Add internal counters to `StyleSystem` (elements cascaded, rules tested, rules matched,
        declarations applied, full clears, budget rejections, and later the filter and cache
        counters), readable through the existing `RenderingDiagnostics` snapshot. They are
        `StyleCascadeCounters` in the document registry's context and only grow: the work of a
        frame or edit is the difference between a capture taken after it and one taken before
        it. The same capture carries the style time.
  - [x] Add `//donner/benchmarks:css_cascade_perf_bench` with the documents and edits in
        [Reproducing the Baseline](#reproducing-the-baseline).
  - [x] Add `//donner/benchmarks:css_cascade_perf_tests`, a `donner_perf_cc_test` whose
        correctness target asserts the counter values for fixed documents and edits
        (CPU-invariant, PR-gated) and whose wall-clock target records times (manual, `perf` tag).
  - Done when: the counters reproduce this document's counter columns exactly for the splash,
    bootstrap-icons and synthetic documents.
- [ ] Milestone A2: One cascade per full restyle (1 day,
      [#2717](https://github.com/jwmcglynn/donner/pull/2717)). After A1.
  - [x] Clear computed styles once per rebuild in `RenderingContext::createComputedComponents`
        (`StyleSystem::discardComputedStylesForFullRestyle`), and make both of its
        `StyleSystem::computeAllStyles` calls compute only entities without a computed style
        (`FullRestyle::ComputeMissing`), so the second computes only the shadow-tree clones the
        first could not see.
  - [x] Establish with a test, before the change, whether `ApplyAnimationPresentation` depends on
        the second pass discarding its overrides: it does not, so animated documents take the same
        path (`RenderingContextTest.AnimatedDocumentFullRestyleMatchesFirstFrame` in
        `//donner/svg/renderer/tests:renderer_driver_tests`).
  - Done when: the correctness counter test shows elements cascaded equal to the styled entity count
    on the edit frames, and the next-frame style time after a splash class edit is at most 10 ms
    (from 18.8 ms). Met: every edit and keystroke row of `css_cascade_perf_tests_correctness`
    cascades each styled entity once with one full clear (see
    [Measured Baseline](#measured-baseline)). Not met: on the baseline machine class (the same harness pinned
    to one core, load average 27 to 36, four alternating runs of the old and new binaries) the
    splash class edit's style time falls from 17.93 ms (17.10-18.54) to 10.67 ms (10.45-10.72), and
    on a 14-core Apple silicon desktop (load average 4.7 to 8.0, six alternating runs) from
    4.33 ms (4.28-4.40) to 2.49 ms (2.45-2.52). The time falls roughly by the share of cascades
    removed, 447 of 1,069 (the 175 shadow-tree clones were already cascaded once), not by half; the
    remaining 0.7 ms is left to the rule index (Milestone A3).
- [x] Milestone A3: Rule index (4 days,
      [#2730](https://github.com/jwmcglynn/donner/pull/2730)). After A1.
  - [x] Build `RuleIndex` for a stylesheet's own rules and for each imported sheet when
        `StylesheetComponent::rebuildSelectorCaches` runs, and charge it with the stylesheet.
  - [x] Collect candidates per element in `StyleSystem::applyStylesheetRules` and
        `collectMatchedStyleRules`, each bucket at most once per element; charge the budget per
        candidate.
  - [x] Add the differential test (`//donner/svg/renderer/tests:style_index_differential_tests`),
        the differential fuzzer (`//donner/svg/components/style:style_cascade_differential_fuzzer`)
        and the key unit tests (`//donner/svg/components/style:rule_index_tests`).
  - Done when: rules tested and style time meet the load goals, the differential test and fuzzer
    are green, and the Wasm size check passes with its current ceiling. Met: rules tested as in
    the goals; on the baseline machine class (the benchmark pinned to one core, load average 30 to
    59, four alternating runs of the previous and new binaries) first-frame style time is 3.60 ms
    on the splash (from 10.45 ms), 14.94 ms (runs 14.60 to 15.48 ms) on the synthetic 2,000 x 200
    document (from 76.94 ms), 29.08 ms on bootstrap-icons (from 30.69 ms) and 5.31 ms on the
    rule-free 1,000-element document (from 6.03 ms); on a 14-core Apple silicon desktop (load
    average 5.8 to 8.1, six alternating runs) 0.87, 3.95, 8.51 and 1.32 ms (from 2.55, 20.17, 8.86
    and 1.44 ms). The differential test, the fuzzer and the Wasm size checks pass: the renderer's
    Wasm is 2,340,052 bytes raw and 925,917 gzipped (ceilings 2,500,000 and 1,020,000), the editor's
    10,623,727 and 3,385,346 (ceilings 10,880,000 and 3,570,000).
- [ ] Milestone A4: Matcher constant factors (2 days, one commit per change). After A3.
  - [ ] Remove the per-attempt heap allocation of the backtracking stack in
        `ComplexSelector::matchChain`.
  - [ ] Store each complex selector's specificity at parse time instead of recomputing it in
        `ComplexSelector::matches`.
  - [ ] Match `ClassSelector` against a class list split once per element instead of copying and
        splitting the `class` string for every class selector tested.
  - [ ] Resolve pseudo-class names to an enumeration at parse time.
  - Done when: each kept change shows a median improvement of at least 5 percent in candidate
    match time on the benchmark; a change that does not is dropped.
- [ ] Milestone A5: Narrowed invalidation for single-element edits (9.5 days). After A2.
  - [ ] Compute a `SelectorDependencySummary` per stylesheet at parse time, including the
        directional dependencies of filtered `:nth-child` and `:nth-last-child` selectors.
  - [ ] Route class, `id`, `style` and presentation-attribute writes in `SVGElement` through it,
        marking the affected entities dirty for the existing selective style pass instead of
        setting `RenderTreeState::needsFullStyleRecompute`.
  - [ ] Restyle the `<use>` shadow instances of a written element.
  - [ ] Make a style-only change eligible for the retained update (`FlagsAreRetainable` in
        `RetainedRenderTreeUpdate.cc`) and recompute the style-derived components of the
        restyled entities, falling back to the full rebuild where they cannot be recomputed
        locally (see [Retained Update](#retained-update-for-style-only-changes)).
  - [ ] Add the narrow-versus-full differential edit test, comparing computed styles, render-tree
        and resolved-reference state, and rendered output, and asserting the retained-update and
        full-rebuild counters.
  - Done when: the edit goals are met, the differential edit test is green on all three
    comparisons for every edit routed to the narrowed path, and every eligible edit in it took
    the retained update.
- [x] Milestone A6: Base-library Bloom filter (2 days,
      [#2735](https://github.com/jwmcglynn/donner/pull/2735)). No dependency.
  - [x] Add `donner/base/BloomFilter.h` with `BloomFilter`, `CountingBloomFilter` and `BloomHash`,
        unit tests in `//donner/base:base_tests` and `//donner/base:bloom_filter_fuzzer`.
  - Done when: the no-false-negative property test, the counting-removal and saturation tests,
    the deterministic false-positive-rate test and the fuzzer are green.
- [ ] Milestone A7: Ancestor filter in the cascade (3 days). After A3 and A6.
  - [ ] Store up to four ancestor hashes per selector-list entry in `RuleIndex`.
  - [ ] Maintain the filter with `StyleAncestorFilter` in every traversal mode and check it
        before `MatchSelectorRule`; count filter rejections.
  - [ ] Run the differential test and fuzzer with the filter on and off, including the
        low-traversal-limit case.
  - Done when: the ancestor-filter goal is met and the differential targets are green.
- [x] Milestone A8: Per-element cost and shared computed-style storage (4 days,
      [#2743](https://github.com/jwmcglynn/donner/pull/2743)). After A1.
  - [x] Break down the measured 4.6 microseconds of per-element style work outside rule matching
        with timers (see [Per-Element Cost](#per-element-cost)), and remove avoidable
        whole-`PropertyRegistry` copies in `computePropertiesInto`, `inheritComputedParent` and
        `inheritProperties`: the cascade builds the record in place, inherits into it with
        `PropertyRegistry::inherit`, and copies only the element's local declarations, counted in
        `StyleCascadeCounters::registryCopies`.
  - [x] Store computed styles as `std::shared_ptr<const ComputedStyle>` records and move every
        reader to the const accessor.
  - Done when: style time on the rule-free 1,000-element synthetic document falls from 6.1 ms by
    at least 20 percent, or the breakdown shows the cost is inherent and the result is recorded
    here; all existing suites are green on the shared storage. Met: on the baseline machine class
    (the benchmark pinned to one core, load average 38 to 45, five alternating runs of the
    previous and new binaries) style time on that document is 2.83 ms (2.72-2.86), from 5.39 ms
    (5.16-5.54) with the rule index alone, 54 percent below the 6.1 ms baseline; bootstrap-icons
    falls from 29.68 to 16.26 ms and the splash from 3.73 to 2.24 ms. On an Apple silicon desktop
    (load average 20 to 23, six alternating runs) the document takes 0.71 ms (0.68-0.75), from
    1.50 ms (1.38-1.57). Computed styles, warnings and cascade counts are unchanged on every
    document of the resvg suite, the renderer test data, the splash and the sprite.
- [ ] Milestone A9: Matched properties cache (3.5 days). After A3 and A8.
  - [ ] Key, look up and insert shared records in `computePropertiesInto`; count hits and
        uncacheable elements; charge retained bytes per record; evict by clock.
  - [ ] Run the differential test and fuzzer with the cache on and off.
  - Done when: the shared-computed-style goals and the bootstrap-icons style-time goal are met and
    the differential targets are green.

Part A estimate: 30.5 engineering days.

### Part B Milestones

- [ ] Milestone B1: Custom property declarations and storage (4 days). After A8.
  - [ ] Route `--*` declarations from rules and `style` attributes into a per-element custom
        cascade instead of the "Unknown property" diagnostic, and store computed custom
        properties as shared immutable maps in the computed-style record.
  - [ ] Charge each map and token-sequence allocation to `RetainedStyleBytes` through its own
        deleter.
  - Done when: custom properties cascade and inherit in unit tests, computed styles of the
    existing corpus are unchanged, and the shared-map release regression is green.
- [ ] Milestone B2: `var()` substitution (4 days). After B1.
  - [ ] Mark declarations containing `var()` at parse time, hold them as pending substitutions in
        the cascade, substitute after the custom properties are computed, and parse the result;
        detect cycles and enforce the size caps.
  - Done when: the ported cycle, fallback, keyword, invalid-at-computed-value-time and
    exponential-expansion cases pass.
- [ ] Milestone B3: `var()` in presentation attributes and shorthands (2 days). After B2.
  - Done when: the ported presentation-attribute and shorthand cases pass.
- [ ] Milestone B4: `@layer` and `@import ... layer` parsing (3.5 days). No dependency.
  - [ ] Parse layer statements and blocks (nested, dotted, anonymous) and assign each retained
        rule a layer; compute the document's layer order.
  - Done when: parser tests and `stylesheet_parser_fuzzer` cover every form, and rules inside
    `@layer` blocks are retained instead of dropped.
- [ ] Milestone B5: Layer order in the cascade (2.5 days). After B4.
  - [ ] Build the document layer tree, assign post-order ranks, add the layer rank to
        `css::Specificity` with importance reversal, place presentation attributes below every
        author layer, and check `Property::inheritFrom` with layered declarations.
  - [ ] Record per-stylesheet layer streams with import markers and the import origin of each
        flattened imported sheet, and build the tree from those streams in source order.
  - [ ] Cover nested layers declared out of order (`@layer a, b;` then `@layer a.x`), a layer
        statement before a layered import whose sheet sets the same property (the `a, b` example
        under [Cascade Layers](#cascade-layers)), layers extended in a later stylesheet, an import
        that loads after the first frame, and rank recomputation after an edit adds a layer.
  - Done when: the ported layer cases pass and computed styles of documents without layers are
    unchanged.
- [ ] Milestone B6: Conformance cases (2 days). After B3 and B5.
  - Done when: the full set in [Part B tests](#part-b-tests) is committed and green.

Part B estimate: 18 engineering days.

### Part C Milestones

- [ ] Milestone C1: Rule-level incremental stylesheet reparse (5.5 days). No dependency.
  - [ ] Carry the source edit to the style projection: `SVGDocument::applySourceEdit` passes the
        `XMLSourceDelta` it already returns alongside the `NodeValueChanged` mutation, and the
        projection maps it to stylesheet offsets through the stylesheet's source map.
  - [ ] Add `StylesheetComponent::applyTextEdit` that reparses the window of rules an edit
        touches and splices the result, extending the window until it resynchronizes and falling
        back to a full stylesheet reparse where the edit cannot be mapped.
  - [ ] Add the incremental-versus-full differential fuzzer.
  - Done when: the fuzzer is green and `ProjectStyleContents` uses the incremental path for
    `<style>` text edits.
- [ ] Milestone C2: Restyle only elements affected by changed rules (3 days). After C1, A3 and A5.
  - Done when: the keystroke goals are met and the differential edit test covers stylesheet
    edits.
- [ ] Milestone C3: CSS highlighting in the source pane (2 days). No dependency.
  - Done when: every CSS byte in `<style>` elements and `style` attributes of the test corpus gets
    a CSS class from the tokenizer, in a headless `TextEditorCore` test.
- [ ] Milestone C4: CSS completion (3 days). After C3; `var()` names after B2.
  - Done when: each completion context in
    [CSS Highlighting and Completion](#css-highlighting-and-completion) has a headless test
    returning suggestions from its data source.

Part C estimate: 13.5 engineering days.

### Part D Milestones

- [ ] Milestone D0: Read-only cascade signatures (2 days). After A8.
  - [ ] The cascade takes `Registry&` and `EntityHandle` throughout, `ShadowedElementAdapter` holds
        a `std::reference_wrapper<Registry>`, and `GetStyleResourceBudget` emplaces into the
        registry context. Change the read paths (`applyStylesheetRules`, `MatchSelectorRule`, the
        adapter, `inheritProperties`, `computePropertiesInto`) to take `const Registry&`, route the
        only write (the computed-style slot) through a narrow writer, and move context creation
        into the pre-region step. Serial behavior is unchanged.
  - Done when: the existing style suites and the differential tests are green, and the parallel
    path compiles against the const view only.
- [ ] Milestone D1: Parallel style pass (4 days). After D0, A7, A9 and scheduler milestones S1 to
      S3.
  - [ ] Pre-region step: create every pool and context object, pre-emplace computed-style slots
        for dirty entities and their missing ancestors, find the roots of dirty subtrees.
  - [ ] Decompose the full and selective passes into subtree tasks on the scheduler, with a
        `const Registry&` view for workers, per-worker budget shares, warning buffers, ancestor
        filters and caches, a serial commit of warnings, the serial fallback on budget pressure,
        and a debug guard that pool sizes are unchanged across the region.
  - Done when: the differential tests are green at every worker count and the serial threshold
    keeps the splash and tiger documents on the serial path.
- [ ] Milestone D2: Race and determinism coverage (2 days). After D1.
  - [ ] Add `//donner/svg/components/style:style_parallel_tests` to the scheduler's ThreadSanitizer
        lane, run the differential corpus under the seeded-schedule mode, and add an
        injected-delay test that is red with a deliberately broken commit order.
  - Done when: the lane is green, and the injected-delay test was observed red with the broken
    order and green with the real one.
- [ ] Milestone D3: Threshold and grain calibration (1 day). After D2.
  - Done when: the threshold and subtree grain are set from a measured curve recorded here, the
    deepest stack use of a cascade task is measured and declared as this client's stack bound to
    the scheduler, and the Part D speedup goal is met.

Part D estimate: 9 engineering days.

### Order and Total

A1 first; A2, A3, A6, A8, B4, C1 and C3 can then proceed in parallel; A4 follows A3; A7 follows
A3 and A6; A5 follows A2; A9 follows A3 and A8; B1 to B3 follow A8; B5 follows B4; B6 closes Part B;
C2 follows C1, A3 and A5; C4 follows C3 and, for `var()` names, B2; D0 follows A8; D1 follows D0,
A7, A9 and the scheduler milestones S1 to S3 of [0073-task_scheduler](0073-task_scheduler.md); D2
and D3 follow D1. Total committed estimate for this document: 71 engineering days (A 30.5, B 18,
C 13.5, D 9); the scheduler is estimated in 0073.

## Background

### Donner Today

**CSS model.** `donner::css` parses stylesheets into `css::Stylesheet`, a vector of
`css::SelectorRule` (a `css::Selector` list plus `css::Declaration`s with source ranges).
`StylesheetComponent` holds a `<style>` element's sheet, its flattened `@import`ed sheets, and
parse-time caches built by `StylesheetComponent::rebuildSelectorCaches`: the attribute names used
by attribute selectors and whether any selector depends on text content. The user agent sheet,
`kUserAgentStylesheet` in `donner/svg/core/UserAgentStylesheet.h`, contributes 4 rules that are
tested against every element.

**Selectors.** `css::ComplexSelector::matches` matches right to left with an explicit backtracking
stack (`matchChain` and `CandidateCursor`), allocated per attempt. Supported: type, universal,
namespace, id, class, attribute selectors (`=`, `~=`, `|=`, `^=`, `$=`, `*=`), the descendant,
child, next-sibling and subsequent-sibling combinators, and the pseudo-classes `:root`, `:scope`,
`:defined`, `:empty`, `:first-child`, `:last-child`, `:only-child`, `:first-of-type`,
`:last-of-type`, `:only-of-type`, `:nth-child`, `:nth-last-child` (both with an optional
`of S` filter), `:nth-of-type`, `:nth-last-of-type`, `:is`, `:where`, `:not` and `:has`.
Pseudo-class names are compared as strings while matching (`PseudoClassSelector`).
`ClassSelector::matches` copies the element's `class` string and splits it for each class
selector tested. Every traversal step is charged to `css::SelectorTraversalBudget`.

**Matching.** `StyleSystem::applyStylesheetRules` iterates every `StylesheetComponent`, each
imported sheet, then the sheet's own rules, and calls `MatchSelectorRule` for each of the
element's candidates from the sheet's rule index (see [Rule Index](#rule-index)), in rule order.
`MatchSelectorRule` returns the first selector-list entry that matches, with that entry's
specificity. Each candidate is charged to `StyleResourceBudget::reserveRuleElementMatch` before the
selector runs, so the budget counts rules tested, not rules matched.

**Cascade.** There is no sort. `computePropertiesInto` starts from the element's
`StyleComponent::properties`, which hold presentation attributes at specificity `(0,0,0)` and
`style` declarations at `Specificity::StyleAttribute()`. Matched rules are applied in document
order; each declaration is parsed from its component values (`PropertyRegistry::parseProperty`)
and replaces the stored value unless that value has a higher `css::Specificity`. Because equal
specificity replaces, the later rule wins ties, so applying in source order with a monotonic
comparison yields the cascade order. `css::Specificity::SpecialType` encodes origin and
importance (user agent, author, style attribute, animation, important variants, override).
A declaration whose name starts with `--` reaches `PropertyRegistry::parseProperty`, which reports
"Unknown property" and drops it; `var()` is not recognized anywhere. The stylesheet parser keeps
`@layer` statements only for import ordering and drops `@layer` blocks with their rules, and it
drops imports that name a layer ("CSS @import cascade layers are unsupported").
`AtRuleEndsImportSection` in `StylesheetParser.cc` ends the import section at an `@namespace`
statement or at any block at-rule it lists (an `@layer` block among them); an `@layer` statement
does not end it, as Cascade Level 5 allows layer statements before imports. Retained imports are
loaded later and stored flattened in `StylesheetComponent::importedStylesheets`, each imported
sheet after the sheets it imports and all of them before the importing sheet's own rules; the
position of an import relative to the `@layer` statements around it is not kept. `css::Declaration`
already stores its value as a `std::vector<ComponentValue>`, which is the token sequence custom
properties need.

**Computed style.** `computePropertiesInto` builds each computed style in a new `ComputedStyle`
record, the computed `PropertyRegistry` and the resource opacities, that starts as a copy of the
element's local `StyleComponent::properties`. After the cascade, `ApplyAnimatedStyle`, contextual
font sizes, scalar and paint animation styles and symbol display are applied, then
`inheritComputedParent` inherits the parent's computed properties into the record in place
(computing the parent first if needed), and fonts are resolved to absolute values against the
parent. The record is then stored as `ComputedStyleComponent::style`, a
`std::shared_ptr<const ComputedStyle>`, and never modified: computing a style again stores a new
record. It is memoized: `computePropertiesInto` returns early when `style` is set.
`sizeof(PropertyRegistry)` is 4,840 bytes in the measured build, so every styled entity owns a
record of about 4.8 KiB on the heap, plus heap storage for strings and lists; the component itself
is 24 bytes.

**Computed-style readers.** Outside `donner/svg/components/style/`, 33 production files reference
`ComputedStyleComponent`: the renderer (`RendererDriver`, `RenderingContext`,
`ExternalUseStyleRetention`, `RetainedRenderTreeUpdate`, `AnimationPresentation`), the compositor,
text (`TextSystem`, `TextEngine`), layout, shape, paint, filter, shadow-tree and animation
systems, `SVGElement`, `RenderingDiagnostics` and one editor header. Those that access the
component read through the const accessors `properties()` and `resourceOpacities()`, which return
null while the style is not computed. The only writer of a record is `computePropertiesInto`
(including its budget-refusal branch), before it stores the record;
`ShadowTreeSystem::createShadowEntity` emplaces an empty component, and `ExternalUseStyleContext`
and the occurrence snapshots of `ExternalUseStyleRetention` share the host's record. Static
assertions in `ComputedStyleComponent.h` keep the stored record `const`.
Whole-component clears happen in `StyleSystem::discardComputedStylesForFullRestyle` (once per
full-restyle rebuild, and in the full pass of `computeAllStyles` for its other callers),
`MarkStylesheetChanged`, `MarkNeedsFullRebuild`, `StyleSystem::discardComputedStyles` and
`RecomputeAnimationStyles`.

**Shadow trees.** `<use>`, pattern, marker and mask instances are shadow entities.
`ShadowedElementAdapter` matches selectors against the shadow tree's structure (parent, siblings,
children) while reading `id`, `class` and attributes from the referenced light element. Each shadow
entity is cascaded like any other element.

**Traversal.** A full pass in `computeAllStyles` visits `registry.view<TreeComponent>()` in
storage order, which is not tree order; `computePropertiesInto` computes a parent on demand
before inheriting from it. The selective pass visits dirty entities in storage order. Shadow
entities are cascaded in the same passes and through `computeStylesFor`. `ExternalUseStyleContext`
cascades a referenced external document in its own registry.

**Invalidation.** `StyleSystem::computeAllStyles` has two modes. The selective pass recomputes
entities flagged `DirtyFlagsComponent::Style` whose computed style was removed. The full pass
computes every entity without a computed style, after discarding every `ComputedStyleComponent`
unless its caller already did (`StyleSystem::FullRestyle`), and runs whenever
`RenderTreeState::needsFullStyleRecompute` is set. That flag is set by `SVGElement::setClassName`,
`setId`, `setStyle` and `updateStyle`, by presentation-attribute writes other than `transform` and
`opacity` (`MarkPresentationAttributeDirty`), by stylesheet changes (`MarkStylesheetChanged`), and
by child insertion and removal (`MarkChildInserted`, `MarkChildRemoved`). `transform` and `opacity`
writes request it only when a stylesheet selects on the attribute
(`StyleSystem::anyStylesheetUsesAttributeInSelector`). A full restyle also takes the full rebuild
path in `RenderingContext::ensureComputedComponents`, which tears down and re-instantiates shadow
trees, because `tryRetainedUpdate` declines when the flag is set.

`RenderingContext::createComputedComponents` calls `StyleSystem::computeAllStyles` twice: before
and after shadow-tree instantiation. On a full-restyle frame it first discards every computed
style once, and both calls compute only entities without a computed style: the first the document
tree, the second the shadow-tree clones instantiated in between. A full-restyle frame therefore
cascades each styled entity once, the work of the edited document's first frame. Animated
documents add a second whole-document cascade: `ApplyAnimationPresentation` calls
`RecomputeAnimationStyles`, which clears and recomputes every style with animation enabled.

[0005-incremental_invalidation](0005-incremental_invalidation.md) introduced the dirty flags and
deferred a selector index for stylesheet-level changes; this design supplies that index and the
selector-dependency side of invalidation.

**Stylesheet text edits.** The editor's source pane turns a keystroke into
`SVGDocument::applySourceEdit` (`TryApplyStructuredSourceEdit` in `donner/editor/SourceSync.cc`).
For a keystroke inside a `<style>` element, `XMLDocument` takes the `ReparseScope::TextNode`
scope and emits a node-value mutation; `SVGDocument` projects it through `ProjectStyleContents`,
which concatenates the element's text, reparses the whole stylesheet with its source map, charges
the parsed-payload budget, and then calls `MarkStylesheetChanged`, which clears every computed
style and requests a full rebuild. No document reparse happens; the whole-stylesheet reparse and
whole-document restyle do. A keystroke in a `class` attribute takes `ReparseScope::AttributeValue`
and reaches the same full restyle through `SVGElement::setClassName`.

**Source-pane highlighting and completion.** `LanguageDefinition::SVG()` in
`donner/editor/TextEditorCore.cc` colors source with a per-line character tokenizer: XML
punctuation, SVG element names as keywords and known attribute names as identifiers. It has no CSS
classes; CSS inside `<style>` and `style=""` is colored like any other text.
`DetectXmlAutocompleteContext` in `donner/editor/XmlAutocomplete.cc` uses `xml::Tokenize` to find
element-name, attribute-name and CSS property-name contexts; the property-name context covers both
`style` attributes and `<style>` text and offers `PropertyRegistry::propertyNames()`.
`MakeStylePropertyContext` finds the start of the declaration by scanning for `;` and `{`
characters, which does not account for strings, comments or nested blocks, and there is no
completion for selectors, values, `var()` names or at-rules.

### Measured Baseline

Machine: a 64-bit Arm Linux server, optimized build (`-c opt`), the benchmark process
pinned to one core while unrelated work ran on other cores (load average 40 to 72 during the
load, edit and scaling runs, 12 to 15 during the opportunity and keystroke runs). Revision
`a4cd11c9` for load, edits and scaling, and `867a4136` (this design's branch on a later main) for
the opportunity and keystroke runs. Each load case ran 9 times on a freshly parsed document; each
edit ran 19 times on one loaded document, discarding the first; each keystroke ran 20 times,
discarding the first two. Tables give the median with the minimum and maximum in parentheses.
"Style" is the inclusive time of the outermost `StyleSystem::computeAllStyles` and
`computeStylesFor` calls; "match" is the time inside `applyStylesheetRules` (rule matching plus
declaration application), from a separate run with per-element timers that added under 0.5 ms to any
style time. "Null frame" is `RendererDriver::draw` with a renderer that records nothing, so it
covers style, layout, shapes, shadow trees and render-tree instantiation but no rasterization.
"TinySkia frame" is the same draw with the software renderer. Counters are exact and identical
across runs.

**Load (first frame).**

| Document                                      | Elements cascaded | Rules tested | Rules matched | Parse ms            | Null frame ms       | Style ms            | Match ms | TinySkia frame ms      |
| --------------------------------------------- | ----------------: | -----------: | ------------: | ------------------- | ------------------- | ------------------- | -------: | ---------------------- |
| `donner_splash.svg` (89 rules)                |               622 |       57,838 |         1,382 | 12.03 (11.60-18.25) | 14.18 (13.67-16.98) | 10.81 (10.43-11.48) |     7.65 | 135.38 (133.84-146.03) |
| `bootstrap-icons.svg` (no author rules)       |             5,137 |       20,544 |         9,291 | 59.47 (58.68-60.37) | 63.91 (63.25-69.06) | 31.52 (31.39-34.85) |     8.19 | 64.47 (63.56-71.69)    |
| `Ghostscript_Tiger.svg` (no author rules)     |               484 |        1,928 |           481 | 12.77 (12.25-14.34) | 6.20 (5.74-6.50)    | 3.22 (3.09-3.44)    |     0.74 | 26.89 (25.90-27.58)    |
| Synthetic, 2,000 elements, 200 rules          |             2,003 |      408,608 |         3,254 | 45.03 (44.18-45.88) | 87.27 (86.44-88.10) | 77.64 (77.13-78.14) |    67.02 | 93.71 (91.68-94.36)    |
| Synthetic, 200 `<use>` of 20 shapes, 20 rules |             4,425 |      106,196 |         8,846 | 4.29 (4.18-4.65)    | 82.55 (80.80-84.68) | 39.05 (38.35-39.50) |    21.89 | 302.90 (297.60-317.65) |

Style as a share of the work it sits in:

| Document                | Of null frame | Of parse plus null frame | Of parse plus TinySkia frame |
| ----------------------- | ------------: | -----------------------: | ---------------------------: |
| `donner_splash.svg`     |    76 percent |               41 percent |                    7 percent |
| `bootstrap-icons.svg`   |    49 percent |               26 percent |                   25 percent |
| `Ghostscript_Tiger.svg` |    52 percent |               17 percent |                    8 percent |
| Synthetic 2,000 x 200   |    89 percent |               59 percent |                   56 percent |
| Synthetic `<use>`       |    47 percent |               45 percent |                   13 percent |

When a full-document software rasterization follows, style is 7 to 8 percent of loading the two
real illustrations; it dominates loads whose rasterization is cheap or absent (the sprite file
draws nothing; GPU and compositor paths rasterize less than a full TinySkia frame) and documents
with many rules.

**Edits on a loaded document.** One element's attribute is written through
`SVGElement::setAttribute`, the call the editor's attribute writeback makes, then one frame is
prepared. The append case rewrites the `<style>` element's contents through
`SVGStyleElement::setContents` with one added rule.

| Document and edit                  | Elements cascaded | Rules tested | Null frame ms          | Style ms               | Style share | TinySkia frame ms      |
| ---------------------------------- | ----------------: | -----------: | ---------------------- | ---------------------- | ----------: | ---------------------- |
| Splash, `class`                    |             1,069 |       99,401 | 23.33 (22.97-23.77)    | 18.79 (18.55-19.18)    |  81 percent | 144.96 (143.55-147.05) |
| Splash, `fill` attribute           |             1,069 |       99,401 | 24.16 (22.92-25.09)    | 19.40 (18.60-19.88)    |  80 percent | 145.90 (144.49-155.43) |
| Splash, `style` attribute          |             1,069 |       99,401 | 23.83 (23.19-24.37)    | 19.14 (18.71-19.60)    |  80 percent | 145.59 (143.05-150.76) |
| Splash, append one rule            |             1,069 |      100,470 | 24.07 (23.56-26.48)    | 19.38 (18.88-21.03)    |  81 percent | 146.84 (144.48-154.04) |
| Synthetic 2,000 x 200, `class`     |             4,006 |      817,216 | 167.32 (163.37-170.15) | 158.22 (155.10-161.47) |  95 percent | 173.48 (170.44-178.92) |
| Synthetic 2,000 x 200, `fill`      |             4,006 |      817,216 | 166.62 (164.46-175.59) | 158.27 (156.14-166.41) |  95 percent | 170.72 (168.77-176.32) |
| Synthetic 2,000 x 200, `style`     |             4,006 |      817,216 | 168.34 (164.78-170.10) | 159.26 (156.43-160.73) |  95 percent | 173.93 (170.43-180.98) |
| Synthetic 2,000 x 200, append rule |             4,006 |      821,222 | 165.24 (163.46-169.22) | 156.81 (155.13-160.67) |  95 percent | 171.74 (169.67-175.51) |
| Bootstrap icons, `class`           |            10,274 |       41,088 | 70.07 (68.58-72.81)    | 63.14 (62.05-65.25)    |  90 percent | 70.43 (69.00-73.58)    |
| Bootstrap icons, `fill`            |            10,274 |       41,088 | 70.97 (69.42-72.98)    | 63.98 (62.29-65.41)    |  90 percent | 70.83 (68.95-74.23)    |
| Bootstrap icons, `style`           |            10,274 |       41,088 | 70.50 (69.00-72.98)    | 63.41 (62.08-65.66)    |  90 percent | 69.87 (67.83-72.19)    |

In these measurements every edit, including a `fill` write on one shape, cascades the whole document
twice (2 full clears per frame in every row). The append rows give the work of the frame after the
rule is appended; the frame that removes it again tests the original 99,401 and 817,216 rules. Their
times are medians over both frames of the alternation. The editor presents through its compositor
and Geode rather than a full TinySkia frame, so its edit-to-frame cost lies between the two frame
columns; style is 80 to 95 percent of the frame preparation that precedes rasterization in every
case measured.

An edit frame cascades every styled entity once with one full clear, the work of the edited
document's first frame, and each element tests only the rule index's candidates.
`css_cascade_perf_tests_correctness` asserts these counts:

| Document and edit                                     | Elements cascaded | Rules tested | Rules matched | Declarations applied | Full clears |
| ----------------------------------------------------- | ----------------: | -----------: | ------------: | -------------------: | ----------: |
| Splash, first frame                                   |               622 |        1,384 |         1,382 |                1,386 |           0 |
| Splash, `class`, `fill` or `style`; declaration value |               622 |        1,384 |         1,382 |                1,386 |           1 |
| Splash, append one rule                               |               622 |        1,384 |         1,382 |                1,386 |           1 |
| Splash, selector or `class` letter inserted           |               622 |        1,383 |         1,381 |                1,385 |           1 |
| Splash, closing brace deleted                         |               622 |        1,246 |         1,244 |                1,245 |           1 |
| Synthetic 2,000 x 200, first frame                    |             2,003 |        3,543 |         3,254 |                4,506 |           0 |
| Synthetic 2,000 x 200, `class`                        |             2,003 |        3,544 |         3,255 |                4,508 |           1 |
| Synthetic 2,000 x 200, `fill`, `style` or append rule |             2,003 |        3,543 |         3,254 |                4,506 |           1 |
| Bootstrap icons, first frame                          |             5,137 |        9,293 |         9,291 |                9,291 |           0 |
| Bootstrap icons, `class`, `fill` or `style`           |             5,137 |        9,293 |         9,291 |                9,291 |           1 |
| `Ghostscript_Tiger.svg`, first frame                  |               484 |          483 |           481 |                  481 |           0 |
| Synthetic `<use>`, first frame                        |             4,425 |        8,848 |         8,846 |                8,846 |           0 |
| Synthetic 4,000 x 1,000 mixed, first frame            |             4,003 |        7,102 |         6,555 |                9,108 |           0 |
| Repeated class tokens, first frame                    |                 4 |        2,005 |         2,003 |                2,003 |           0 |

The splash styles 447 document elements and 175 shadow-tree clones; the synthetic document and the
sprite have no clones. Rules tested equal the candidates of
[Ancestor Filter Opportunity](#ancestor-filter-opportunity). On the splash, sprite and tiger
documents all but two candidate tests per document match; on the synthetic document 289 do not. A
rule appended for a class no element carries is no element's candidate. The 4,000 x 1,000 document
styles completely; testing every rule would exhaust the 1,048,576-test budget after 1,702 matches.
The repeated-token document has 2,000 `.a` rules and one rect whose `class` names `a` 20,000 times;
it tests each rule once.

**Scaling.** Synthetic documents with groups of ten `<rect>` elements, each with one class, and
`R` rules of one kind (first-frame style time, median of 9):

| Elements x rules | Rule kind                | Rules tested | Style ms | Match ms |
| ---------------- | ------------------------ | -----------: | -------: | -------: |
| 1,000 x 0        | (user agent only)        |        4,004 |     6.08 |     1.47 |
| 1,000 x 10       | mixed                    |       14,038 |     8.25 |     3.46 |
| 1,000 x 100      | mixed                    |      104,308 |    22.83 |    17.84 |
| 1,000 x 1,000    | mixed                    |    1,007,008 |   163.09 |   156.95 |
| 4,000 x 0        | (user agent only)        |       16,004 |    24.75 |     6.00 |
| 4,000 x 10       | mixed                    |       56,038 |    33.72 |    13.96 |
| 4,000 x 100      | mixed                    |      416,308 |    92.76 |    71.86 |
| 4,000 x 1,000    | mixed, budget exhausted  |    1,048,576 |   185.06 |   164.62 |
| 1,000 x 1,000    | `.cN` only               |    1,007,008 |   161.49 |   155.70 |
| 1,000 x 1,000    | `.gK .cN` only           |    1,007,008 |   161.30 |   155.78 |
| 1,000 x 1,000    | type and `:nth-child(N)` |    1,007,008 |   169.85 |   163.51 |
| 1,000 x 1,000    | `[data-kN]` only         |    1,007,008 |   163.54 |   157.72 |

"Mixed" is 70 percent class, 10 percent type, 15 percent descendant and 5 percent attribute
selectors. Findings:

- Matching cost is linear in elements times rules and nearly independent of selector kind: 155 to
  173 ns per rule tested, almost all of them rejections (0.8 percent of tests match on the mixed
  document, 2.4 percent on the splash). A rejection on the class of a one-class element costs as
  much as a descendant selector, so the per-test overhead, not the combinator walk, dominates.
- Outside matching, each element costs about 4.6 microseconds (style minus match, divided by
  elements, on the rule-free documents and the sprite); see
  [Per-Element Cost](#per-element-cost) for where it goes.
- At 4,000 elements and 1,000 rules, testing every rule exhausts the 1,048,576 rule-element budget
  and leaves later elements with partial styles, whatever they actually match; with the rule index
  that document tests 7,102 candidates and styles completely.

#### Per-Element Cost

Where the style time of a rule-free document goes, per cascaded element, on the 1,000-element
synthetic document with no author rules (1,002 entities cascaded), from the instrumented build
described in [Reproducing the Baseline](#reproducing-the-baseline): the benchmark pinned to one core
of the baseline machine class (load average 45 to 52), with the rule index, median of three
alternating runs of each build, each run the median of nine first frames. Each row includes about
35 ns per clock reading, so the instrumented totals run about 0.3 ms above the uninstrumented style
time. The left column stores the computed `PropertyRegistry` inline in its component; the right
one is the shared record.

| Phase, nanoseconds per element                               | Inline `PropertyRegistry` | Shared record |
| ------------------------------------------------------------ | ------------------------: | ------------: |
| Rule matching and declaration application                    |                       861 |           756 |
| Copy of the local declarations, with the record's allocation |                       983 |         1,022 |
| Inheritance from the parent                                  |                     1,474 |           269 |
| Storing the result in the component                          |                       682 |            40 |
| Placeholder components created before the pass               |                       656 |            50 |
| Component lookups and memoization checks                     |                       424 |           244 |
| Animation hooks (the document has no animations)             |                       367 |           347 |
| Symbol display and element colors                            |                       120 |           116 |
| Parent style lookup                                          |                        89 |            88 |
| Retained-byte charge                                         |                       155 |           203 |
| Font resolution                                              |                       183 |           141 |
| Setup and pass overhead                                      |                       150 |           144 |
| Whole style pass (instrumented median)                       |                     6,148 |         3,493 |

In the left column, inheritance builds a new registry from the element's and moves it into place
twice, storing moves it into the component, and every placeholder component writes a 4,912-byte
slot; the shared record removes those copies. Outside matching an element now costs about 2.7
microseconds instrumented (about 2.1 uninstrumented), down from 5.3 (4.5). What remains is inherent
to computing each element's own style: one copy of its local declarations into the new record (about
1 microsecond, which the matched properties cache of Milestone A9 skips on a hit), the animation
hooks, which look up three animation contexts even on documents without animations, and font
resolution. The other measured documents agree: per element, inheritance falls from 1.48 to 0.26
microseconds on bootstrap-icons and from 1.64 to 0.35 on the tiger, and the placeholder step from
about 0.65 to 0.05 on both.

Memory per element is unchanged: the component shrinks from 4,912 to 24 bytes and the record takes
about 4.9 KB on the heap (`sizeof(PropertyRegistry)`, the resource opacities and the shared
pointer's control block). Heap in use after a first frame falls by 67 KB (0.9 percent) on the
1,000-element document, 24 KB (0.07 percent) on bootstrap-icons, 117 KB (2.7 percent) on the tiger
and 63 KB (0.9 percent) on the splash, and grows by 13 KB (0.05 percent) on the 4,000-element
document.

#### Ancestor Filter Opportunity

The harness simulated the rule index of [Rule Index](#rule-index) (one key per selector-list entry
from the subject compound, preferring id, class, attribute, type) and recorded, for each simulated
candidate, whether matching took a combinator step and whether it then failed. A candidate whose
selectors have a descendant or child combinator and that fails after walking is one an ancestor
filter could reject. Counters, first frame:

| Document                         | Candidates after index | With descendant or child | Failed after an ancestor walk | Steps in those walks / all candidate steps |
| -------------------------------- | ---------------------: | -----------------------: | ----------------------------: | -----------------------------------------: |
| `donner_splash.svg`              |                  1,384 |                      620 |                             1 |                                    3/2,011 |
| `bootstrap-icons.svg`            |                  9,293 |                    5,136 |                             1 |                                   3/22,747 |
| `Ghostscript_Tiger.svg`          |                    483 |                      482 |                             1 |                                      3/971 |
| Synthetic 2,000 x 200, mixed     |                  3,543 |                    2,293 |                           288 |                                  864/6,129 |
| Synthetic `<use>`                |                  8,848 |                    4,424 |                             1 |                                   3/14,082 |
| Synthetic 1,000 x 100, `.gK .cN` |                  1,913 |                    1,911 |                           902 |                                2,706/4,731 |

On the real documents the candidates with a combinator are almost all the user agent rule
`*:not(svg), *:not(foreignObject) > svg`, whose second entry has a child combinator; one candidate
per document fails after a walk. The filter has nothing to remove there. On descendant-heavy
stylesheets it removes most failing walks: 47 percent of candidates and 57 percent of candidate
traversal steps on the descendant-only document. At the measured 155 ns per rule test, all 1,913
candidates of that document cost about 0.3 ms, so the filter's time effect is bounded by a few
tenths of a millisecond even there. The simulated candidate counts also confirm the index goal:
1,384 candidates on the splash document against 57,838 rules in the every-rule cascade, the count
the rule index produces.

#### Shared Computed Style Opportunity

The harness grouped elements by the inputs that determine a computed style: the parent's group,
the element type, whether the parent carries `DoNotInheritFillOrStrokeTag`, whether the element is
a direct `<use>` instance root, the ordered matched rules with their specificities, and the
element's local `StyleComponent` properties (compared with `PropertyRegistry::operator==`).
Elements with animation values or an external-use host were counted as uncacheable (none in these
documents). For every element whose key matched an earlier group, the harness also compared the
final computed `PropertyRegistry` with the group's; there were no mismatches, so on these
documents the key is complete.

| Document                         | Elements | Reuse an earlier group | Reuse ignoring local style | Distinct records |
| -------------------------------- | -------: | ---------------------: | -------------------------: | ---------------: |
| `donner_splash.svg`              |      622 |       413 (66 percent) |                        507 |              209 |
| `bootstrap-icons.svg`            |    5,137 |   5,129 (99.8 percent) |                      5,131 |                8 |
| `Ghostscript_Tiger.svg`          |      484 |       389 (80 percent) |                        434 |               95 |
| Synthetic 2,000 x 200, mixed     |    2,003 |     1,854 (93 percent) |                      1,854 |              149 |
| Synthetic `<use>`                |    4,425 |     4,378 (99 percent) |                      4,378 |               47 |
| Synthetic 1,000 x 100, `.gK .cN` |    1,003 |       991 (99 percent) |                        991 |               12 |

The column that ignores local style shows how much reuse presentation attributes and `style`
attributes cost: on the splash and the tiger, elements with the same rules and parent often
differ only in a `fill` attribute. At 4,840 bytes per computed `PropertyRegistry`, one record per
element, bootstrap-icons retains 24.9 MB of computed style for 8 distinct values.

#### Stylesheet Keystrokes

One-character source edits on the loaded splash document through `SVGDocument::applySourceEdit`,
the call the editor's source pane makes, then one frame (load average 12 to 15):

| Keystroke                                          | Scope selected           | Source edit ms   | Null frame ms       | Style ms            | TinySkia frame ms      |
| -------------------------------------------------- | ------------------------ | ---------------- | ------------------- | ------------------- | ---------------------- |
| `<style>` declaration value, one hex digit         | `TextNode` (18/18)       | 1.26 (1.20-1.32) | 23.22 (23.01-23.65) | 19.20 (19.06-19.62) | 138.96 (138.30-139.92) |
| `<style>` selector, insert or delete a letter      | `TextNode` (18/18)       | 1.20 (1.17-1.26) | 22.32 (22.01-22.57) | 18.49 (18.24-18.71) | 139.04 (138.48-139.54) |
| `<style>` rule's closing brace deleted, restored   | `TextNode` (18/18)       | 1.16 (1.10-1.26) | 22.32 (9.91-22.67)  | 18.52 (6.27-18.80)  | 138.29 (105.52-139.31) |
| `class` attribute value, insert or delete a letter | `AttributeValue` (18/18) | 0.55 (0.52-0.61) | 22.69 (22.36-23.14) | 18.67 (18.38-19.02) | 139.36 (138.03-141.82) |

Every keystroke stayed incremental at the XML level and none reparsed the document, but each
reparsed the whole stylesheet (inside the source edit time) and restyled the whole document (1,069
element cascades and 2 full clears in these measurements, as for the attribute edits above).
Deleting a rule's closing brace changes what the stylesheet parse keeps: the two alternating states
differ by a factor of three in style time (6.3 to 18.8 ms), which is what a reader typing through an
unbalanced brace sees today.

### Reproducing the Baseline

`StyleSystem` keeps the counters and the style timer in the document registry's context, and
`//donner/benchmarks:css_cascade_perf_bench` runs the load, edit, keystroke and scaling cases with
the method above, one `RESULT` line per case:

```sh
bazel build -c opt //donner/benchmarks:css_cascade_perf_bench
taskset -c 3 bazel-bin/donner/benchmarks/css_cascade_perf_bench \
    --mode=all   # or load, edits, keystrokes, scaling; --no-renderer skips TinySkia frames
```

Run it from the repository root, or with `bazel run`, so it finds the three document files. An
edit whose two directions do different work (appending and removing a rule, inserting and
deleting a character, deleting and restoring a brace) prints one line per direction, from its
alternate repetitions, and the run fails if the repetitions of any line differ in work.
`//donner/benchmarks:css_cascade_perf_tests_correctness` asserts the current counts above (the
load and scaling counter columns are the every-rule cascade they replace), and
`//donner/benchmarks:css_cascade_perf_tests_wallclock` (manual, `perf`) records the splash
first-frame, splash class-edit, bootstrap-icons and synthetic 2,000 x 200 style times.

1. Documents: `donner_splash.svg`, `third_party/bootstrap-icons/bootstrap-icons.svg`,
   `donner/svg/renderer/testdata/Ghostscript_Tiger.svg`, and generated documents: groups
   `<g class="gK">` (`K` is the group index mod 97) of up to ten `<rect class="cN">` with no
   whitespace text nodes between elements (the default 8,192-node parser cap applies), each rect's
   class drawn from a fixed seed; rules of one kind, `.cN`, `.gK .cN` with `K = N mod 97`, a type
   selector cycling through eight element names with `:nth-child(N)` from the ninth rule on, or
   `[data-kN]`; and a `<symbol>` of 20 `<rect class="cN">` used by 200 `<use>` elements.
2. Edit targets: the first `.cls-71` path in the splash (class toggled to `cls-88`), element
   `#e1000` of the synthetic document (class `c3` and `c7`), the first `symbol > path` of the
   sprite (classes `a` and `b`). Keystroke targets in the splash: the last hex digit of `#fffffc`
   in the `.cls-2` rule, a letter after the first `.cls-2` selector, that rule's closing brace,
   and a letter after the first `class="cls-71`.
3. "Match" times and the opportunity tables need instrumentation the benchmark does not carry:
   time `applyStylesheetRules` per element; count combinator steps in
   `ComplexSelector::matchChain` (one increment where it pushes a frame); in
   `applyStylesheetRules`, decide per rule whether it would be an index candidate (the key choice
   of [Rule Index](#rule-index)) and record whether the match took a combinator step, failed, and
   how many `SelectorTraversalBudget` steps it used; capture the matched rules right after
   `applyStylesheetRules` and, at the end of `computePropertiesInto`, group the element by the key
   in [Shared Computed Style Opportunity](#shared-computed-style-opportunity) and compare its final
   properties with the group's. These counters copy registries and must be off for timing runs.
   The per-phase times of [Per-Element Cost](#per-element-cost) come from a separate build that
   reads `std::chrono::steady_clock` at each phase boundary of `computeStyle`,
   `computePropertiesInto`, `computeAllStyles` and `computeMissingStyles` and adds the interval to
   the phase being left, so a parent cascaded on demand is charged to its own phases; each reading
   costs about 35 ns on the baseline machine and is included in the phase it ends.

### Browser Techniques

**Rule bucketing by the rightmost compound.** Engines index each rule under one key of its
subject (rightmost) compound selector: id, class, attribute name, type name, or a universal
bucket scanned for every element. Blink's `RuleSet` keeps maps for id, class, attribute and tag
plus vectors for a few pseudo-classes and a universal fallback, picks the bucket by expected
rarity, packs each `RuleData` into a few words, and compacts each bucket into a contiguous span
([rule_set.h](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/core/css/rule_set.h)).
Stylo's `SelectorMap` has root, id, class, attribute, local-name, namespace, rare-pseudo-class and
"other" buckets, considers only the rightmost compound, and pre-hashes keys
([selector_map.rs](https://github.com/servo/stylo/blob/main/style/selector_map.rs)). It pays
whenever rules outnumber the keys an element carries; the cost is one entry per selector, and
the pitfall is a large universal bucket, which every element still scans.

**Ancestor Bloom filter.** While walking the tree, the engine keeps a counting Bloom filter of
the ancestors' tag, id, class and attribute-name hashes; each selector stores a few hashes from
the compounds to the left of descendant and child combinators, and is rejected without walking
ancestors when any hash is absent. WebKit's `SelectorFilter` uses a 4,096-slot counting filter
(`bloomFilterKeyBits = 12`), collects a selector's hashes in separate lists for ids, classes, tags
and attributes, keeps up to 4 per selector, and quotes about 0.2 percent false positives at 100
strings
([SelectorFilter.h](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/css/SelectorFilter.h));
the change that introduced it estimated about 30 percent faster matching on sites with many
descendant selectors ([WebKit bug 53880](https://bugs.webkit.org/show_bug.cgi?id=53880)). Stylo
uses 4,096 8-bit saturating counters with two hash functions and documents that the filter never
reports a present item as absent
([selectors/bloom.rs](https://github.com/servo/stylo/blob/main/selectors/bloom.rs),
[style/bloom.rs](https://github.com/servo/stylo/blob/main/style/bloom.rs)). False positives only
cost a full match; a false negative is a wrong style, so removals must mirror insertions exactly.
It pays when many candidate rules have ancestor conditions that fail.

**Right-to-left matching and fast paths.** All engines match from the subject compound leftward.
WebKit reported a 25 percent overall gain, and 2x for descendant and child selectors, from a
specialized non-recursive loop for common selector shapes
([Sullivan, 2011](https://calendar.perfplanet.com/2011/css-selector-performance-has-changed-for-the-better/)).
WebKit's CSS JIT compiles selectors to machine code for about 2x on common selectors, with
interpreter fallback for unsupported pseudo-classes
([WebKit blog, 2014](https://webkit.org/blog/3271/webkit-css-selector-jit-compiler/)).

**Style sharing.** Stylo keeps a per-depth 32-entry cache of recently styled siblings and cousins
and reuses a style when local name, state, classes, `style` attribute, presentational hints and
the results of a set of "revalidation" selectors (structural and attribute selectors) are equal;
its source notes that 32 entries resolved 3x fewer styles than 8
([sharing/mod.rs](https://github.com/servo/stylo/blob/main/style/sharing/mod.rs),
[Lin Clark, 2017](https://hacks.mozilla.org/2017/08/inside-a-super-fast-css-engine-quantum-css-aka-stylo/)).
Blink disabled its style sharing outside tests in 2017
([CL 2858423002](https://codereview.chromium.org/2858423002)). The pitfall is completeness: any
selector feature that can tell two candidates apart must be revalidated.

**Matched properties cache.** Blink's `MatchedPropertiesCache` and WebKit's
`MatchedDeclarationsCache` key computed style on a hash of the matched declaration blocks (plus
the parent style), so elements that match the same blocks reuse the cascade result
([matched_properties_cache.h](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/core/css/resolver/matched_properties_cache.h),
[MatchedDeclarationsCache.h](https://github.com/WebKit/WebKit/blob/main/Source/WebCore/style/MatchedDeclarationsCache.h)).
Blink keeps one entry per hash and notes the cache is vulnerable to hash collisions; WebKit keeps
a vector of entries per hash. It needs shareable, immutable computed-style storage.

**Rule tree.** Gecko and Stylo store each element's ordered matched declarations as a path in a
shared tree, so restyles that only swap the `style` attribute or animation level replace one node
without re-matching
([rule_tree/mod.rs](https://github.com/servo/stylo/blob/main/style/rule_tree/mod.rs)).

**Parallel traversal.** Stylo styles subtrees on multiple threads with work stealing and
per-thread Bloom filters (Lin Clark, above). Context only.

**Invalidation.** Blink extracts the ids, classes, attribute names and tags each selector depends
on into invalidation sets; a change to class `c1` under `.c1 div.c2` schedules a descendant walk
that restyles only `c2` elements, and sets are conservative by design
([style-invalidation.md](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/core/css/style-invalidation.md)).
Stylo's invalidation map records, per id, class, attribute and state, the selector and the
combinator position the dependency sits at, and derives whether the element, its descendants or
its siblings are affected; `:has()` uses a separate upward map
([invalidation_map.rs](https://github.com/servo/stylo/blob/main/style/invalidation/element/invalidation_map.rs)).
Blink's `:has()` invalidation filters mutations by the argument's features and marks the elements
it walked ([Igalia, 2023](https://blogs.igalia.com/blee/posts/2023/05/31/how-blink-invalidates-styles-when-has-in-use.html)).

**Inheritance and custom properties.** Blink copies "independent" inherited properties (first
`visibility` and `pointer-events`) from parent to child without a full recalculation, reporting up
to 2x on root-level changes ([CL 2117143003](https://codereview.chromium.org/2117143003)). Stylo
keeps custom properties in shared inherited and non-inherited maps
([custom_properties.rs](https://github.com/servo/stylo/blob/main/style/custom_properties.rs)).
Stylo removes dependency cycles during substitution, invalidating every member of a cycle, and
caps a value at 2 MiB (`MAX_VALUE_LENGTH_IN_BYTES`, with a comment that values can otherwise grow
exponentially). Blink detects cycles with a depth-first walk that locks the properties it is
applying (`CascadeResolver` in
[cascade_resolver.h](https://chromium.googlesource.com/chromium/src/+/main/third_party/blink/renderer/core/css/resolver/cascade_resolver.h))
and caps a variable at 2 MiB (`kMaxVariableBytes` in
[css_variable_data.h](https://chromium.googlesource.com/chromium/src/+/main/third_party/blink/renderer/core/css/css_variable_data.h),
"This matches Firefox"). Ladybird shares custom properties between parent and child through a
parent-linked chain
([PR #7928](https://github.com/LadybirdBrowser/ladybird/pull/7928),
[February 2026](https://ladybird.org/newsletter/2026-02-28)).

**Cascade ordering.** Stylo sorts matched declarations by layer, specificity, scope proximity and
source order, packed into a 24-byte record because its size drives sort cost
([applicable_declarations.rs](https://github.com/servo/stylo/blob/main/style/applicable_declarations.rs)).
Its `CascadePriority` packs cascade level and layer order into 32 bits and reverses the layer
comparison for important declarations. Blink packs origin, importance, tree order, layer order,
inline-style bit and rule position into one comparable integer and flips the layer bits for
important declarations: "Layer order bits are flipped for important declarations to reverse the
priority"
([cascade_priority.h](https://chromium.googlesource.com/chromium/src/+/main/third_party/blink/renderer/core/css/resolver/cascade_priority.h)).

**Incremental stylesheet editing and editor data.** Chrome DevTools edits stylesheets by range,
not by full reparse: `CSS.setStyleTexts` applies a list of `StyleDeclarationEdit`s, each a range
in the enclosing stylesheet plus replacement text, and `setRuleSelector` edits one selector by
range ([CSS.pdl](https://raw.githubusercontent.com/ChromeDevTools/devtools-protocol/master/pdl/domains/CSS.pdl)).
Editor parsers reuse unchanged subtrees: Lezer reuses cached tree fragments outside the edited
ranges and warns that an edit such as opening a block comment can force a large reparse
([Lezer guide](https://lezer.codemirror.net/docs/guide/)); tree-sitter reparses with the old tree
after it is told about the edit and shares structure with it
([tree-sitter advanced parsing](https://tree-sitter.github.io/tree-sitter/using-parsers/3-advanced-parsing.html)).
DevTools generates its property and keyword completion lists from the engine's property table
(`generate_supported_css.py` reads Blink's `css_properties.json5`,
[source](https://chromium.googlesource.com/devtools/devtools-frontend/+/main/scripts/build/generate_supported_css.py));
Donner's equivalent is its own `PropertyRegistry`.

**Ladybird.** The recent Ladybird work is "Differential style resolution"
([PR #11088](https://github.com/LadybirdBrowser/ladybird/pull/11088), merged in August 2026). It
replaced selector matching, style invalidation, the cascade and computed-style retention with a
new engine: DOM and stylesheet changes enter as typed deltas, are routed to the selectors that
depend on them, and propagation stops at each stage once an output is unchanged ("selectors are
standing queries, and computed styles are materialized views"). Retained match results and
computed styles report their bytes to a budget, and the engine is checked against a cache-free
reference evaluator. The August 2026 update states the goal as "style work to be proportional to
what changed, not to what might have changed" and reports its StyleBench score on Linux rising
from about 3.5 to about 83 over that month, crediting most of the movement to the new style engine
together with layout caching ([This Month in Ladybird, August 2026](https://ladybird.org/newsletter/2026-08-31)).
Earlier 2026 updates landed narrower pieces of the same direction: scoped rule-cache rebuilds
(reddit.com on the author's machine: 4,473 to 979 rebuilds and 13,174 ms to 3,223 ms,
[PR #9138](https://github.com/LadybirdBrowser/ladybird/pull/9138),
[April 2026](https://ladybird.org/newsletter/2026-04-30)), sibling invalidation sets
([March 2026](https://ladybird.org/newsletter/2026-03-31)) and a per-element `:has()` result cache
([January 2026](https://ladybird.org/newsletter/2026-01-31)).

### Specifications

**Custom properties.** [CSS Custom Properties Level 1](https://drafts.csswg.org/css-variables-1/)
defines `--*` properties: names are case-sensitive, the value is an unresolved token sequence,
custom properties inherit, and the initial value is the guaranteed-invalid value; CSS-wide
keywords apply, and `all` does not reset custom properties. The substitution mechanics are in
[CSS Values Level 5, arbitrary substitution](https://drafts.csswg.org/css-values-5/#arbitrary-substitution):
a declaration containing `var()` is assumed valid at parse time and checked against its grammar
after substitution at computed-value time; a failure makes it invalid at computed-value time, so
the property takes its inherited value if it inherits and its initial value otherwise. The
fallback is everything after the first comma and is used when the referenced property is the
guaranteed-invalid value. A cycle makes every participant guaranteed-invalid. A shorthand with
`var()` fills its longhands with a pending-substitution value and is parsed after substitution.
User agents must defend against exponential expansion: "UAs must impose a UA-defined limit on the
allowed length of the token stream", past which the result is guaranteed-invalid.

**Custom properties in presentation attributes.**
[SVG 2](https://www.w3.org/TR/SVG2/styling.html) says presentation attributes are parsed as values
of their property, contribute to the author level with specificity 0, and is silent on `var()`.
The SVG Working Group resolved on 2025-11-20 to allow substitution functions such as `var()` in
presentation attributes, leaving the parsing algorithm to later discussion, and recorded that
Firefox and Safari already do this and Chromium does it partially
([minutes](https://www.w3.org/2025/11/20-svg-minutes.html)). The web-platform-tests file
`css/css-variables/variable-presentation-attribute.html` expects `stroke-width="var(--x)"` to
apply and inherit.

**Cascade layers.** [CSS Cascade Level 5](https://www.w3.org/TR/css-cascade-5/) sorts by origin
and importance, context, element-attached styles (the `style` attribute), layers, specificity and
order of appearance. `@layer name { ... }` and `@layer a, b;` declare layers; a block without a
name is an anonymous layer that cannot be referenced again; dotted names are nested layers; layers
are ordered by first declaration; unlayered styles form an implicit final layer. For normal
declarations the last layer wins; for important ones the first layer wins, so unlayered important
styles lose to every layer's important styles. `@import url() layer(name)` and `layer` put an
imported sheet into a named or anonymous layer. Presentational hints, which is where SVG
presentation attributes fall, are either user-agent rules or a presentational-hint origin between
user and author, so every author layer overrides them.

### Which Techniques Apply

Ranked by expected effect on the measured baseline per unit of complexity.

| Rank | Technique                                      | Decision        | Reason                                                                                                                                                                                                                                  |
| ---: | ---------------------------------------------- | --------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
|    1 | Cascade once per full restyle                  | Adopt           | Halves the style time of every measured edit; a few lines in one function.                                                                                                                                                              |
|    2 | Rule index on the rightmost compound           | Adopt           | On the stylesheet-heavy documents 97.6 to 99.2 percent of rule tests reject; the simulated index leaves 1,384 of 57,838 tests on the splash document.                                                                                   |
|    3 | Narrowed invalidation for single-element edits | Adopt           | Turns an edit from two whole-document cascades into a subtree cascade. Reuses the existing selective pass and dirty flags.                                                                                                              |
|    4 | Matched properties cache and shared records    | Adopt           | 66 to 99.8 percent of elements reuse an earlier element's computed style on the measured documents, and shared records remove up to 24.9 MB of duplicated computed style. Needs the storage change of Milestone A8.                     |
|    5 | Matcher constant factors                       | Adopt, measured | 155 ns per rejected test points at per-attempt overhead; each fix is local and independently testable.                                                                                                                                  |
|    6 | Per-element base cost (copies)                 | Adopt, measured | 4.6 microseconds per element dominated rule-light documents; [Per-Element Cost](#per-element-cost) shows where it went, and the copy removal and shared storage cut it to about 2.1.                                                    |
|    7 | Ancestor Bloom filter                          | Adopt           | Requested regardless of payoff. After indexing it removes one failing walk per real document and 47 percent of candidates on descendant-heavy stylesheets; its effect is measured by a rejection counter, not by time.                  |
|    8 | Sibling and cousin style sharing               | Reject          | Correctness depends on a complete revalidation set over Donner's full selector support, including shadow-tree adapters; the matched properties cache captures the same reuse with a simpler key.                                        |
|    9 | Rule tree                                      | Reject          | Its benefit, restyle without re-matching, is delivered by narrowed invalidation and the matched properties cache with far less machinery.                                                                                               |
|   10 | Selector JIT                                   | Reject          | Large complexity, no executable memory on WebAssembly, and a 2x ceiling on a cost the index already removes.                                                                                                                            |
|   11 | Parallel traversal                             | Adopt           | Part D, on the 0073 scheduler. Pays on the measured documents above about 1,000 elements (the sprite and the `<use>` document); the splash and tiger stay serial below the threshold. Costs a decomposition and a ThreadSanitizer lane. |
|   12 | Custom properties, with shared maps            | Adopt           | Part B, by operator decision: `var()` is a missing feature, not a speedup. An element that declares no custom property shares its parent's map, as Ladybird and Stylo do, so documents without custom properties pay nothing.           |
|   13 | Cascade layers, ordered inside the priority    | Adopt           | Part B, by operator decision. The layer rank joins `css::Specificity`, as Blink and Stylo pack it into their priority, so the source-order cascade needs no sort.                                                                       |
|   14 | Range-based incremental stylesheet edits       | Adopt           | Part C: a keystroke costs a 19.2 ms whole-document restyle today; a rule-window reparse plus a restyle of the matched elements removes it.                                                                                              |

## Proposed Architecture, Part A: Performance

### Single Cascade per Full Restyle

`RenderingContext::createComputedComponents` performs the full-restyle clear itself, once, before
the first `computeAllStyles` call (`StyleSystem::discardComputedStylesForFullRestyle`, counted in
`fullClears` and in the style time), and both calls run with
`StyleSystem::FullRestyle::ComputeMissing`, which computes only entities without a computed style.
Other callers of `computeAllStyles` keep `FullRestyle::DiscardAndRecompute`. The second call
computes only the clones that `PaintSystem::createShadowTrees` and `InstantiateMainShadowTrees`
create after the first call; clones created later (`InstantiateStyledPaintTrees`,
`InstantiateNestedMarkerTrees`, `createFeImageShadowTree`) are styled through `computeStylesFor`.
The style budget is reset at the start of each `computeAllStyles` call, so the second call charges
only the clones, as on a first frame, and a call that ends refused counts once in
`budgetRejections`. The dirty-entity pass, the retained update and the first frame do not discard: a
full rebuild without the full-restyle flag (for example an `opacity` edit with retained updates off)
restyles only the dirty entities, which
`RenderingContextTest.PartialRebuildRestylesOnlyDirtyEntities` pins with exact counters.

**Animation.** `ApplyAnimationPresentation` runs between the two calls and, for animated documents
(which `ensureComputedComponents` always sends through the full restyle), recomputes every style
through `RecomputeAnimationStyles` with animation enabled. Its other overrides go to geometry,
transform, reference and element-attribute components, not to computed styles, so nothing depends on
the second call discarding styles.
`RenderingContextTest.AnimatedDocumentFullRestyleMatchesFirstFrame` pins this: after a class edit,
every computed style of an animated document (an animated fill and geometry drawn through `<use>`,
an animated `<use>` target and an animated inherited color) equals the first frame of the document
parsed with the edit applied. Animated documents take the same path; their animation recompute stays
a second whole-document cascade and a second full clear on every rebuild.

**Shadow trees and selectors.** The main shadow root of a `<use>` is attached under its host while
the frame is built. A full rebuild styles the document tree before that, so on first frames and
full-restyle frames selector matching for document elements does not see `<use>` shadow content:
a `:has()` on an ancestor of a `<use>` does not match the instanced content.
`RenderingContextTest.FullRestyleMatchesUseHostWithoutItsShadowTree` pins this. A retained update
restyles dirty entities with the shadow trees still attached, so a `:has()` on a restyled ancestor
can match the instanced content there
([#2715](https://github.com/jwmcglynn/donner/issues/2715)).

`//donner/svg/renderer/tests:full_restyle_corpus_tests` checks that an unedited full restyle leaves
every computed style of every corpus document (the resvg suite, the renderer test data, the splash
and the icon sprite) as its first frame computed it. Nothing is edited, so it cannot detect stale
styles, a skipped discard or extra work; the counter test and the `RenderingContextTest` cases
cover those.

### Rule Index

**Data structure.** `RuleIndex` lives in `donner/svg/components/style/RuleIndex.h`, one per
`css::Stylesheet`: `StylesheetComponent::ruleIndex` for the sheet's own rules and
`importedRuleIndexes` for each imported sheet. `rebuildSelectorCaches` builds them, so every parse
and every import replacement rebuilds them; the cascade asserts that an index was built from as
many rules as its sheet has, so a sheet changed without a rebuild fails loudly. It holds:

- Four sorted flat vectors of `(key, firstEntry, count)` for id, class, lowercased type name and
  lowercased attribute local name, pointing into one `entries` vector.
- One `universal` span of entries.
- Each entry is `{uint32_t ruleIndex}` (4 bytes); the per-key arrays add 8 bytes per distinct key
  besides the key itself, an `RcString` that shares the selector's storage, or a lowercased copy
  for a type or attribute name written with capitals.

Lookups are binary searches with exact (id, class) or ASCII-lowercased (type, attribute)
comparison. Sorted vectors avoid hashing attacker-chosen strings, keep the structure contiguous,
and are built once per parse.

**Key choice.** For each `ComplexSelector` in a rule's selector list, take the last compound
selector and choose one key, preferring id, then class, then attribute name, then type name.
Every key is a necessary condition for the compound to match, so filtering by it never drops a
rule that could match:

- id and class: case-sensitive, matching `IdSelector` and `ClassSelector`.
- type: `TypeSelector::matches` compares names with ASCII case-insensitivity, so keys and lookups
  are lowercased. A universal type (`*`, `ns|*`) gives no key; the namespace part is left to the
  full match.
- attribute: lowercased local name, the same over-approximation `usesAttributeInSelector` makes;
  a namespaced or wildcard attribute selector gives no key.
- Pseudo-classes in the last compound, including `:is()`, `:where()`, `:not()`, `:has()` and the
  structural pseudo-classes, give no key by themselves; if the compound has no other key, the
  entry goes to `universal`. An `:is()` or `:where()` argument is not mined for keys in the first
  version (Stylo inserts such lists into several buckets; that is a later refinement).

A rule is listed once per selector-list entry, possibly under several keys; duplicates of the same
rule are removed during candidate collection.

**Candidate collection.** `applyStylesheetRules` builds, per element and per sheet, the sorted,
de-duplicated list of candidate `ruleIndex` values from: the entry for the element's id, one per
class (split with `StringUtils::SplitAsciiWhitespace`, the same split `ClassSelector` uses), the
tag name, one per attribute local name, and `universal`. The keys are read through
`RuleIndexElementView`, which follows the rule of `ShadowedElementAdapter` (a shadow entity's tag
from its tree entity, its id, classes and attributes from its light element), reads the
components without copying, visits attribute names through
`AttributesComponent::forEachAttributeName` without allocating, and is asked only for the key
kinds the sheet's index has: the user agent sheet has only type keys and universal entries, so it
never splits a class list or lists attributes. The list is reused across elements.
Each bucket is gathered at most once per element, however often the element names its key: a
class list that repeats a token, or attribute names that differ only in case, find the same bucket
again and add nothing. Every filed entry sits in exactly one bucket or the universal span, so an
element gathers at most the index's entry count (one per selector-list entry) before duplicates are
removed, whatever its markup; the remaining collection work is one lookup per key the element
carries, linear in the element's own source. Gathering a bucket once per key occurrence instead
would let a rect whose `class` repeats `a` 20,000 times against 2,000 `.a` rules gather 40 million
positions to test 2,000 rules.
Candidates are visited in ascending rule order and passed to the unchanged `MatchSelectorRule`,
which still tests every entry of the rule in list order and returns the first match. Sheet order,
import order and rule order are preserved, so the declarations applied, their order and their
specificities are the same as today; only rules that cannot match are skipped.

`collectMatchedStyleRules` (the editor's matched-rule panel) uses the same candidate list.
`findStyleRuleAtSourceOffset` is a source-offset query and is unchanged.

**Budget.** `StyleResourceBudget::reserveRuleElementMatch` is charged once per candidate tested
instead of once per rule in the sheet. Limits are unchanged. A document whose unindexed cascade
fits the budget charges no more than before, so its styles are unchanged; a document that
exhausted the budget only because of rules that could not match now styles completely. This is
the only intended behavior difference, and it is limited to documents that currently end with
`StyleResourceBudget::rejected()`.

**Memory accounting.** Index bytes (vector capacities plus per-key records and lowercased key
copies) are computed when it is built and added to the stylesheet's charge in the document's
`ParsedPayloadResourceBudget` (`Category::Stylesheet`) alongside the parsed stylesheet bytes, when
a `<style>` element is parsed and when a source edit reparses it
(`RuleIndexCharge.StylesheetChargeIncludesItsRuleIndex` in
`//donner/svg/components/style:style_system_tests`). Imported sheets are not charged to that
budget; their rules and component values are bounded by the import budget
(`StylesheetImportBudget`). The index never exceeds 16 bytes
per selector-list entry plus vector slack, and selector-list entries are already bounded by the
parser's per-stylesheet rule and component-value limits (`RuleParser::kMaximumRules`,
`Stylesheet::kMaximumDeclarations`, `kMaximumComponentValues`). `SVGStyleElement::setContents`
reparses without charging the parsed-payload budget (issue #2726); the candidate list collected
per element is bounded by the index's entry count and reused across elements, and a cascade that
grew it past 4,096 entries (`StyleSystem::kRetainedCandidateCapacity`, the parser's rule limit and
so the most candidates one sheet has once duplicates are removed) releases it afterwards, so a
thread never keeps more than 16 KB of it.

**Wasm size.** The index adds code to the shipped editor and renderer. Milestone A3 runs the Wasm
size test and package size checks against the current ceilings and reports the measured size.

### Matcher Constant Factors

Each is one commit with a before and after benchmark run; a change that does not improve candidate
match time by at least 5 percent is not kept.

- `ComplexSelector::matchChain` allocates a `std::vector<Frame>` on every match attempt. Use a
  small inline-capacity stack (the depth is the number of compounds) and fall back to the heap only
  for long selectors.
- `ComplexSelector::matches` calls `computeSpecificity()` on every successful match. Compute it
  once at parse time and store it in the selector.
- `ShadowedElementAdapter::className()` returns an owning copy and `ClassSelector::matches` splits
  it for every class selector tested. Give the adapter a per-element split class list, built once
  when the element's cascade begins, and let `ClassSelector` search it.
- `PseudoClassSelector` compares `ident` against names on every match. Resolve the name to an
  enumeration when parsing.

### Narrowed Invalidation for Single-Element Edits

**Dependency summary.** `StylesheetComponent::rebuildSelectorCaches` also builds a
`SelectorDependencySummary`: for each id, class and attribute local name that appears anywhere in
a selector, whether it appears only in a subject compound (affects the element itself), in a
compound left of a descendant or child combinator (affects descendants), or left of a sibling
combinator (affects following siblings and their descendants). Names inside the `of S` filter of
`:nth-child(An+B of S)` and `:nth-last-child(An+B of S)` get a direction of their own (see
routing below). It also records whether any selector uses `:has()`, a structural pseudo-class
(`:nth-*`, `:first-*`, `:last-*`, `:only-*`, `:empty`), or a key-less attribute selector. The
summary is the union over the sheet's own and imported rules, and the document view is the union
over applicable stylesheets.

**Routing.** `SVGElement::setClassName`, `setId`, `setStyle`, `updateStyle` and the presentation
attribute path in `MarkPresentationAttributeDirty` compute the affected set instead of setting
`needsFullStyleRecompute`:

- The written element always gets `DirtyFlagsComponent::Style` and loses its computed style.
- Every entity whose own cascade is redone also has its descendants recomputed, whatever the kind
  of edit, because a computed style is the cascade result merged with the parent's computed style:
  a class change that alters an inherited property changes every descendant that inherits it. This
  is what the existing `invalidateComputedStyleForDescendants` and
  `propagateStyleDirtyToDescendants` do for `style` and presentation attributes today. Recomputing
  a descendant re-runs its cascade in this design; stopping the walk where a recomputed style is
  equal to the old one is listed under [Future Work](#future-work).
- For a class change, the changed classes are the symmetric difference of the old and new class
  lists. For an `id` change, the old and new id. For any attribute write, the attribute name.
  A class change also looks up the attribute name `class`, and an `id` change the attribute name
  `id`, because attribute selectors can select on them. Each changed name is looked up in the
  summary: subject-only dependencies redo the element's cascade; descendant dependencies redo the
  cascade of the subtree; sibling dependencies redo following siblings and their subtrees.
- A name inside a pseudo-class argument (`:is()`, `:where()`, `:not()`) is recorded at the widest
  position it can reach: the position of the compound that holds the pseudo-class, widened by any
  combinator to its right inside the argument. A name under `:not()` is a dependency like any
  other, since adding or removing it changes the match.
- `:has()` anywhere, or a key-less attribute selector when the written name is an attribute, falls
  back to `needsFullStyleRecompute`.
- Unfiltered structural pseudo-classes (`:first-child`, `:nth-child(2n)`, `:nth-of-type()` and the
  rest) depend only on tree position and element names, which these edits do not change. The
  filtered forms do depend on attributes: `PseudoClassSelector::matchesNthFunction` passes the
  `of S` selector to `getIndexInParent`, which counts only siblings that match `S`. A class change
  on one sibling can therefore change another sibling's match with no tree mutation:
  `:nth-child(1 of .selected)` counts matching siblings before the subject, so a change to a name
  in `S` on element X redoes X and every following sibling of X; `:nth-last-child(1 of .selected)`
  counts matching siblings after the subject, so it redoes X and every preceding sibling. Each
  redone sibling's subtree is redone too, and the direction is widened to both when the
  pseudo-class sits in a compound left of a sibling combinator. The summary records each name in
  an `of S` filter with these directions, including names nested in `:is()`, `:where()` and
  `:not()` inside `S`.
- The `<use>` and resource shadow instances whose `ShadowEntityComponent::lightEntity` is a dirty
  entity are dirtied too, because they read the light element's attributes. This needs a reverse
  map from light entity to shadow entities, maintained by `ShadowTreeSystem` when it creates and
  tears down clones.

Every affected entity is restyled by the existing selective pass in `computeAllStyles`. Taking
`RenderingContext::tryRetainedUpdate` instead of the full rebuild needs more than the routing
change; see [Retained Update for Style-Only Changes](#retained-update-for-style-only-changes).

**Render-side dependents.** A full restyle also sends the frame through the full rebuild, which
recomputes things that depend on an element without being its style: gradients and patterns built
from their stops and children and drawn by shapes elsewhere, markers, clip paths, masks and
filters, text layout, and references resolved by id (`href`, `url(#id)`). A narrowed edit that
restyles the right elements can still leave those stale, and a computed-style comparison would
not notice. So the following keep the full restyle and full rebuild until the differential edit
test, with its render-side comparisons, shows them local:

- `id` writes (`SVGElement::setId`), which re-target `href` and `url(#id)` references;
- any write to an element that is, or is inside, a paint server, `<marker>`, `<clipPath>`,
  `<mask>`, `<filter>` or `<symbol>`, since its effect reaches the elements that reference it;
- writes that set `needsFullStyleRecompute` for reasons other than selectors today
  (element-specific attributes on lines, convolve matrices, conditional processing, `<use>`
  geometry);
- any write to a text content element or inside one (`<text>`, `<tspan>`, `<textPath>`), since
  restyling a span invalidates its text root's layout.

The narrowed path starts with `class`, `style` and presentation-attribute writes on elements
outside those resource subtrees, plus their `<use>` instances through the reverse map.

**Why presentation attributes request a full restyle today is not stated in the code.** The
plausible dependents are `<use>` instances (covered by the reverse map), paint servers and
gradient stops (`ComputedStopComponent`), and text layout. Milestone A5 starts by running the
differential edit test with the current full restyle replaced by the narrowed set on the whole
corpus, and keeps the full restyle for any attribute whose narrowed result differs until the
missing dependent is identified.

**Stylesheet edits.** Loading imports keeps the full restyle, now one cascade per frame. Rule
edits restyle only the elements the changed rules match; see
[Restyle from Changed Rules](#restyle-from-changed-rules).

#### Retained Update for Style-Only Changes

Removing `needsFullStyleRecompute` alone does not reach the retained path. `PlanRetainedUpdate`
asks `FlagsAreRetainable` (`donner/svg/renderer/RetainedRenderTreeUpdate.cc`) about every dirty
entity, and it refuses `DirtyFlagsComponent::Style` unless `Compositing` is also set: today the
only style change known to be local is an `opacity` write (`markCompositingDirty`). A class edit
marked `Style` alone would still send `RenderingContext::ensureComputedComponents` down the full
rebuild, which tears down and re-instantiates every shadow tree. When the retained path does run,
`ApplyRetainedUpdate` re-runs the selective restyle and recomputes layout for entities flagged
`Layout`, and the next instantiation rebuilds the render instances (`needsInstanceRebuild`), which
re-resolves fill, stroke, clip, mask and marker references from the new computed styles. Every
other computed component that `createComputedComponents` builds from a computed style is not
touched, so A5 adds:

- **A narrowed-style flag.** The router marks every entity it restyles with `Style` and
  `RenderInstance` plus a new `DirtyFlagsComponent::NarrowedStyle` bit, and `FlagsAreRetainable`
  accepts `Style` when either `Compositing` or `NarrowedStyle` is set. Other writers that set
  `Style` keep today's refusal.
- **Per-entity recomputation.** For each entity carrying `NarrowedStyle`, after the selective
  restyle, `ApplyRetainedUpdate` recomputes its layout (`LayoutSystem::recomputeInvalidatedLayout`)
  and its computed path (`ShapeSystem::createComputedPathIfShape`), since geometry properties and
  `display` and `visibility` can come from a class.
- **Fallback for dependents that are not per-entity.** Styled paint trees, nested marker trees,
  filters, gradient stops and text layout are built by whole-document steps
  (`InstantiateStyledPaintTrees`, `InstantiateNestedMarkerTrees`, `FilterSystem`, `PaintSystem`,
  `TextSystem`). The plan therefore declines when a restyled entity is, or is inside, a text
  element, or when its computed style references a paint server, marker, clip path, mask or
  filter. That check runs both before the restyle and after it. The router performs the first
  one when it marks the entity, while the old computed style still exists (marking removes it),
  and sets `NarrowedStyle` only when it passes; `ApplyRetainedUpdate` performs the second on the
  new computed style. When the post-restyle check fails, `tryRetainedUpdate`
  returns false and the frame takes the full rebuild, whose selective pass restyles the same
  dirty entities again. The edit's result is then exactly today's.
- **Shadow instances.** The `<use>` instances of a restyled light element are restyled through
  the reverse map and carry `NarrowedStyle` themselves; `ChangeStaysLocal` keeps refusing the own
  changes it refuses today (transform, compositing, shadow tree) on shadow sources, which a class
  edit does not make.

Making more dependents local (per-entity styled paint trees, for example) is future work; each
needs its own differential edit cases first.

### Ancestor Bloom Filter

**Base-library helper.** `donner/base/BloomFilter.h` is header-only and depends on nothing but
the standard library. It follows the layout the base library uses for its other utilities (a
header in the `//donner/base:base` target, tests in `tests/BloomFilter_tests.cc` inside
`base_tests`, a `donner_cc_fuzzer` named
`bloom_filter_fuzzer` with a corpus under `tests/bloom_filter_corpus`). It provides:

- `BloomHash`: a 32-bit value that callers build once and reuse. `BloomHash::Of(bytes, salt)`
  computes a fixed, documented, platform-independent hash of a `std::string_view` (not
  `std::hash`, whose values differ between standard libraries, so tests and fuzzer corpora are
  reproducible): 32-bit FNV-1a over the salt's four little-endian bytes and then the key bytes,
  followed by the MurmurHash3 `fmix32` finalizer.
  The salt separates key kinds that share spelling, such as the tag `g` and the class `g`.
- `BloomFilter<kLog2Bits>`: a plain bit-array filter with `insert(BloomHash)`,
  `mayContain(BloomHash)` and `clear()`, for sets that only grow.
- `CountingBloomFilter<kLog2Slots>`: 8-bit saturating counters with `insert`, `remove`,
  `mayContain` and `clear`. `insert` reports whether either probed counter is saturated after
  adding both probe contributions, including when the probes coincide. A counter that reaches 255
  is never decremented again, so removal cannot create a false negative; the cost is a stuck false
  positive until `clear()`. `//donner/base:base_tests` covers the saturation signal and removal.
- Both derive their two probe positions from disjoint bit ranges of the 32-bit hash (bits 0 to
  `k-1` and `k` to `2k-1`), as Stylo does, so `kLog2Bits` and `kLog2Slots` are at most 16 and a
  `static_assert` enforces it. Sizing guidance in the header gives the expected false-positive
  rate for a number of keys, and `expectedFalsePositiveRate(keys)` estimates it assuming
  uniform probes and independent occupancy checks. Retained saturated counters after removal
  can raise the counting filter's false-positive rate.

The documented invariant is: after any sequence of operations in which every `remove(h)` follows
a matching `insert(h)`, `mayContain(h)` is true for every `h` inserted more times than removed.
It is tested three ways: a property test inserting and removing random multisets against a
`std::multiset` oracle, a deterministic false-positive-rate test that inserts a fixed set and
counts positives over a fixed probe set (a count, not a timing), and the fuzzer, which decodes an
operation sequence and asserts the invariant after every step. Saturation and `clear()` have
their own unit tests.

**Cascade filter.** `StyleAncestorFilter` in `donner/svg/components/style/` owns one
`CountingBloomFilter<12>` (4,096 counters, 4 KiB) and a stack of the entities whose hashes it
holds. For each element on the stack it inserts the salted hashes of the lowercased tag name, the
id, each class and each lowercased attribute local name, read through `ShadowedElementAdapter` so
shadow entities contribute their light element's id, classes and attributes and their own tag,
exactly as matching sees them.

**Selector hashes.** When `RuleIndex` is built, each selector-list entry stores up to four
`BloomHash` values. Walking left from the subject compound, compounds reached through descendant
and child combinators are ancestors of the subject; the walk stops at the first sibling
combinator, because compounds beyond it are siblings of an ancestor, not ancestors. From those
ancestor compounds it takes id, class, non-universal type and un-namespaced attribute-name simple
selectors, preferring ids, then classes, then attributes, then types (rarer first, as WebKit does).
Simple selectors inside pseudo-class arguments are never used, because `:not()` inverts them and
`:is()` lists alternatives. An entry with no such selector has no hashes and is never rejected by
the filter.

**Check.** After the index produces a candidate rule, the rule is skipped when every one of its
entries has a hash the filter does not contain. A skipped candidate is still charged to the
rule-element budget, but the ancestor walk it skips would have consumed `SelectorTraversalBudget`
steps (`ComplexSelector::matchesCandidate` and `SelectorTraversalNavigation` charge each one), and
once that budget rejects, `StyleResourceBudget::reserveRuleElementMatch` refuses every later
rule. So for a document whose filter-off pass ends traversal-rejected, the filter-on pass can
apply rules the filter-off pass refused. This is permitted, with the same scope as the rule
index's change: a skipped walk only ever fails, so the filter-on pass consumes no more of any
budget than the filter-off pass, and every document whose filter-off pass completes within its
budgets completes with the filter on and produces identical results. Charging the saved steps is
not done, because only the walk itself knows how many steps it would have taken. Serial and
parallel passes (Part D) both run with the filter and make the same filter decisions, so their
traversal charges agree and Part D's parity is unaffected. Two counters
record filter rejections and filter passes that then failed in an ancestor walk (false
positives).

**Keeping the ancestor set correct.** Before matching element E, the filter is synchronized to
E's ancestor chain, which is the chain `ShadowedElementAdapter::parentElement()` produces (shadow
tree parents for shadow entities, stopping where the adapter stops). Synchronization pops stack
entries that are not on the chain, from the top, removing their hashes, then pushes the missing
ancestors from the outermost down. A counter that would reach 255 saturates and could then never
be decremented, which would leave the filter's content depending on history rather than on E. So
when an insertion would saturate a counter, the filter is cleared and disabled (every probe
answers "may contain") until synchronization pops back above the ancestor whose insertion
saturated it, and then rebuilt from the chain. Saturation needs 255 live probe contributions to
one counter. An insertion contributes two when its probes coincide. With that rule, whether the
filter is enabled and what it contains are functions of E's ancestor chain alone, so the filter
is correct in every traversal mode and makes the same decisions in the serial and parallel passes:

- Full pass in storage order: consecutive elements usually share most of their chain, so the
  cost is the difference between two chains. A later change may visit in tree order to make it
  one push and one pop per element; correctness does not depend on order.
- On-demand parent computation: `computePropertiesInto` computes a missing parent before
  inheriting from it, which synchronizes the filter to the parent's chain; the child's match
  happened earlier, before that call, and any later match synchronizes again first.
- Selective pass: dirty entities arrive in arbitrary order and each is synchronized.
- Shadow trees and `computeStylesFor`: the adapter's chain is used. A main-branch shadow root
  is appended as a child of its `<use>` element (`ShadowTreeSystem::createShadowEntity`), so a
  shadow entity's chain runs through its shadow ancestors into the `<use>` element and the light
  ancestors above it, which is the chain matching walks; offscreen shadow roots are not attached,
  so their chains stop at the root. The filter follows whatever chain the adapter produces.
- External-use documents: each registry's traversal owns its own filter.

The filter belongs to one style pass object, created when `computeAllStyles` or
`computeStylesFor` starts and destroyed when it returns. A pass abandoned by a budget rejection
destroys its filter with it, so no later pass can see a half-maintained set. Chain
synchronization is not charged to `SelectorTraversalBudget` or any other style budget, so it adds
nothing to any budget outcome; the only budget difference the filter makes is the skipped walks
above. Its cost is bounded instead by the parser's nesting-depth limit: at most one pop and one
push per ancestor level per element.

**Expected effect.** See [Ancestor Filter Opportunity](#ancestor-filter-opportunity): no
measurable time on the real documents, and up to 902 of 1,913 candidates on the descendant-only
synthetic document. The cost is one synchronization per element (a few hashes per ancestor
changed) and up to four probes per candidate. The goal guards against a regression on the real
documents and measures the effect by counter.

### Shared Computed Styles and the Matched Properties Cache

**Storage.** An immutable `ComputedStyle` record holds a computed style:
`PropertyRegistry properties` and `std::optional<ComputedResourceOpacities> resourceOpacities`
(always set by the cascade; absent only on manually constructed styles).
In Part B it also holds the element's computed custom properties.
`ComputedStyleComponent` holds `std::shared_ptr<const ComputedStyle> style` and keeps
`externalFill` and `externalStroke`, which depend on the element's external-use context. Readers
use the const accessors `style.properties()`, returning `const PropertyRegistry*`, and
`style.resourceOpacities()`, both null while the style is not computed. The record is `const`, so a
reader that wrote through it would fail to compile, and static assertions in
`ComputedStyleComponent.h` fail the build if the stored pointer or the accessor stops being const.
`ShadowTreeSystem::createShadowEntity` emplaces an empty component;
`ExternalUseStyleContext::parent` holds the host's `ComputedStyleComponent`, sharing its record,
and `ExternalUseStyleRetention` keeps the shared record of each retained occurrence. Without the
cache each element owns one record, released when its component is removed (a selective restyle,
a full restyle's clear, an element removal, document teardown) unless an external-use occurrence
still shares it. Those holders charge the document family for the host's dynamic property bytes,
because they can keep the record alive after the host is restyled. Retained bytes are charged per
entity through `StyleResourceBudget::reserveComplexPropertyBytes`; the per-record account below
comes with the cache.

**Cache key.** The cache maps a key to a `std::shared_ptr<const ComputedStyle>`. The key holds
everything a computed style depends on in `computePropertiesInto`:

- The matched rules, in order, with the specificity each was applied at: the stylesheet entity,
  sheet ordinal, rule index and specificity of each rule, hashed incrementally while
  `applyStylesheetRules` runs.
- The local declarations: a hash of `StyleComponent::properties`, computed and stored on the
  `StyleComponent` whenever it changes (parse, `setStyle`, presentation-attribute writes), with
  full `PropertyRegistry::operator==` on a hash match. With Part B, the element's local custom
  declarations (from its `style` attribute) are stored on the `StyleComponent` beside the
  registry, not inside it, and the local registry also holds pending-substitution entries. Both
  join the key: the hash covers the custom declarations' names and token sequences, and equality
  compares them and the pending entries' token sequences. Without that, two sibling rectangles
  with `style="--x:red"` and `style="--x:blue"` matching `rect { fill: var(--x) }` would share one
  record.
- The parent's record, by pointer identity. Two elements whose parents share a record inherit the
  same values, including font sizes resolved against the parent.
- Whether the parent carries `DoNotInheritFillOrStrokeTag`, and whether the element is a symbol
  that is a direct `<use>` instance root (`ApplySymbolInstanceDisplay`).
- The element type of the data entity (`ElementTypeComponent`), which the measured opportunity
  keyed on.
- The root element's record, for `rem` lengths (`ComputedRootFontSize`).

`externalFill` and `externalStroke` stay on the component, outside the shared record, because
`inheritComputedParent` derives them from the element's own fill and stroke states, the
external-use occurrence and the parent component's flags. On a hit they are computed with the same
`ExternalPaintOrigin` calls from the local `StyleComponent` states (equal for every element with
this key) and this element's parent component, which costs two state checks.

Elements are not cached when their data entity, which is the light element for a shadow instance,
has `AnimatedValuesComponent` (`ApplyAnimatedStyle` and the contextual font size read the data
entity), when they are an external-use root (`ExternalRootContext`), or when the cascade reported a
warning to the sink (a cached result would skip the diagnostic). The measured opportunity used
these inputs and found no element whose key matched another's while its computed style differed
(see [Shared Computed Style Opportunity](#shared-computed-style-opportunity)). Six documents are
not evidence that the key is complete; the gate is the differential fuzzer, run with the cache on
and off, whose generator produces presentation attributes, `style` attributes, `<use>` instances,
external-use documents and animations, and, once Part B lands, custom properties declared in rules
and `style` attributes, inherited and overridden, with `var()` in rules, `style` attributes and
presentation attributes.

**Where it runs.** Matching still runs for every element; the cache replaces what follows it:
declaration parsing into the local registry copy, animation hooks, inheritance and font
resolution, which the baseline measures at about 4.6 microseconds per element. On a hit the
element's component takes the cached pointer.

**Lifetime and invalidation.** The cache belongs to the document's style context. It is cleared
whenever computed styles are discarded as a whole (`MarkStylesheetChanged`,
`StyleSystem::discardComputedStyles`, a full restyle), because rule identities and parent records
change. Entries hold strong references, so a cached record outlives the elements that used it
until the next clear or eviction. Narrowed invalidation needs no cache change: a recomputed
element builds a new key, and a new parent record changes its children's keys.

**Eviction.** The cache holds at most 4,096 entries and at most 8 per hash bucket. When either is
full, an insertion evicts by a clock sweep over a reference bit set on every hit, so a long editing
session in which full clears are rare keeps caching its current working set rather than freezing
on the first 4,096 keys. An adversary who forces distinct keys makes every insertion evict, which
costs one sweep step and leaves today's per-element cost.

**Memory budget and ownership.** Retained bytes are charged per distinct record instead of per
entity, and the charge lives exactly as long as the record. Today the 64 MiB per-document limit is
kept inside `StyleResourceBudget` itself (`complexPropertyBytes_` against `limits_`, with the
per-entity `reservations_` map), and `DocumentResourceFamilyBudget` is an optional outer envelope
whose pointer is null unless a document family is configured. The per-document retained-byte
counter therefore moves out of `StyleResourceBudget` into a `RetainedStyleBytes` account: a small
object with an atomic byte count and the 64 MiB limit, always created with the document's style
context and shared by `std::shared_ptr`, which also forwards each charge and release to the family
envelope when one exists. When a record is created (by the cache or for an uncacheable element),
its heap bytes are reserved against that account, and the record keeps a reference to the account
and its byte count; the record's deleter releases exactly those bytes when its last reference
goes, whether the cache evicted it long before or never held it. Eviction only drops the cache's
reference. Records are the only holders of these charges, so the per-entity `reservations_` map
leaves the computed-style path; `StyleResourceBudget::reset()` still leaves retained bytes alone,
and in this design `releaseAll()` does not release computed-style bytes itself: clearing the
`ComputedStyleComponent`s it accompanies drops the references, and each record's deleter releases
once, so nothing is released twice. Because the account is shared, a record that outlives the
`StyleResourceBudget` object (through `ExternalUseStyleRetention`, for example) still releases
correctly, and releases from worker threads in Part D are atomic. A hostile
document therefore hits the same 64 MiB retained-property limit as today: distinct keys create
distinct records, each charged until no element points at it. A test
(`style_index_differential_tests`, hostile case) builds a document whose elements each carry a
distinct attacker-sized `stroke-dasharray` and asserts that the budget refuses at the same element
with the cache on and off.

### Interaction with Existing Incremental Paths

- The selective style pass and its two-part invariant (dirty flag plus removed computed style)
  are unchanged; narrowing only chooses which entities enter it.
- `ExternalUseStyleContext` and `ExternalUseStyleRetention` cascade referenced external documents
  with their own registries and stylesheets; each registry has its own index, filter, cache and
  summary, and the external-use aggregate budget is charged per candidate exactly as above.
- Text-content-dependent selectors (`:empty`) keep their existing `selectorsDependOnTextContent`
  handling.
- No path in this design reparses the document.

## Proposed Architecture, Part B: Cascade Features

### Custom Properties and var()

**Parsing.** Declarations whose name starts with `--` are already kept by the declaration parser
as `css::Declaration` with their component values; they stop reaching
`PropertyRegistry::parseProperty`. The bare name `--` is rejected, as the specification reserves
it. When a stylesheet, `style` attribute or presentation attribute is parsed, each declaration
records whether its component values contain a `var()` function at any depth
(`Declaration::containsSubstitution`), so the cascade never rescans values. A malformed `var()`
(no `--` name, or tokens other than a fallback after the name) makes the declaration invalid at
parse time, as today.

**Custom cascade.** `computePropertiesInto` keeps, next to the local `PropertyRegistry`, a small
vector of winning custom declarations `(name, declaration, specificity)`, filled by
`ApplyRuleDeclarations` and by the element's `style` attribute with the same "replace unless the
stored specificity is higher" rule as other properties, so source order breaks ties exactly as
today.

**Computed custom properties.** A `CustomPropertyMap` is an immutable sorted vector of
`(RcString name, std::shared_ptr<const TokenSequence> value)`. An element that declares no custom
property shares its parent's map pointer, which is the common case and costs nothing per element.
An element that declares some builds a new map: the parent's entries overlaid with its own, after
substituting `var()` references inside its own declarations. Only the element's own declarations
can form a cycle, because inherited values were substituted on the parent. Substitution walks
references depth-first with an explicit stack and an in-progress mark per name, as Blink's
`CascadeResolver` does; meeting an in-progress name marks every name on the stack from it
upward as cyclic, and cyclic names become the guaranteed-invalid value (absent from the map).
CSS-wide keywords in a custom property follow the specification: `initial` is the
guaranteed-invalid value, `inherit` and `unset` take the parent's value. Untyped custom properties
hold tokens, never lengths, so the font-size dependency cycles of registered properties cannot
arise.

**Substituting into ordinary properties.** In the local registry a declaration with
`containsSubstitution` does not parse; it sets a pending-substitution entry for its property at
its specificity, which wins or loses the cascade exactly as a parsed value would. A shorthand
records one pending entry for the shorthand declaration and marks every longhand it covers as
pending on that entry; after substitution the shorthand is parsed once and its result is
distributed to the longhands that still hold the mark (a longhand a later declaration replaced
keeps its own value). After the custom map is computed, each pending entry is substituted (the
fallback is used when the referenced name is absent), and the result is parsed with
`PropertyRegistry::parseProperty` at the entry's specificity into a property slot reset to
unset. A parse failure, a guaranteed-invalid reference without fallback, or a value past
the size cap makes the property invalid at computed-value time: it is left unset, so an inherited
property inherits and any other takes its initial value. Inheritance then runs as today.

**Presentation attributes.** A presentation attribute whose value contains `var()` is stored as a
pending-substitution entry at presentation-attribute priority and substituted like any other,
which is the behavior the SVG Working Group resolved to allow and Firefox and Safari implement.
The parse model is not yet specified, so this is an open question below.

**Limits.** A substituted value is capped at 65,536 component values and 1 MiB of source bytes,
below the 2 MiB both Blink and Stylo use, because an SVG document's whole parsed-payload budget is
smaller than a web page's. The caps are checked while substitution appends, and the first append
that would cross one aborts the substitution, so an exponential value is never built in full.
Substitution also charges each produced component value to
`StyleResourceBudget::reserveDeclarationApplication`'s component work and its bytes to the byte
work, so one document's total expansion is bounded by the existing aggregate limits (16 Mi units
each). Past any cap the value is guaranteed-invalid and a diagnostic is reported once.

**Retained bytes of maps and token sequences.** A map or token sequence can outlive the record
that created it: a child's record shares its parent's map, an overlaid map shares the parent's
unchanged token sequences, and any record can be kept alive through `ExternalUseStyleRetention`.
Charging such an allocation to the first record that holds it would release the reservation while
the allocation stays live. So each `CustomPropertyMap` and each `TokenSequence` is created through
one factory that reserves its heap bytes against the `RetainedStyleBytes` account of Part A and
gives the `std::shared_ptr` a deleter holding the account and the byte count, exactly like a
computed-style record. The reservation is released when the allocation's last reference goes,
whichever record or map held it last. A record charges only its own bytes; a map charges its
entry vector, and each token sequence charges itself once however many maps share it. A refused
reservation makes the custom property guaranteed-invalid with the same one-time diagnostic as a
size cap.

**`revert` and `revert-layer`.** Donner does not support `revert` today: it is not a CSS-wide
keyword in `PropertyRegistry` (only a reserved font-family name), so a declaration using it fails
to parse and is dropped. Supporting it means rolling the cascade back to the previous origin or
layer, which the single "replace unless higher" slot per property cannot do; it needs every
candidate declaration kept per property. Part B keeps them unsupported, with today's behavior,
and lists them as an open question.

**Interactions.** The rule index is unchanged: custom properties do not affect matching. The
matched properties cache key needs the extension described in
[Shared Computed Styles](#shared-computed-styles-and-the-matched-properties-cache): an element's
custom map is a function of the custom declarations of its matched rules (covered by the matched
rules' identities), its local custom declarations (added to the key), and its parent's computed
custom map. The inherited map is held in the parent's record, so the parent record pointer
already in the key captures it: equal parent pointers mean equal inherited maps. Narrowed
invalidation already recomputes the descendants of every restyled element, which is what a changed
custom property needs; the future "stop when equal" optimization must include the custom map in its
equality. `collectMatchedStyleRules` and the editor's annotations show custom declarations like any
other.

### Cascade Layers

**Parsing.** `StylesheetParser` parses `@layer a, b;` statements and `@layer name? { ... }` blocks,
recursively, with nested blocks producing dotted names (`@layer a { @layer b {} }` is `a.b`) and
nameless blocks producing anonymous layers with a unique internal name. `@import url() layer(x)`
and `@import url() layer` are retained with their layer instead of dropped. Each `SelectorRule`
gains a `layer` id into the stylesheet's layer table; unlayered rules use a reserved id. Nesting is
capped at 32 levels and a stylesheet at 4,096 layers, and a document at 16,384 distinct layers
across all its stylesheets and imports, so the document-wide rank always fits in 16 bits; past any
cap the block or import is dropped with a diagnostic.

**Imports.** Imports with a layer start loading where they are dropped today. They go through the
same loader, resource limits and import-nesting limits as other imports
(`ResourceManagerContext::loadStylesheetImports`); no new loading path or origin is added.

**Order.** Layer order is hierarchical, not the global order of first appearance. Cascade
Level 5 orders sibling layers by first declaration within their parent, places a nested layer's
order inside its parent, and places a layer's own declarations (those in the layer but in none of
its sublayers) after its sublayers for normal declarations; unlayered author styles are the own
declarations of the implicit outermost layer. For a stylesheet of `@layer a, b;`, then
`@layer b { rect { fill: blue } }`, then `@layer a.x { rect { fill: red } }`, every normal
declaration in `b` outranks every one in `a` and `a.x`, although `a.x` is declared after `b`.

The tree is built from source order, not from the flattened imports-first order the cascade uses
for rules. Cascade Level 5 treats an imported sheet's contents as if written at the position of
its `@import`, and allows `@layer` statements before imports, so the position of each import among
the layer statements around it decides order. For example, with `@layer a, b;`, then
`@import url("b.css") layer(b);`, then `@layer a { rect { fill: red } }`, and `b.css` containing
`rect { fill: blue }`, the statement declares `a` before `b`, so `b` ranks above `a` and blue
wins. Building the tree imports-first would create `b` first and make red win.

So each parsed stylesheet records, in source order, one layer stream: its `@layer` statement
names, its `@layer` blocks (with their nested blocks), and a marker for each retained `@import`
with the import's layer (`layer(x)`, the anonymous `layer`, or none). The flattened
`importedStylesheets` gain, per entry, the index of the import that loaded it and its parent
entry, so each import marker can find its sheet. The document builds one layer tree by walking the
applicable stylesheets in the order the cascade applies them and, within each, its stream in
source order: a statement or block adds its dotted path if absent; an import marker walks the
imported sheet's own stream at that point, under the import's layer node (`layer(x)` adds `x` if
absent, `layer` adds a fresh anonymous node, an unlayered import uses the current node). New
children are appended after their existing siblings, so sibling order within a parent is the
order of first appearance across that whole ordered stream. A sheet imported into `x` therefore
places `@layer y` at `x.y`. An import that has not loaded yet contributes nothing, so the tree is
provisional until imports resolve; when one loads, the tree and ranks are recomputed and one full
restyle follows, as below. Ranks
are assigned only after the whole tree is known, by a post-order walk: a node's children receive
ranks in sibling order, then the node's own declarations receive the next rank. The root's own
declarations, the unlayered author styles, therefore get the highest rank. Presentation attributes
get rank 0, below every author layer, placing them in the presentational-hint origin the
specification describes; with no layers present the root is the only node, so every author rule
gets one rank and presentation attributes still lose to all of them, which is today's result.

Rules store their layer's node id, not a rank; the rank comes from the document's rank table when
`MatchSelectorRule`'s specificity is built, so recomputing ranks never rewrites rules. The tree
and ranks are recomputed whenever a stylesheet is parsed or replaced, an import loads, or an
incremental edit adds, removes or reorders an `@layer` declaration (Part C already reparses the
whole stylesheet when a window contains an `@layer` rule). If the rank table changes, styles are
discarded and the document takes one full restyle. The document-wide cap of 16,384 layer nodes
bounds a post-order walk to at most 16,385 ranks (nodes plus the root's own), which fits the
16-bit rank.

**Priority.** `css::Specificity` gains a 16-bit layer rank compared after `SpecialType` and before
the `(a, b, c)` tuple. For the important types (`Important`) the comparison inverts the rank, as
Blink flips its layer bits, so an earlier layer's important declaration wins and unlayered
important declarations lose to every layer's. The `style` attribute is element-attached and is not
layered (its special types already order it). Because layer and specificity are compared as one
value, the existing source-order application with "replace unless higher" still yields the
specified order with no sort.

**Risk.** `Property::inheritFrom` compares a parent's specificity with the child's to decide
inheritance. Adding the layer rank changes that comparison for layered declarations; Milestone B5
pins the current behavior for unlayered documents with tests and adds layered cases before
changing it.

**Interactions.** The index stores layer-agnostic keys; the matched properties cache key carries
the specificity each rule was applied at, which now includes the rank; a change in layer order
is a stylesheet change and takes the stylesheet path (Part C).

### Out of Scope

`@property`, `env()`, `attr()`, `@media`, `@supports`, `@container`, `@scope` and SMIL animation
of custom properties, as listed under [Non-Goals](#non-goals).

## Proposed Architecture, Part C: Live Stylesheet Editing

### Incremental Stylesheet Reparse

**Entry point.** An `XMLMutation` carries only the node and its new `value`, not where the text
changed. `SVGDocument::applySourceEdit` already holds the edit's `XMLSourceDelta` (offset, removed
and inserted lengths, in document source bytes) in its `ApplySourceEditResult`; it passes that
delta to the style projection together with the `NodeValueChanged` mutation. `ProjectStyleContents`
keeps building the element's concatenated text and its source map, then maps the delta's start and
end through the old source map (`mapToLocalCssOffset`) to stylesheet offsets and calls
`StylesheetComponent::applyTextEdit` instead of `parseStylesheet`.

**When the edit cannot be mapped.** The source map has a segment for a text or CDATA child only
when the child's source bytes and its decoded value have the same length, which is the case when
the child contains no entity or character references (`ProjectStyleContents` adds the segment only
then). Decoding is not proportional to bytes: typing the third character of `&amp;` changes the
decoded text from three characters to one. So the incremental path runs only when the edited child
has a segment both before and after the edit, and the old and new bytes of the edited range contain
no `&`; otherwise the stylesheet is reparsed as today. Edits to CDATA content map one to one, since
CDATA is not decoded. When the style text is held on the element's own value rather than in text
children (the `!foundTextChild` branch of `ProjectStyleContents`), the same rule applies to the
single value segment, which exists only when the value's source span and decoded length match.
`ApplySourceEditResult::sourceDeltas` can hold more than one delta; the incremental path runs only
when exactly one delta falls inside the `<style>` element's content, and any other shape takes the
full stylesheet reparse.

**Rule window.** Every retained rule already carries its `ruleSourceRange`. The window starts at
the start of the last top-level rule that begins at or before the edit start (or at the sheet
start) and ends at the end of the first top-level rule that ends at or after the edit end (or at
the sheet end). The window text is parsed with the same rule parser and limits.

**Resynchronization.** The window parse is accepted only when its token stream ends, at top
level, exactly at the token that completed its last rule (the `}` closing a qualified rule or
block at-rule, or the `;` ending a statement at-rule), with nothing after it in the window but
whitespace and closed comments, and that end lies on an old rule boundary. Anything else is a
pending state that a fresh parse would carry into the following text: an unfinished qualified-rule
or at-rule prelude (`.ne` typed before a rule, or a deleted `{` leaving `b c}` that would absorb
the next rule), a stray top-level `}`, which CSS consumes as prelude text, an open block, comment,
string or `url(`, or a trailing backslash that would escape the next character. In that case the
window end moves to the end of the next old rule and the window is parsed again, until it is
accepted or reaches the sheet end.

**Cross-rule parser state.** Tokenizing restarts cleanly after a completed rule's final token, but
Donner's parsers carry state from rule to rule, so token-level resynchronization is not enough.
Reading `StylesheetParser::Parse`, `StylesheetParseState`, `HandleAtRule` and
`RuleParserImpl::consumeListOfRules`, that state is:

- `StylesheetParseState::importsAllowed`: true until a retained qualified rule or an at-rule that
  ends the import section (`AtRuleEndsImportSection`: an `@namespace` statement, or any of the
  block at-rules it lists, including `@layer` blocks; an `@layer` statement does not end it) has
  been seen. It decides whether a later `@import` is kept.
- The accumulated `imports` and `fontFaces` lists, in order.
- Cumulative resource accounting: `Stylesheet::SecurityStats` (rules, declarations, component
  values, `rejected`), the rule-count cap `RuleParser::kMaximumRules` checked against the running
  rule count, and the shared `ComponentValueParsingBudget` across every rule. A parse that hits a
  limit stops early, so everything after that point depends on everything before it.
- `@charset` removal (`maybeRemoveCharset`), which applies only at the start of the sheet.
- No namespace state: `@namespace` rules are not recorded, and the selector parser does not consult
  declared prefixes today. If namespace prefixes are later plumbed into selector parsing, the
  prefix map becomes cross-rule state and joins this list.

The window parse is seeded with the old parse's state at the window start (`importsAllowed`, and the
running counts), and a window that starts at offset 0 goes through the same `@charset` handling as
a full parse. `RuleParser::kMaximumRules` counts raw rules, including invalid ones that
`StylesheetParser` later drops, and that raw count at each retained rule's start is not stored
today; the parse records it per retained rule (alongside `ruleSourceRange`), together with the
running declaration and component-value totals, so a window can be seeded exactly. The equality
covers the resulting `css::Stylesheet`. Parse warnings are not part of it: the style projection
parses with a disabled warning sink today (`StylesheetComponent::parseStylesheet` without a sink),
so there are no stored warnings for the unchanged prefix and suffix to keep or shift; if a warning
sink is added later, warnings of unchanged rules are kept with shifted ranges and only the
window's are re-emitted. The window is accepted only when, in addition to the token-level condition,
the parser state at the window end is the same in the old and the new parse: `importsAllowed` is
equal, the window contains no `@import`, `@namespace`, `@font-face` or `@layer` rule before or
after the edit, the spliced totals stay within every limit, and the old parse was not stopped by a
limit (`rejected`). Otherwise the whole sheet is reparsed. For example, in
`a{} @import url(i); b{}`, deleting `a{}` empties the window; the old state at the window end has
`importsAllowed` false, the new one true, so the edit takes the full reparse and the import is kept,
as a fresh parse keeps it. The equality argument is then: the parser state at both window boundaries
is the same in the old and new parse, the text outside the window is unchanged, and each rule's
parse depends only on its text and the state at its start, so a fresh parse of the new text is the
old parse of the prefix, the window parse, and the old parse of the suffix.

**Splice.** The new rule vector is the rules before the window, the window's rules with source
ranges shifted to sheet offsets, and the rules after the window with ranges shifted by the edit's
length change. Rules outside the window keep their identity. The index, dependency summary and
filter hashes are rebuilt for the sheet (they are linear in its rules and the sheet is capped at
4,096 rules).

**Behavior while the text is invalid.** The result always equals a fresh parse of the new text,
which is what export and reload produce, so the rendering follows the specification's error
recovery while a rule is mid-edit: a declaration with an incomplete value is dropped, and a
deleted closing brace makes the following rule text part of the broken block until the brace
returns. The measured brace case shows the visible effect today. Holding the last good rules while
the text is invalid is an editor policy question below; the engine does not keep stale rules.

### Restyle from Changed Rules

The rules removed from the window and the rules added to it are the changed rules; a window rule
whose text equals the rule it replaces is unchanged. The elements a changed rule matches, old or
new, are found by a scan, not a reverse map: every styled entity in the registry, light and shadow
alike, is checked against the rule's index keys with the same key test candidate collection uses
(id equality, a class in the element's class list, the tag, an attribute name), and only entities
that pass get the full `MatchSelectorRule`, through `ShadowedElementAdapter` so a shadow entity is
matched against its own shadow-tree structure. No key-to-element map exists, and none is added: it
would have to be maintained on every class, id, attribute and tree mutation. The scan's cost is
one key test per entity per changed rule; at the measured upper bound of 155 ns per full rule
test, a 5,137-entity document costs at most 0.8 ms per changed rule, and a key test is a fraction
of a full test. A keystroke changes one rule, which is scanned twice (its old and its new
version), so the bound is about 1.6 ms on the 5,137-entity sprite and about 0.2 ms on the
622-entity splash, where the keystroke goal is set. A rule whose subject compound has no key (a
universal entry) gets a full match against every entity, at that same upper bound. The scan is
charged to the rule-element budget. Matching elements, their descendants and their `<use>` instances
are marked for the selective pass; the matched properties cache is cleared because rule identities
changed. A changed rule that uses `:has()`, or a change in layer order, falls back to one full
restyle.

**When the scan's allowance runs out.** The scan runs on its own pass allowance (a fresh
`StyleResourceBudget` with the document's limits), charging rule-element matches and
`SelectorTraversalBudget` steps like a cascade. An element the scan did not reach cannot be
treated as a non-match: it may still hold a style from a removed or modified rule without being
marked dirty. This can happen even when a fresh cascade of the new text completes, because an
edit that replaces several rules scans both their old and new versions while a fresh cascade
tests only the new ones. So when the scan is refused by any limit, its partial marks are
discarded and one full restyle is requested (`needsFullStyleRecompute`), which runs with fresh
pass allowances and gives exactly the result of a fresh cascade, budget outcome included.

**Budget rejection in the other new scans and pre-passes.** Each one this design adds either
charges nothing and cannot be refused, or has a stated fallback:

- **Narrowed invalidation (Part A).** The dependency lookups are hash lookups in the stylesheet's
  `SelectorDependencySummary`, one per changed name, and the marking walks visit at most every
  entity once; neither is charged to a style budget, so neither can be refused. The selective
  pass they feed has its existing budget behavior, the same as when it follows today's full
  restyle request.
- **Retained-update eligibility (Part A).** Both checks read component presence and computed
  values of the restyled entities and charge nothing. If the selective restyle inside
  `ApplyRetainedUpdate` is refused by a budget, the update declines exactly as when the
  post-restyle check fails: `tryRetainedUpdate` returns false and the full rebuild runs with
  fresh pass allowances, so the frame's result is today's.
- **Layer-tree rebuild (Part B).** The walk visits each layer statement, block and import marker
  of the applicable stylesheets once; their number is bounded by the stylesheet parse limit
  (`RuleParser::kMaximumRules` per sheet) and the number of loaded imports, so it is not charged
  and cannot be refused. A rebuild is always followed by one full restyle, which has the normal
  budgets.
- **Parallel pre-region step (Part D).** It emplaces the computed-style slots the serial pass
  also emplaces and creates context objects; it charges nothing. A refusal inside the parallel
  region abandons the pass and reruns it serially, as described under budgets, and the
  pre-emplaced slots are what that serial rerun would create, so they stay.

A keystroke in a `class`, `id` or `style` attribute is an attribute edit, not a stylesheet edit:
it takes the `AttributeValue` scope and reaches `SVGElement`, where Part A's narrowed invalidation
(Milestone A5) handles it.

### CSS Highlighting and Completion

This is a scoped plan: what the engine provides and how the editor consumes it, not a visual
design.

**Regions.** CSS regions come from the XML token stream the editor already uses
(`xml::Tokenize`): the text content of `<style>` elements and the values of `style` attributes.
Presentation attribute values are CSS values too, and are classified as values of their property.

**Classification.** Each region is tokenized with `css::parser::details::Tokenizer`, and a small
state machine over the tokens tracks whether it is in a selector prelude, an at-rule prelude, a
declaration name or a declaration value. `ColorIndex` in `donner/editor/TextBuffer.h` has generic
classes only, and the mapping uses them without adding new ones: selector type names and at-rule
names map to `Keyword`, class and id selectors and custom property names to `Identifier`, known
property names to `KnownIdentifier`, numbers and dimensions to `Number`, strings and `url()` to
`String`, comments to `Comment`, braces, colons, semicolons and combinators to `Punctuation`, and
unknown property names and bad tokens to `ErrorMarker`. Distinct selector and property colors would
need new `ColorIndex` values and palette entries; that is a visual-design choice left out of this
plan. Classification works on one region at a time and, after an edit, reclassifies only the edited
region.

**Completion contexts and data sources.**

| Context           | Suggestions                                       | Data source                                                                                                   |
| ----------------- | ------------------------------------------------- | ------------------------------------------------------------------------------------------------------------- |
| Selector prelude  | element names, `.class`, `#id`                    | SVG element registry; the live document's `ClassComponent` and `IdComponent` values                           |
| Declaration name  | property names and declared custom property names | `PropertyRegistry::propertyNames()`; custom declarations in the document's sheets and `style` attributes      |
| Declaration value | keywords valid for the property                   | a new `PropertyRegistry::keywordValues(name)`, built from the keyword tables the property parsers already use |
| Inside `var(`     | declared custom property names                    | same as above                                                                                                 |
| At-rule name      | `@import`, `@namespace`, `@font-face`, `@layer`   | the at-rules the stylesheet parser supports                                                                   |

Contexts are detected from the CSS token state instead of the character scan in
`MakeStylePropertyContext`, which is replaced. Left out deliberately: units and numeric value
templates, color pickers, documentation on hover, browser-compatibility data, formatting and
linting.

## Proposed Architecture, Part D: Parallel Style Traversal

### Opportunity and Threshold

Per-element style cost in the baseline (first-frame style time divided by elements cascaded) is
17.4 microseconds on the splash, 6.1 on the sprite, 6.7 on the tiger, 38.8 on the synthetic
2,000-element document and 8.8 on the `<use>` document. Part A lowers these; outside matching an
element costs 2.1 microseconds with shared records (see [Per-Element Cost](#per-element-cost)).
A subtree task of 64 elements therefore carries roughly 0.3 to 1 ms of work, far above the
per-task cost of a work-stealing deque, so the costs that decide the threshold are waking idle
workers and the serial work around the parallel region (setup, warning commit, font
synchronization). [0073-task_scheduler](0073-task_scheduler.md) measures the wake latency. With
four workers, a pass of 1,000 elements at 5 microseconds (5 ms serial) can finish in roughly a
quarter of that plus the wake latency; a pass of 200 elements (1 ms) cannot repay a wake that costs
a meaningful fraction of a millisecond. The initial threshold is therefore 1,000 elements to
cascade, below which the serial path runs unchanged; Milestone D3 replaces it with a measured
curve. On the measured documents the splash (622) and tiger (484) stay serial and the sprite
(5,137) and `<use>` document (4,425) go parallel. Most single illustrations are below the
threshold; documents with thousands of elements, `<use>`-heavy documents and full restyles of
large documents are what this part serves.

Stylo, the engine that ships parallel style, does the same kind of chunking: "The traversal starts
in sequential mode, and optionally parallelizes as it discovers work"
([driver.rs](https://github.com/servo/stylo/blob/main/style/driver.rs)); it hands children to
other workers only in chunks of `work_unit_max`, keeps at least `local_queue_size` of work for the
current thread, advances one DOM level at a time, and gives each job a thread-local style context
holding its Bloom filter and sharing cache
([parallel.rs](https://github.com/servo/stylo/blob/main/style/parallel.rs),
[context.rs](https://github.com/servo/stylo/blob/main/style/context.rs)). Firefox's defaults are 16
nodes per work unit and a local queue of 32 on the main thread
([StaticPrefList.yaml](https://github.com/mozilla-firefox/firefox/blob/main/modules/libpref/init/StaticPrefList.yaml)),
for a browser's per-node cost; Donner's initial grain of 64 elements reflects its higher measured
per-element cost and is calibrated in Milestone D3.

### What Must Hold for Parallel Cascading

- **No structural registry change inside the parallel region.** EnTT's registry is not thread
  safe for structural changes: entity creation, emplace or remove on a pool, and first use of a
  component type all grow shared vectors and maps. Its documentation says "the entire registry is
  not thread safe as it is", that a set of components can be iterated by several threads while
  none are assigned or removed, and that "the creation of pools and resources within the registry
  is not necessarily thread safe"
  ([EnTT entity documentation](https://github.com/skypjack/entt/blob/v3.16.0/docs/md/entity.md)).
  Making the storage itself concurrent would require forking EnTT's sparse set, storage and
  registry (pool lookup, `contains` and `index` are not virtual, and the pools map, free list and
  signals cannot be customized), adding a lock or atomic to every lookup (measured at roughly 4 to
  8 times the cost of an uncontended `get` of 8.7 ns), and accepting nondeterministic packed
  iteration order, which Donner's output depends on (for example, styled paint trees are
  instantiated in `view<ComputedStyleComponent>` order and recycled entity ids are reused last in,
  first out). That route is rejected. The style pass instead performs all structural work before
  the parallel region and keeps the existing serial phases serial.
- **What the pre-region step does.** A census of the full pass on the splash, the sprite, the
  2,000-element synthetic and the `<use>` documents found exactly one structural operation per
  element, the placeholder `get_or_emplace` of `ComputedStyleComponent` over
  `view<TreeComponent>` that `computeAllStyles` already performs, and none inside the cascade loop,
  with no entity or pool creation. Shadow-tree instantiation is heavily structural (about four
  component additions per created entity) and stays serial between the two `computeAllStyles`
  calls. Before workers start, the pass therefore: emplaces the placeholders (full pass) or, for
  the selective pass, emplaces slots for every dirty entity and any ancestor that lacks one (the
  serial selective pass emplaces in its loop, and `computeStyle` and `inheritProperties` emplace a
  missing parent on demand); creates every pool and context object the cascade touches
  (`StyleResourceBudget`, the animation contextual-work context) and fixes EnTT's type ids
  (`ENTT_USE_ATOMIC`, or by touching every component type the pass reads up front); and, for the
  selective pass, finds the roots of the dirty subtrees, the dirty entities whose parent is clean.
  Workers then receive a `const Registry&`: on a const registry `try_get` and `all_of` cannot
  create a pool, while non-const `get` and `view` can. Inside the region a worker writes only the
  computed-style slot of an entity it owns and its own per-worker state. A debug guard records
  every pool's size and the entity count before the region and asserts they are unchanged after
  it.
- **The serial placeholder step is small.** With a pointer-sized slot to a shared record, the
  placeholder step costs 0.04 microseconds per element on the sprite, clock reading included
  ([Per-Element Cost](#per-element-cost)), about 1 percent of its style time, against 0.67 with
  the 4,840-byte `PropertyRegistry` inline in the component, so it does not cap the speedup of four
  workers. D3 measures the serial phases of the parallel pass.
- **Parents before children.** Inheritance needs the parent's computed record. The pass is a
  top-down traversal from each root inside one flat `TaskScope`: a task cascades its root, then
  walks each child subtree smaller than the grain (initially 64 elements) with an explicit,
  heap-allocated stack, and spawns each larger child subtree into the same scope with
  `trySpawn`. A spawn from a running task goes onto that worker's own deque, so subtrees spread
  by ordinary work stealing between workers, as the scheduler intends for small work. If
  `trySpawn` meets the queue bound it returns false and the task pushes that subtree onto its own
  heap stack instead of running it as a nested call. Tasks never wait, so a task's native stack use
  is one task frame whatever the tree depth, and only the pass owner waits, once. The pass owner
  is usually the render worker, which is not a scheduler worker: its root spawns go to the
  scheduler's global queue and it sleeps on its parker until the pass's counter reaches zero,
  running no tasks itself. A child never
  starts before its parent's record is published, which the spawn order guarantees.
  The on-demand parent computation in `computePropertiesInto` stays for the serial path and is an
  assertion failure in the parallel path. Offscreen shadow trees are separate roots, started after
  the main tree; entities that no root reaches are cascaded by a serial sweep afterwards, as the
  storage-order loop reaches them today.
- **Budgets.** The serial pass defines every budget outcome; a parallel pass is kept only when no
  budget could have refused in either path. The `StyleResourceBudget` work counters (rule-element
  matches, declaration applications and their component and byte work, selector traversal) are
  charged against per-worker shares of the remaining allowance. Matching and traversal work is the
  same per element in both paths, because the ancestor filter's decisions are a function of each
  element's chain (Part A clears and disables it on saturation rather than keeping stuck counters),
  but declaration-application work depends on cache hits, and a per-worker cache can hit where the
  serial cache, with its clock eviction, misses. So in a parallel pass declaration work is charged
  as if every element missed the cache: each element is charged the declaration applications,
  component work and byte work its matched rules and local declarations would cost on a miss, which
  is at least what the serial pass charges for it. Retained computed-style bytes go through the
  shared `RetainedStyleBytes` account of Part A, whose reserve is a compare-exchange loop that never
  overshoots the limit. With per-worker caches a parallel pass can create a different number of
  distinct records than the serial pass, so the retained total differs by schedule and cannot decide
  a refusal by itself. Each worker therefore also sums, for every element it styles, the bytes of
  the record that element ends up holding, which is what a per-entity charge would be; every
  distinct-record total, serial or parallel, is at most that sum. Both comparisons are against the
  allowance remaining when the pass starts, since records held by entities outside a selective pass
  already occupy the account. The parallel pass is abandoned whenever any work share runs out, any
  reserve on the account is refused, or the per-entity sum reaches the remaining retained-byte
  allowance. Abandoning drops every computed style the pass wrote, the per-worker caches and the
  per-worker warning buffers (so their record references and charges go), restores the work counters
  to their values at the start of the pass, and reruns the pass serially. Below the abandon point
  neither path can refuse anything, because every quantity the parallel pass checks bounds the
  serial pass's from above, so their outcomes match; at or above it the serial rerun decides which
  element is refused, so a budget-limited document gets exactly the serial result and only documents
  near their limits pay twice.
- **Warnings.** Each task records an entity's diagnostics in two per-entity buffers: those from
  before its parent is computed in the serial `computePropertiesInto` (rule application) and those
  from after (animation, colors, inheritance, fonts). After the join the commit replays the serial
  pass's order exactly: it walks entities in the same storage order the serial loop uses, and for
  an entity not yet emitted sends its first buffer, then recursively emits its parent if the parent
  is not yet emitted, then sends its second buffer. That is the order the serial on-demand parent
  recursion produces.
- **Ancestor filter and cache.** Each worker owns a `StyleAncestorFilter`; its chain
  synchronization already makes it independent of visit order. Each worker owns a matched
  properties cache; records are immutable and shared through atomic reference counts, so a hit
  on any worker returns a correct style, and only the number of distinct records can vary with
  scheduling. The memory goal of Part A is measured at zero workers.
- **Serial cases.** Animated documents (animation hooks charge a shared contextual-work budget),
  external-use registries, passes below the threshold, and builds or runtimes without workers run
  the serial path.
- **Document access.** Style runs inside the caller's document access (the renderer's or the
  editor worker's). Workers act on its behalf: they read through the `const Registry&` and entity
  handles the caller obtained before the fork, never call public DOM API (which acquires
  `DocumentReadAccess` or `DocumentWriteAccess`), and never acquire a document guard. The caller
  joins before its access ends, so in `ThreadingMode::ConcurrentDom` other threads stay excluded
  for the whole pass, exactly as with the serial pass. The debug assertion that a task does not
  take a guard fires only on guard acquisition; it does not detect a task reading the registry
  through some other path, so the rule is enforced by the const view and review.

### Correctness

The parallel pass must produce the same computed styles as the serial pass. The differential
cascade test and fuzzer of Part A gain a worker-count dimension (0, 1, 2, 4, 8) and the
scheduler's seeded mode, which runs tasks on one thread in an order chosen from a seed so an
ordering-dependent failure reproduces. The seeded mode exercises task orders, not real steals or
the deque's compare-and-swap; those are covered by the real worker counts, the scheduler's own
stress tests and ThreadSanitizer. `style_parallel_tests` runs in the ThreadSanitizer lane that
[0073-task_scheduler](0073-task_scheduler.md) adds. A race test counts as proven only when it was
observed red: the injected-delay test delays one worker between computing a child and publishing
it, together with a deliberately broken publish order, and must fail; with the real order it
passes. No correctness test has a wall-clock deadline; generous hang guards only.

## Security / Privacy

Stylesheets and documents are untrusted input.

- **Index size.** One record per selector-list entry, bounded by the parser's existing stylesheet
  limits, and charged to the parsed-payload stylesheet budget before it is retained. Build time is
  O(n log n) in selector entries.
- **Key flooding.** Keys are attacker-chosen strings. Sorted vectors with binary search have no
  hash collisions to exploit; the worst case per lookup is O(log n) string comparisons.
- **Candidate explosion.** A sheet can place every rule under one class and give every element
  that class. Candidates then equal rules, which is today's cost, and the per-candidate
  rule-element budget stops it at the same limit as today.
- **Selector depth and backtracking.** Unchanged: matching still charges
  `SelectorTraversalBudget` per step, and the inline backtracking stack falls back to the heap
  beyond its inline capacity rather than truncating.
- **Bloom filter.** False positives are safe; a false negative is a wrong style. Insertions and
  removals are paired by the chain synchronization, saturated counters are never decremented, and
  the filter is destroyed with an abandoned pass. Attacker-chosen names that collide in the hash
  only produce false positives, whose cost is the full match the budget already bounds.
- **Matched properties cache.** Keys mix attacker-influenced hashes, so buckets are capped at 8
  entries and the cache at 4,096; full equality is checked on every hit, so a collision can
  never return another element's style. Retained bytes are charged per record.
- **Invalidation.** Under-invalidation is a correctness bug, not a safety bug, but it can show
  stale content. Every uncertain case (`:has()`, key-less attribute selectors, attributes with
  unidentified dependents) falls back to the full restyle.
- **Substitution.** Exponential expansion (each custom property referencing the previous one
  several times) is stopped by the per-value cap and the aggregate component and byte budgets;
  reference cycles are detected before expansion; the reference walk uses an explicit stack, so
  deep reference chains cannot exhaust the native stack.
- **Layers.** Nesting depth, per-stylesheet layer count and document-wide layer count are capped,
  so the 16-bit rank cannot overflow. Imports declared with a layer, dropped today, are fetched
  after Part B; that is new resource-loading exposure, and it uses the existing import loader with
  its limits, so a document can make no more fetches than it could with unlayered imports.
- **Parallel style.** Task count is bounded by the element count, subtrees below the grain run
  inline, and the traversal uses the scheduler's explicit task objects, so a deep or wide hostile
  tree creates no unbounded recursion on worker stacks; budgets are enforced per worker share with
  the serial rerun as the fallback.
- **Incremental reparse.** The window parse runs the same parser with the same limits, and the
  stylesheet's resource stats are recomputed for the spliced result; any limit failure falls back
  to the full reparse, which already enforces them. The differential fuzzer feeds random edits,
  including unbalanced braces, comments and strings.
- **Editor data.** Completion lists built from document ids and classes are capped (256
  suggestions per request) and strings are displayed, never executed.

## Testing and Validation

### Part A Tests

- **Differential cascade test** (`//donner/svg/renderer/tests:style_index_differential_tests`). For
  every SVG in the repository's test corpora (the resvg suite, renderer test data, the splash and
  icon files; the icon sprite, which has no stylesheet, in two halves of its symbols to bound the
  case under ASan), prepare a frame with the index and with the candidate list replaced by every
  rule, then compare every entity's computed style, document and shadow-tree entities alike, plus
  the `collectMatchedStyleRules` lists and the matched-rule and declaration counters; a further case
  edits the stylesheet through a source keystroke and through the DOM after the index was built,
  and small documents built around how the index reads keys cover attribute and type names in
  mixed case in both directions (including `linearGradient`, `clipPath`, `foreignObject` and an
  unknown element), class lists separated by tab, line feed, carriage return and form feed with
  leading, trailing and repeated separators, a `<use>` shadow tree, and an imported sheet. It
  lives with the renderer tests because preparing a frame instantiates the shadow trees. The "every
  rule" candidate source is the test-only `UnindexedCascadeForTesting` context flag; the matcher,
  cascade and inheritance code are shared, so the oracle differs from production only in skipping no
  rule. Once the ancestor filter and the matched properties cache land, the same test runs with each
  on and off. Documents whose unindexed or filter-off pass ends budget-rejected are reported
  separately and must not produce fewer matched rules with the index or the filter. A
  low-traversal-limit case runs the corpus again with
  `StyleResourceBudget::Limits::selectorTraversalSteps` lowered until some documents end
  traversal-rejected with the filter off, and asserts that every document that completes with the
  filter off is identical with it on, and that every rejected one matches, with the filter on, at
  least the rules the filter-off pass matched.
- **Differential fuzzer** (`//donner/svg/components/style:style_cascade_differential_fuzzer`).
  Takes stylesheet text and a small element tree from the fuzzer input and asserts identical
  computed styles, matched-rule lists and counters with the index on and off; the filter and cache
  add their switches when they land. Tags, ids, classes and up to two attributes per element come
  from short tables: mixed-case, namespaced, unknown and foreign tags, class lists with repeated
  tokens and every whitespace separator, and attribute names that differ only in case. Shadow trees
  and imported sheets stay out, because both need the document prepared for rendering; the
  differential test covers them. Run under `--config=asan-fuzzer`.
- **Differential edit test** (`//donner/svg/components/style:style_invalidation_differential_tests`,
  new). For each corpus document and each edit kind (class add, remove and replace; `id`;
  `style`; every presentation attribute the document uses), apply the edit, prepare a frame with
  narrowed invalidation, then compare every computed style with a document that received the same
  edit followed by a forced full restyle. It compares three things, because computed styles alone
  miss render-side under-invalidation: every computed style; the render tree
  (`RenderingInstanceComponent` set and draw order) and every resolved reference (paint servers,
  clip paths, masks, markers, filters, `href` targets); and the rendered TinySkia output, compared
  with `CompareBitmapToBitmap` and `PixelmatchIdentityParams`. Includes `<use>`, gradient stops
  used by shapes elsewhere, patterns, markers, text, `id` changes that re-target references,
  sibling and descendant selectors, and filtered structural selectors (`:nth-child(1 of .selected)`,
  `:nth-last-child(2 of .a, .b)`, and both nested in `:is()` and left of a combinator) where the
  edited element is before, after and equal to the subject. Because the narrow-versus-full output
  comparison passes even if every edit still rebuilds, each edit also asserts the
  `RenderTreeState` counters: an eligible edit (a class edit on an element with no text and no
  paint-server, marker, clip-path, mask or filter reference before or after) increments
  `retainedUpdates` and leaves `fullRebuilds` unchanged, and an ineligible one increments
  `fullRebuilds`. The eligible class edit must be observed red with `FlagsAreRetainable`
  unchanged before it counts.
- **Index unit tests** (`//donner/svg/components/style:rule_index_tests`) for key extraction on
  every selector shape in
  [Donner Today](#donner-today), including namespaces, case, `:is()`, `:not()` and `:has()` in
  the subject compound, and for candidate collection, including that an element gathers each
  bucket once (repeated class tokens, attribute names differing in case, one rule under every key
  kind) and never more positions than the index has entries; a style-system test checks that a
  cascade needing more than 4,096 candidate entries leaves the per-thread list released;
  **filter unit tests** for selector-hash extraction (stopping at sibling
  combinators, ignoring pseudo-class arguments) and for chain synchronization in each traversal
  mode, including a pass abandoned by a budget rejection.
- **Base library.** `BloomFilter` property, saturation, removal and false-positive-count tests in
  `//donner/base:base_tests`, and `//donner/base:bloom_filter_fuzzer`.
- **Existing suites** must stay green unchanged: `//donner/css/...` tests,
  `//donner/svg/components/style:style_system_tests`, the CSS-related renderer tests and
  `//donner/svg/renderer/tests:resvg_test_suite`. The shared computed-style storage relies on
  them.
- **Performance.** `css_cascade_perf_bench` (benchmark binary) and a `donner_perf_cc_test` whose
  correctness target asserts exact counters (elements cascaded, rules tested, full clears, filter
  rejections, cache hits) for fixed documents and edits, so a regression in work done fails the
  PR gate on any machine. Wall clock is recorded only by the `manual`/`perf` wall-clock target;
  correctness tests carry no time limits.
- The invariant "computed style is unchanged" is enforced by the three differential targets
  above; the rule "a full restyle cascades each element once" and "the filter rejects the
  expected candidates" by the counter test; and "a full restyle computes the first frame's
  styles" by `//donner/svg/renderer/tests:full_restyle_corpus_tests`. "A stored computed-style
  record is never modified" is enforced at compile time by the static assertions in
  `ComputedStyleComponent.h`, which every target that reads computed styles compiles, and
  `StyleSystemTest.RestyleStoresNewRecordAndKeepsHeldRecordUnchanged` in
  `//donner/svg/components/style:style_system_tests` checks that a restyle stores a new record and
  leaves a held one unchanged; "each element's local declarations are copied once and inheritance
  copies nothing" by the `RegistryCopies` cases of
  `//donner/benchmarks:css_cascade_perf_tests_correctness`.

### Part B Tests

- **Feature tests** (`//donner/svg/components/style:style_cascade_feature_tests`, new): SVG cases
  authored for Donner that cover the behaviors of these web-platform-tests files, which are HTML
  and are not run directly: in `css/css-variables`, `variable-cycles.html`,
  `variable-exponential-blowup.html`, `long-variable-reference-crash.html`,
  `variable-substitution-shorthands.html`, `variable-css-wide-keywords.html`,
  `variables-substitute-guaranteed-invalid.html`, `var-fallback-braces.html` and
  `variable-presentation-attribute.html`; in `css/css-cascade`, `layer-basic.html`,
  `layer-import.html`, `layer-important.html`, `layer-vs-inline-style.html`,
  `layer-statement-before-import.html`, `layer-rule-invalid-name.html` and
  `presentational-hints-cascade.html` (its HTML `width` attribute becomes an SVG presentation
  attribute). The resvg suite has no custom-property or layer rows. A Donner case adds the layer
  order example from [Cascade Layers](#cascade-layers): `@layer a, b;`, then
  `@import url("b.css") layer(b);` with `b.css` setting `rect { fill: blue }`, then
  `@layer a { rect { fill: red } }`, resolved through a test resource loader; the rectangle is
  blue. The case must be observed red against an imports-first tree before it counts.
- **Shared custom-map release regression** (in `style_cascade_feature_tests`): a parent declares
  a large custom property and a child inherits its map; the parent's record is released while the
  child's record is kept alive through `ExternalUseStyleRetention`, and the `RetainedStyleBytes`
  total must still include the map and its token sequences; then the child is released and the
  total returns to its starting value. A second case shares one token sequence between two
  independently overlaid maps and releases them in both orders. Both must be observed red with the
  map charged to its first record before they count.
- **Rendering cases** in the Donner SVG 2 suite for a custom property feeding `fill` through a
  `<use>` instance, a layered `!important` override, and a presentation attribute losing to the
  first declared layer.
- **Parser tests and fuzzers**: `stylesheet_parser_fuzzer` and `declaration_list_parser_fuzzer`
  gain `@layer` and `var()` dictionaries; the differential cascade fuzzer generates custom
  properties and layers.
- **Unchanged documents**: the Part A differential test also compares the corpus before and after
  each Part B milestone, which must be identical for documents without `--*`, `var()` or `@layer`.

### Part C Tests

- **Incremental reparse fuzzer** (`//donner/svg/components:stylesheet_edit_differential_fuzzer`,
  new): a stylesheet and a sequence of random edits; after each edit, the incrementally updated
  `css::Stylesheet` must equal a fresh parse of the new text, rule ranges included. The edit
  generator is biased toward the resynchronization cases: text typed between rules and at the
  sheet end, deleted and inserted braces, a stray top-level `}`, unterminated comments, strings
  and `url(`, trailing backslashes, entity references in the source (which must take the full
  reparse path), and edits that change cross-rule parser state: deleting or inserting the last
  rule before an `@import`, `@namespace` or `@layer` statement, and edits that move a sheet across
  a parser limit.
- **Stylesheet edit cases** in the differential edit test: the four measured keystrokes, a rule
  insertion and deletion, and an edit that changes a selector so the rule moves between elements.
  An exhaustion case replaces several rules in a document whose limits are lowered so that a
  fresh cascade completes but the old-and-new scan is refused; it asserts that a full restyle was
  requested and that every computed style equals the fresh cascade's. It must be observed red
  with the scan's partial result kept before it counts.
- **Editor tests** with headless `TextEditorCore` and the autocomplete functions: classification
  of every CSS byte in the corpus' `<style>` elements and `style` attributes, and one test per
  completion context returning suggestions from its source.

### Part D Tests

- The differential cascade test and fuzzer at worker counts 0, 1, 2, 4 and 8 and under the
  seeded-schedule mode, over the whole corpus.
- `//donner/svg/components/style:style_parallel_tests` (new): the serial fallback on budget
  pressure, warning order, offscreen shadow roots, and the injected-delay red/green pair; run in the
  ThreadSanitizer lane.
- Counter assertions in the perf test's correctness target: the splash and tiger passes take the
  serial path, the sprite pass takes the parallel path.

## Alternatives Considered

- **Port the differential engine approach (Ladybird #11088).** Retaining match results and
  computed styles as materialized views and routing typed deltas gives the most general
  invalidation, but it is a rewrite of the style system. The narrowed invalidation and cache here
  reach the measured cases with the existing selective pass. The reference-evaluator idea is kept
  as the differential tests.
- **Hash maps instead of sorted vectors for the index.** Constant-time lookups, but hashing
  attacker-chosen keys needs a seeded hash to resist flooding, and the maps are larger and
  scattered. Element lookups are few (id, classes, tag, attributes), so binary search costs
  little.
- **Index per document instead of per stylesheet.** Fewer lookups per element with several
  `<style>` elements, but every stylesheet edit would rebuild a merged structure and source order
  across sheets would have to be encoded. Documents rarely have more than a few sheets.
- **Pre-parse declaration values once per rule.** Today each matched declaration is parsed into
  typed values per element. The matched properties cache removes that parse on every hit, which
  is 66 to 99.8 percent of elements on the measured documents; pre-parsing would only help the
  misses.
- **A filter rebuilt per element instead of a counting filter.** Rebuilding a plain filter from
  the ancestor chain for every element needs no removal, but costs the whole chain per element;
  the counting filter with chain synchronization costs only the difference between consecutive
  chains.
- **Tarjan's algorithm for custom property cycles (Stylo).** It finds cycles across all of an
  element's custom properties in one pass; the depth-first walk with in-progress marks (Blink)
  does the same for the few properties an element declares, with less code.
- **Level-synchronous parallelism (all elements of one depth at once).** Simple, but each level
  needs a barrier and SVG trees are shallow and wide in places and deep in others; subtree tasks
  with a grain adapt to both.
- **Shared, locked budgets and caches.** Exact budget accounting across workers needs a lock or
  atomics on every charge and makes the exhaustion point depend on scheduling; per-worker shares
  with a serial rerun keep the outcome deterministic.
- **A general incremental parser (tree-sitter or Lezer style).** Reusing arbitrary subtrees
  needs an error-tolerant grammar of its own; Donner's rules are flat and carry source ranges, so
  a rule window gives the same reuse with the existing parser and its limits.

## Decisions

- **Order of work.** The rule index lands before narrowed invalidation, as in the
  [Implementation Plan](#implementation-plan). The single-cascade fix already halves the cost of
  every edit, and the index shrinks what each remaining full restyle costs.
- **Budget semantics.** The rule-element budget is charged per candidate tested. Documents that
  exhaust the budget today only because of rules that cannot match style completely afterwards,
  under unchanged limits. The ancestor filter extends the same change to the selector-traversal
  budget: walks it skips are not charged, so a document that ends traversal-rejected today can
  apply more rules. This is the one intended behavior change of Part A, limited to documents that
  end budget-rejected today.
- **Test-only unindexed candidate source.** The differential tests use a test-only option on
  `StyleSystem` that offers every rule as a candidate. It is a switch on the shared matcher and
  cascade path, not a second implementation; the filter and cache have the same kind of switch.
- **Selector-list specificity.** `MatchSelectorRule` applies the first matching entry's
  specificity, where
  [Selectors Level 4](https://www.w3.org/TR/selectors-4/#specificity-rules) uses the most specific
  matching selector. That is tracked as
  [#2663](https://github.com/jwmcglynn/donner/issues/2663) and fixed separately; the index
  preserves whichever behavior the matcher has.
- **Shared computed-style storage and the matched properties cache** are in scope (Milestones A8
  and A9).
- **Ancestor Bloom filter** is built regardless of its measured payoff, on a reusable
  `donner/base/` helper; its effect is reported by counter.
- **Custom properties and cascade layers** are in scope (Part B).
- **Incremental stylesheet reparse, CSS highlighting and completion** are in scope (Part C).
- **Parallel style traversal** is in scope (Part D) on a base-library work-stealing scheduler with
  `co_await` support, designed as its own document,
  [0073-task_scheduler](0073-task_scheduler.md), because it is a general facility with its own
  goals, risks and tests and other clients.
- **Storage route for parallel style.** No structural registry change inside the parallel region,
  with structural work done before it and results written into pre-existing slots and per-worker
  buffers. An evaluation of making EnTT's storage safe for parallel structural changes found it
  possible only by forking EnTT, 4 to 8 times slower per lookup, and order-nondeterministic, so
  that route is rejected (see [What Must Hold](#what-must-hold-for-parallel-cascading)).
- **Document scale.** Documents above the XML parser's default 8,192-node cap are not a target of
  these goals. The techniques scale with elements and rules, so larger documents benefit, but no
  goal is stated or measured for them.

## Open Questions

Each question is labelled with its part, so a part can be lifted into its own document with its
questions.

- **Part A: retained-byte accounting for shared records.** Charging the computed-style budget per
  distinct record instead of per entity lets documents that exceed the 64 MiB retained-property
  limit today only through duplication style completely. Recommendation: charge per record, the
  same reasoning as the per-candidate decision.
- **Part B: `var()` in presentation attributes.** The SVG Working Group resolved to allow it but has
  not specified the parse model; Firefox and Safari support it, Chromium partially. Recommendation:
  support it as a pending substitution at presentation-attribute priority, matching
  `variable-presentation-attribute.html`, and revisit if the specified parse model differs.
- **Part C: invalid text while typing.** The engine renders what the current text parses to, so
  typing through an unbalanced brace changes the rendering until it is closed. Recommendation: keep
  the engine exact (it equals export and reload) and, if the flicker bothers users, add an
  editor-side debounce that defers applying a stylesheet edit while the source has an unclosed
  block.
- **Part B: `revert` and `revert-layer`.** Unsupported today (dropped as invalid). Supporting them
  needs every candidate declaration per property kept through the cascade instead of one winning
  slot. Recommendation: keep them out of Part B and take them up with `@property` in a later design,
  since both change how the cascade stores candidates.
- **Part D: parallel threshold.** The initial 1,000-element threshold keeps most single
  illustrations serial. Recommendation: keep it until Milestone D3 measures the curve, and do not
  lower it to make small documents parallel unless the measured wake cost allows.

## Future Work

- [ ] Narrow invalidation for element insertion and removal using the sibling and structural
      dependency bits.
- [ ] Stop a descendant recompute where the recomputed style equals the previous one, so an edit
      that changes no inherited value does not walk the subtree.
- [ ] Mine `:is()` and `:where()` arguments for index keys.
- [ ] Per-worker structural command buffers applied at a sync point in an order derived from the
      tree, as a general facility for parallelizing phases that create entities (shadow-tree
      instantiation, shapes); not needed for style.
- [ ] `@property` registration, as a separate design.
