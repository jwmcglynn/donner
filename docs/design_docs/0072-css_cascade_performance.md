# Design: CSS Cascade Performance, Features and Live Editing

**Status:** Draft
**Author:** Claude Fable 5.2
**Drafted by:** Claude Opus 5.5
**Requested by:** Jeff McGlynn
**Created:** 2026-11-23
**Tracking:** [#2660](https://github.com/jwmcglynn/donner/issues/2660)

## Summary

Donner resolves style by testing every rule of every applicable stylesheet against every element,
then re-parsing each matched declaration into the element's `PropertyRegistry`. Any edit to a
class, `id`, `style` attribute, presentation attribute or stylesheet restyles the whole document,
and the frame that follows runs the whole-document cascade twice. Each element keeps its own
4,840-byte computed `PropertyRegistry`, even when hundreds of elements compute the same style.
Custom properties (`var()`) and cascade layers (`@layer`) are not supported.
Typing in a `<style>` element reparses the whole stylesheet and restyles the whole document, and
the source editor has no CSS-aware highlighting or value completion.

This design measures that cost, surveys how browser engines make style resolution fast and how
they implement the missing features, and commits to
three parts:

**Part A: Performance.**

1. Cascade each element once per full restyle (today a full restyle cascades every element twice).
2. A per-stylesheet rule index keyed on the rightmost compound selector, so an element tests only
   the rules that can match it.
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

Computed style stays bit-identical for documents that use only today's features. On the measured
documents style is 47 to 89 percent of preparing the first frame without rasterization, and 80 to
95 percent of preparing the frame after a one-element edit.

## Goals

All numbers are against the baseline in [Measured Baseline](#measured-baseline), measured with the
same harness, machine class and method. Each goal names the counter or timer that decides it.

### Part A: Performance

- **One cascade per element per full restyle.** A frame that follows a full restyle cascades each
  styled entity once. Today the splash document cascades 1,069 elements after a one-element class
  edit and the bootstrap-icons sprite cascades 10,274, twice its 5,137 entities.
- **Rules tested per element.** On `donner_splash.svg` (89 author rules, 4 user agent rules),
  rules tested per first frame fall from 57,838 to at most 5,000 (from 93 per element to at most
  8). On the synthetic 2,000-element, 200-rule document they fall from 408,608 to at most 20,000.
- **First-frame style time.** On `donner_splash.svg` style time falls from a 10.8 ms median to at
  most 5 ms. On the synthetic 2,000-element, 200-rule document it falls from 77.6 ms to at most
  15 ms. Style time on the rule-free synthetic and bootstrap-icons documents does not regress by
  more than 5 percent before the matched properties cache lands, and on bootstrap-icons falls from
  31.5 ms to at most 15 ms after it.
- **Edit cost.** A class, `style` or presentation-attribute write on one leaf element whose new
  and old values no selector depends on in a combinator position cascades at most the element, its
  descendants and its `<use>` instances. On the splash document the style time of the next frame
  falls from 18.8 ms to at most 1 ms, and on the bootstrap-icons sprite from 63 ms to at most 1 ms.
- **Ancestor filter.** On the synthetic descendant-only document (1,000 elements, 100 rules), the
  new counter of candidates rejected by the filter reaches at least 857 of the 902 candidates that
  today fail after an ancestor walk (95 percent), with no false negative (enforced by the
  differential tests), and style time on the real documents does not regress by more than 2
  percent. On the real documents only one candidate per document fails in an ancestor walk, so no
  time goal is set there (see [Ancestor Filter Opportunity](#ancestor-filter-opportunity)).
- **Shared computed styles.** With the matched properties cache, the share of elements whose
  computed style is reused reaches at least 60 percent on the splash document (opportunity
  measured at 66 percent) and at least 99 percent on bootstrap-icons (measured at 99.8 percent).
  Retained computed-style records on bootstrap-icons fall from 5,137 (24.9 MB of inline
  `PropertyRegistry` storage) to at most 50.
- **Budget headroom.** The rule-element match budget charges rules actually tested, so the
  synthetic 4,000-element, 1,000-rule document, which exhausts the 1,048,576-match budget today,
  styles completely under unchanged limits.
- **No behavior change.** For every document whose cascade completes within its budgets today,
  every computed style is identical before and after each Part A step, enforced by differential
  tests and fuzzers (see [Testing and Validation](#testing-and-validation)).
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
- **No cost when unused.** On documents with no custom properties and no layers, style time does
  not regress by more than 2 percent and computed styles are unchanged.

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

## Non-Goals

- No change to computed style, cascade order or selector semantics for documents that use only
  features supported today. Known deviations found while doing this work are reported separately
  (see [Decisions](#decisions)).
- No selector JIT or code generation.
- No parallel style traversal. Style resolution stays single-threaded inside the document lock.
- No `@property` registration. Registered custom properties add typed values, computed-value
  resolution of lengths and their font-size dependency cycles, and non-inherited custom
  properties; untyped custom properties cover `var()` without them. A later design can add it.
- No `env()`, `attr()` or other substitution functions, and no `@media`, `@supports`,
  `@container` or `@scope` (unsupported today and unchanged).
- No SMIL animation of custom properties.
- No change to the public API. The index, filter, cache, counters and invalidation summaries are
  internal; public documentation keeps describing documents, elements and styles.
- No full document reparse, and no new trigger for one.
- Narrowing invalidation for element insertion or removal; listed under
  [Future Work](#future-work).
- In the editor, no visual design beyond the classification and completion data, and no CSS
  formatting, linting, hover documentation, color pickers or browser-compatibility data.
- Raising the XML parser's default 8,192-node cap, which bounds the element count of documents
  parsed with default options.

## Next Steps

- Land the cascade counters and the benchmark (Milestone A1), so every later step reports its
  effect with the same instrument.
- Land the single-cascade fix (Milestone A2): the smallest change with the largest measured effect
  on edits.
- Land the base-library Bloom filter (Milestone A6), which has no dependency, in parallel.

## Implementation Plan

Each milestone is one independently reviewable PR unless noted. Estimates are engineering days,
including tests and review fixes. "After" names the milestones that must land first.

### Part A Milestones

- [ ] Milestone A1: Cascade counters and benchmark (1.5 days)
  - [ ] Add internal per-pass counters to `StyleSystem` (elements cascaded, rules tested, rules
        matched, declarations applied, full clears, budget rejections, and later the filter and
        cache counters), readable through the existing `RenderingDiagnostics` snapshot.
  - [ ] Add `//donner/benchmarks:css_cascade_perf_bench` with the documents and edits in
        [Reproducing the Baseline](#reproducing-the-baseline).
  - [ ] Add a `donner_perf_cc_test` whose correctness target asserts the counter values for fixed
        documents (CPU-invariant, PR-gated) and whose wall-clock target records times (manual,
        `perf` tag).
  - Done when: the counters reproduce this document's counter columns exactly for the splash,
    bootstrap-icons and synthetic documents.
- [ ] Milestone A2: One cascade per full restyle (1 day). After A1.
  - [ ] Clear computed styles once per rebuild in `RenderingContext::createComputedComponents`,
        and make its second `StyleSystem::computeAllStyles` call compute only entities without a
        computed style (the shadow-tree clones the first call could not see).
  - [ ] Establish with a test, before the change, whether `ApplyAnimationPresentation` depends on
        the second pass discarding its overrides; keep animated documents on their current path if
        it does.
  - Done when: the correctness counter test shows elements cascaded equal to the styled entity
    count on the edit frames, and the next-frame style time after a splash class edit is at most
    10 ms (from 18.8 ms).
- [ ] Milestone A3: Rule index (4 days). After A1.
  - [ ] Build `RuleIndex` for a stylesheet's own rules and for each imported sheet when
        `StylesheetComponent::rebuildSelectorCaches` runs.
  - [ ] Collect candidates per element in `StyleSystem::applyStylesheetRules` and
        `collectMatchedStyleRules`; charge the budget per candidate.
  - [ ] Add the differential test and the differential fuzzer.
  - Done when: rules tested and style time meet the load goals, the differential test and fuzzer
    are green, and the Wasm size check passes with its current ceiling.
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
- [ ] Milestone A5: Narrowed invalidation for single-element edits (5.5 days). After A2.
  - [ ] Compute a `SelectorDependencySummary` per stylesheet at parse time, including the
        directional dependencies of filtered `:nth-child` and `:nth-last-child` selectors.
  - [ ] Route class, `id`, `style` and presentation-attribute writes in `SVGElement` through it,
        marking the affected entities dirty for the existing selective style pass instead of
        setting `RenderTreeState::needsFullStyleRecompute`.
  - [ ] Restyle the `<use>` shadow instances of a written element.
  - [ ] Add the narrow-versus-full differential edit test.
  - Done when: the edit goals are met and the differential edit test is green.
- [ ] Milestone A6: Base-library Bloom filter (2 days). No dependency.
  - [ ] Add `donner/base/BloomFilter.h` with `BloomFilter`, `CountingBloomFilter` and `BloomHash`,
        unit tests in `//donner/base:base_tests` and `//donner/base:bloom_filter_fuzzer`.
  - Done when: the no-false-negative property test, the counting-removal and saturation tests,
    the deterministic false-positive-rate test and the fuzzer are green.
- [ ] Milestone A7: Ancestor filter in the cascade (3 days). After A3 and A6.
  - [ ] Store up to four ancestor hashes per selector-list entry in `RuleIndex`.
  - [ ] Maintain the filter with `StyleAncestorFilter` in every traversal mode and check it
        before `MatchSelectorRule`; count filter rejections.
  - [ ] Run the differential test and fuzzer with the filter on and off.
  - Done when: the ancestor-filter goal is met and the differential targets are green.
- [ ] Milestone A8: Per-element cost and shared computed-style storage (4 days). After A1.
  - [ ] Break down the measured 4.6 microseconds of per-element style work outside rule matching
        with timers, and remove avoidable whole-`PropertyRegistry` copies in
        `computePropertiesInto`, `inheritComputedParent` and `inheritProperties`.
  - [ ] Store computed styles as `std::shared_ptr<const ComputedStyle>` records and move every
        reader to the const accessor.
  - Done when: style time on the rule-free 1,000-element synthetic document falls from 6.1 ms by
    at least 20 percent, or the breakdown shows the cost is inherent and the result is recorded
    here; all existing suites are green on the shared storage.
- [ ] Milestone A9: Matched properties cache (3 days). After A3 and A8.
  - [ ] Key, look up and insert shared records in `computePropertiesInto`; count hits and
        uncacheable elements; charge retained bytes per record.
  - [ ] Run the differential test and fuzzer with the cache on and off.
  - Done when: the shared-computed-style goals and the bootstrap-icons style-time goal are met and
    the differential targets are green.

Part A estimate: 26 engineering days.

### Part B Milestones

- [ ] Milestone B1: Custom property declarations and storage (3 days). After A8.
  - [ ] Route `--*` declarations from rules and `style` attributes into a per-element custom
        cascade instead of the "Unknown property" diagnostic, and store computed custom
        properties as shared immutable maps in the computed-style record.
  - Done when: custom properties cascade and inherit in unit tests, and computed styles of the
    existing corpus are unchanged.
- [ ] Milestone B2: `var()` substitution (4 days). After B1.
  - [ ] Mark declarations containing `var()` at parse time, hold them as pending substitutions in
        the cascade, substitute after the custom properties are computed, and parse the result;
        detect cycles and enforce the size caps.
  - Done when: the ported cycle, fallback, keyword, invalid-at-computed-value-time and
    exponential-expansion cases pass.
- [ ] Milestone B3: `var()` in presentation attributes and shorthands (2 days). After B2.
  - Done when: the ported presentation-attribute and shorthand cases pass.
- [ ] Milestone B4: `@layer` and `@import ... layer` parsing (3 days). No dependency.
  - [ ] Parse layer statements and blocks (nested, dotted, anonymous) and assign each retained
        rule a layer; compute the document's layer order.
  - Done when: parser tests and `stylesheet_parser_fuzzer` cover every form, and rules inside
    `@layer` blocks are retained instead of dropped.
- [ ] Milestone B5: Layer order in the cascade (2 days). After B4.
  - [ ] Add the layer rank to `css::Specificity` with importance reversal, place presentation
        attributes below every author layer, and check `Property::inheritFrom` with layered
        declarations.
  - Done when: the ported layer cases pass and computed styles of documents without layers are
    unchanged.
- [ ] Milestone B6: Conformance cases (2 days). After B3 and B5.
  - Done when: the full set in [Part B tests](#part-b-tests) is committed and green.

Part B estimate: 16 engineering days.

### Part C Milestones

- [ ] Milestone C1: Rule-level incremental stylesheet reparse (4 days). No dependency.
  - [ ] Add `StylesheetComponent::applyTextEdit` that reparses the window of rules an edit
        touches and splices the result, falling back to a full stylesheet reparse where the
        window does not resynchronize.
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

Part C estimate: 12 engineering days.

### Order and Total

A1 first; A2, A3, A6, A8, B4, C1 and C3 can then proceed in parallel; A4 follows A3; A7 follows
A3 and A6; A5 follows A2; A9 follows A3 and A8; B1 to B3 follow A8; B5 follows B4; B6 closes Part B;
C2 follows C1, A3 and A5; C4 follows C3 and, for `var()` names, B2. Total committed estimate: 54
engineering days (A 26, B 16, C 12).

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
imported sheet, then the sheet's own rules, and calls `MatchSelectorRule` for every rule. There
is no index. `MatchSelectorRule` returns the first selector-list entry that matches, with that
entry's specificity. A match is charged to `StyleResourceBudget::reserveRuleElementMatch` before
the selector runs, so the budget counts rules tested, not rules matched.

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
`@layer` statements only for import ordering (`AtRuleEndsImportSection` in
`StylesheetParser.cc`) and drops `@layer` blocks with their rules, and it drops imports that name
a layer ("CSS @import cascade layers are unsupported"). `css::Declaration` already stores its
value as a `std::vector<ComponentValue>`, which is the token sequence custom properties need.

**Computed style.** After the cascade, `ApplyAnimatedStyle`, contextual font sizes, scalar and
paint animation styles and symbol display are applied, then `inheritComputedParent` merges the
parent's computed `PropertyRegistry` (computing the parent first if needed). The result is stored
by value in `ComputedStyleComponent::properties`, a `std::optional<PropertyRegistry>`, and
memoized: `computePropertiesInto` returns early when it is set. Fonts are resolved to absolute
values against the parent. `sizeof(PropertyRegistry)` is 4,840 bytes in the measured build, so
every styled entity holds about 4.7 KiB of computed style inline in its component, plus heap
storage for strings and lists.

**Computed-style readers.** Outside `donner/svg/components/style/`, 32 production files reference
`ComputedStyleComponent`: the renderer (`RendererDriver`, `RenderingContext`,
`ExternalUseStyleRetention`, `RetainedRenderTreeUpdate`, `AnimationPresentation`), the compositor,
text (`TextSystem`, `TextEngine`), layout, shape, paint, filter, shadow-tree and animation
systems, `SVGElement`, `RenderingDiagnostics` and one editor header. Those that access the
component read `properties` (through `.value()`, `*` or `->`). The only writers are
`computePropertiesInto` (including its budget-refusal branch),
`ShadowTreeSystem::createShadowEntity`, which emplaces an empty component, and
`ExternalUseStyleContext`, which holds a copy of the host's computed style as its `parent`.
Whole-component clears happen in the full pass of `computeAllStyles`, `MarkStylesheetChanged`,
`MarkNeedsFullRebuild`, `StyleSystem::discardComputedStyles` and `RecomputeAnimationStyles`.

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
clears every `ComputedStyleComponent` and recomputes the document, and runs whenever
`RenderTreeState::needsFullStyleRecompute` is set. That flag is set by `SVGElement::setClassName`,
`setId`, `setStyle` and `updateStyle`, by presentation-attribute writes other than `transform` and
`opacity` (`MarkPresentationAttributeDirty`), by stylesheet changes (`MarkStylesheetChanged`), and
by child insertion and removal (`MarkChildInserted`, `MarkChildRemoved`). `transform` and `opacity`
writes request it only when a stylesheet selects on the attribute
(`StyleSystem::anyStylesheetUsesAttributeInSelector`). A full restyle also takes the full rebuild
path in `RenderingContext::ensureComputedComponents`, which tears down and re-instantiates shadow
trees, because `tryRetainedUpdate` declines when the flag is set.

`RenderingContext::createComputedComponents` calls `StyleSystem::computeAllStyles` twice: before
and after shadow-tree instantiation. `needsFullStyleRecompute` is cleared only after both return,
so on a full-restyle frame both calls take the full pass and the second discards and recomputes
everything the first produced. On the first frame of a freshly parsed document `hasBeenBuilt` is
false, so the second call only fills in the new shadow entities. Animated documents add a third
whole-document cascade: `ApplyAnimationPresentation` calls `RecomputeAnimationStyles`, which
clears and recomputes every style with animation enabled.

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

Machine: an Arm Neoverse-N1 Linux machine, optimized build (`-c opt`), the benchmark process
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
| Splash, append one rule            |             1,069 |       99,401 | 24.07 (23.56-26.48)    | 19.38 (18.88-21.03)    |  81 percent | 146.84 (144.48-154.04) |
| Synthetic 2,000 x 200, `class`     |             4,006 |      817,216 | 167.32 (163.37-170.15) | 158.22 (155.10-161.47) |  95 percent | 173.48 (170.44-178.92) |
| Synthetic 2,000 x 200, `fill`      |             4,006 |      817,216 | 166.62 (164.46-175.59) | 158.27 (156.14-166.41) |  95 percent | 170.72 (168.77-176.32) |
| Synthetic 2,000 x 200, `style`     |             4,006 |      817,216 | 168.34 (164.78-170.10) | 159.26 (156.43-160.73) |  95 percent | 173.93 (170.43-180.98) |
| Synthetic 2,000 x 200, append rule |             4,006 |      817,216 | 165.24 (163.46-169.22) | 156.81 (155.13-160.67) |  95 percent | 171.74 (169.67-175.51) |
| Bootstrap icons, `class`           |            10,274 |       41,088 | 70.07 (68.58-72.81)    | 63.14 (62.05-65.25)    |  90 percent | 70.43 (69.00-73.58)    |
| Bootstrap icons, `fill`            |            10,274 |       41,088 | 70.97 (69.42-72.98)    | 63.98 (62.29-65.41)    |  90 percent | 70.83 (68.95-74.23)    |
| Bootstrap icons, `style`           |            10,274 |       41,088 | 70.50 (69.00-72.98)    | 63.41 (62.08-65.66)    |  90 percent | 69.87 (67.83-72.19)    |

Every edit, including a `fill` write on one shape, restyles the whole document twice (2 full
clears per frame in every row). The editor presents through its compositor and Geode rather than
a full TinySkia frame, so its edit-to-frame cost lies between the two frame columns; style is 80
to 95 percent of the frame preparation that precedes rasterization in every case measured.

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
| 1,000 x 1,000    | `rect:nth-child(N)` only |    1,007,008 |   169.85 |   163.51 |
| 1,000 x 1,000    | `[data-kN]` only         |    1,007,008 |   163.54 |   157.72 |

"Mixed" is 70 percent class, 10 percent type, 15 percent descendant and 5 percent attribute
selectors. Findings:

- Matching cost is linear in elements times rules and nearly independent of selector kind: 155 to
  173 ns per rule tested, almost all of them rejections (0.8 percent of tests match on the mixed
  document, 2.4 percent on the splash). A rejection on the class of a one-class element costs as
  much as a descendant selector, so the per-test overhead, not the combinator walk, dominates.
- Outside matching, each element costs about 4.6 microseconds (style minus match, divided by
  elements, on the rule-free documents and the sprite).
- At 4,000 elements and 1,000 rules the 1,048,576 rule-element budget is exhausted and later
  elements are left with partial styles. Any document with more than about one million
  element-rule pairs is affected today, whatever it actually matches.

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
1,384 candidates on the splash document against 57,838 rules tested today.

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
differ only in a `fill` attribute. At 4,840 bytes per inline `PropertyRegistry`, bootstrap-icons
retains 24.9 MB of computed style for 8 distinct values.

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
reparsed the whole stylesheet (inside the source edit time) and restyled the whole document twice
(1,069 element cascades, 2 full clears, as for the attribute edits above). Deleting a rule's
closing brace changes what the stylesheet parse keeps: the two alternating states differ by a
factor of three in style time (6.3 to 18.8 ms), which is what a reader typing through an
unbalanced brace sees today.

### Reproducing the Baseline

The measurements used a local harness that Milestone A1 commits. Until then:

1. Add process-global counters and timers to `StyleSystem.cc`: increment counters in
   `applyStylesheetRules` (per rule tested and matched), `ApplyRuleDeclarations` (per
   declaration), `computePropertiesInto` (per element), and the full and selective branches of
   `computeAllStyles`; accumulate `std::chrono::steady_clock` time over the outermost
   `computeAllStyles` and `computeStylesFor` calls; optionally time `applyStylesheetRules` per
   element; record `StyleResourceBudget::rejected()` when `computeAllStyles` returns.
2. For the opportunity counters, also: count combinator steps in `ComplexSelector::matchChain`
   (one increment where it pushes a frame); in `applyStylesheetRules`, decide per rule whether it
   would be an index candidate (the key choice of [Rule Index](#rule-index)) and record whether
   the match took a combinator step, failed, and how many `SelectorTraversalBudget` steps it
   used; capture the matched rules right after `applyStylesheetRules` and, at the end of
   `computePropertiesInto`, group the element by the key in
   [Shared Computed Style Opportunity](#shared-computed-style-opportunity) and compare its final
   properties with the group's. These counters copy registries and must be off for timing runs.
3. Build a binary depending on `//donner/svg`, `//donner/svg/renderer`,
   `//donner/svg/renderer:renderer_driver` and `//donner/svg/components/style:style_system` with
   `bazel build -c opt`. For each document: parse with `SVGParser::ParseSVG`, reset the counters,
   time `RendererDriver::draw` with a renderer whose methods do nothing, then time
   `Renderer::draw` (TinySkia) on a second fresh parse. For edits: parse and draw once, then
   alternate between two values of the attribute on one element and time the next draw. For
   keystrokes: parse and draw once, then alternate two one-character `XMLEditIntent`s through
   `SVGDocument::applySourceEdit` and time the call and the next draw.
4. Documents: `donner_splash.svg`, `third_party/bootstrap-icons/bootstrap-icons.svg`,
   `donner/svg/renderer/testdata/Ghostscript_Tiger.svg`, and generated documents: groups
   `<g class="gK">` of ten `<rect class="cN">` with no whitespace text nodes (the default
   8,192-node parser cap applies), rules `.cN`, `.gK .cN`, `rect:nth-child(N)` and `[data-kN]`;
   and a `<symbol>` of 20 `<rect class="cN">` used by 200 `<use>` elements.
5. Edit targets: the first `.cls-71` path in the splash (class toggled to `cls-88`), element
   `#e1000` of the synthetic document (class `c3` and `c7`), the first `symbol > path` of the
   sprite (classes `a` and `b`). Keystroke targets in the splash: the last hex digit of `#fffffc`
   in the `.cls-2` rule, a letter after the first `.cls-2` selector, that rule's closing brace,
   and a letter after the first `class="cls-71`.
6. Pin the process to one core (`taskset`) and report medians with minimum and maximum.

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
ancestors when any hash is absent. WebKit's `SelectorFilter` uses a 4,096-slot counting filter,
salts hashes by kind, keeps up to 4 per selector, and quotes about 0.2 percent false positives at
100 strings
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
Stylo removes dependency cycles during substitution with a variant of Tarjan's
strongly-connected-components algorithm, invalidating every member of a cycle, and caps a value
at 2 MiB (`MAX_VALUE_LENGTH_IN_BYTES`, with a comment that values can otherwise grow
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

| Rank | Technique                                      | Decision        | Reason                                                                                                                                                                                                                        |
| ---: | ---------------------------------------------- | --------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
|    1 | Cascade once per full restyle                  | Adopt           | Halves the style time of every measured edit; a few lines in one function.                                                                                                                                                    |
|    2 | Rule index on the rightmost compound           | Adopt           | On the stylesheet-heavy documents 97.6 to 99.2 percent of rule tests reject; the simulated index leaves 1,384 of 57,838 tests on the splash document.                                                                         |
|    3 | Narrowed invalidation for single-element edits | Adopt           | Turns an edit from two whole-document cascades into a subtree cascade. Reuses the existing selective pass and dirty flags.                                                                                                    |
|    4 | Matched properties cache and shared records    | Adopt           | 66 to 99.8 percent of elements reuse an earlier element's computed style on the measured documents, and shared records remove up to 24.9 MB of duplicated computed style. Needs the storage change of Milestone A8.           |
|    5 | Matcher constant factors                       | Adopt, measured | 155 ns per rejected test points at per-attempt overhead; each fix is local and independently testable.                                                                                                                        |
|    6 | Per-element base cost (copies)                 | Adopt, measured | 4.6 microseconds per element dominates rule-light documents; the breakdown comes first, and the copy removal ships with the storage change of Milestone A8.                                                                   |
|    7 | Ancestor Bloom filter                          | Adopt           | Requested regardless of payoff. After indexing it removes one failing walk per real document and 47 percent of candidates on descendant-heavy stylesheets; its effect is measured by a rejection counter, not by time.        |
|    8 | Sibling and cousin style sharing               | Reject          | Correctness depends on a complete revalidation set over Donner's full selector support, including shadow-tree adapters; the matched properties cache captures the same reuse with a simpler key.                              |
|    9 | Rule tree                                      | Reject          | Its benefit, restyle without re-matching, is delivered by narrowed invalidation and the matched properties cache with far less machinery.                                                                                     |
|   10 | Selector JIT                                   | Reject          | Large complexity, no executable memory on WebAssembly, and a 2x ceiling on a cost the index already removes.                                                                                                                  |
|   11 | Parallel traversal                             | Reject          | Style runs under the single-owner document lock; most documents are smaller than a work-stealing unit.                                                                                                                        |
|   12 | Custom properties, with shared maps            | Adopt           | Part B, by operator decision: `var()` is a missing feature, not a speedup. An element that declares no custom property shares its parent's map, as Ladybird and Stylo do, so documents without custom properties pay nothing. |
|   13 | Cascade layers, ordered inside the priority    | Adopt           | Part B, by operator decision. The layer rank joins `css::Specificity`, as Blink and Stylo pack it into their priority, so the source-order cascade needs no sort.                                                             |
|   14 | Range-based incremental stylesheet edits       | Adopt           | Part C: a keystroke costs a 19.2 ms whole-document restyle today; a rule-window reparse plus a restyle of the matched elements removes it.                                                                                    |

## Proposed Architecture, Part A: Performance

### Single Cascade per Full Restyle

`RenderingContext::createComputedComponents` performs the full-restyle clear itself, once, before
the first `computeAllStyles` call, and both calls then compute only entities without a computed
style. Concretely, `StyleSystem::computeAllStyles` gains an internal mode parameter (full clear or
fill missing), used only by `createComputedComponents`; other callers keep today's behavior. The
second call already only needs the shadow-tree clones that `InstantiateMainShadowTrees` and
`PaintSystem::createShadowTrees` created after the first call.

Risk: `ApplyAnimationPresentation` runs between the two calls and, for animated documents,
recomputes every style through `RecomputeAnimationStyles`. Documents with
`AnimatedValuesComponent` are forced onto the full restyle in `ensureComputedComponents`.
Milestone A2 first adds a test that pins what an animated document's computed style is after an
edit frame, then changes the pass structure; if animation correctness depends on the second clear,
animated documents keep the current path and the change applies to the rest.

### Rule Index

**Data structure.** `RuleIndex` lives next to `StylesheetComponent` in
`donner/svg/components/style/`, one per `css::Stylesheet` (own sheet and each imported sheet). It
is built by `rebuildSelectorCaches` and rebuilt whenever the sheet is reparsed or its imports are
replaced. It holds:

- Four sorted flat vectors of `(key, firstEntry, count)` for id, class, lowercased type name and
  lowercased attribute local name, pointing into one `entries` vector.
- One `universal` span of entries.
- Each entry is `{uint32_t ruleIndex}` (4 bytes); the per-key arrays add 8 bytes per distinct key
  besides the key itself, which is the `RcString` already held by the selector (shared, not
  copied).

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
class (split once with `StringUtils::SplitAsciiWhitespace`, the same split `ClassSelector` uses),
the lowercased tag name, one per attribute local name, and `universal`. All lookups go through
`ShadowedElementAdapter` (`id()`, `className()`, `tagName()`, attribute enumeration), so shadow
entities use exactly the structure-from-tree, data-from-light-element rule they use today.
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

**Memory accounting.** Index bytes (vector capacities plus per-key records) are computed when it
is built and added to the stylesheet's charge in the document's `ParsedPayloadResourceBudget`
(`Category::Stylesheet`) alongside the parsed stylesheet bytes. The index never exceeds 16 bytes
per selector-list entry plus vector slack, and selector-list entries are already bounded by the
parser's per-stylesheet rule and component-value limits (`RuleParser::kMaximumRules`,
`Stylesheet::kMaximumDeclarations`, `kMaximumComponentValues`).

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

Every affected entity is restyled by the existing selective pass in `computeAllStyles`, and the
frame can take `RenderingContext::tryRetainedUpdate` instead of the full rebuild. Paths that set
`needsFullStyleRecompute` for reasons other than selectors (element-specific attributes on lines,
convolve matrices, conditional processing, `<use>` geometry) keep doing so until each is shown to
be local by the differential edit test.

**Why presentation attributes request a full restyle today is not stated in the code.** The
plausible dependents are `<use>` instances (covered by the reverse map), paint servers and
gradient stops (`ComputedStopComponent`), and text layout. Milestone 5 starts by running the
differential edit test with the current full restyle replaced by the narrowed set on the whole
corpus, and keeps the full restyle for any attribute whose narrowed result differs until the
missing dependent is identified.

**Stylesheet edits.** Loading imports keeps the full restyle, now one cascade per frame. Rule
edits restyle only the elements the changed rules match; see
[Restyle from Changed Rules](#restyle-from-changed-rules).

### Ancestor Bloom Filter

**Base-library helper.** `donner/base/BloomFilter.h` is header-only and depends on nothing but
the standard library. It follows the layout the base library uses for its other utilities (a
header in the `//donner/base:base` target, tests in `tests/BloomFilter_tests.cc` inside
`base_tests`, a `donner_cc_fuzzer` named
`bloom_filter_fuzzer` with a corpus under `tests/bloom_filter_corpus`). It provides:

- `BloomHash`: a 32-bit value that callers build once and reuse. `BloomHash::Of(bytes, salt)`
  computes a fixed, documented, platform-independent hash of a `std::string_view` (not
  `std::hash`, whose values differ between standard libraries, so tests and fuzzer corpora are
  reproducible).
  The salt separates key kinds that share spelling, such as the tag `g` and the class `g`.
- `BloomFilter<kLog2Bits>`: a plain bit-array filter with `insert(BloomHash)`,
  `mayContain(BloomHash)` and `clear()`, for sets that only grow.
- `CountingBloomFilter<kLog2Slots>`: 8-bit saturating counters with `insert`, `remove`,
  `mayContain` and `clear`. A counter that reaches 255 is never decremented again, so removal
  cannot create a false negative; the cost is a stuck false positive until `clear()`.
- Both derive their two probe positions from disjoint bit ranges of the 32-bit hash (bits 0 to
  `k-1` and `k` to `2k-1`), as Stylo does, so `kLog2Bits` and `kLog2Slots` are at most 16 and a
  `static_assert` enforces it. Sizing guidance in the header gives the expected false-positive
  rate for a number of keys, and `expectedFalsePositiveRate(keys)` computes it.

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
rule-element budget, so the budget outcome is the same with the filter on or off. Two counters
record filter rejections and filter passes that then failed in an ancestor walk (false
positives).

**Keeping the ancestor set correct.** Before matching element E, the filter is synchronized to
E's ancestor chain, which is the chain `ShadowedElementAdapter::parentElement()` produces (shadow
tree parents for shadow entities, stopping where the adapter stops). Synchronization pops stack
entries that are not on the chain, from the top, removing their hashes, then pushes the missing
ancestors from the outermost down. This makes the filter's content a function of E alone, so it
is correct in every traversal mode:

- Full pass in storage order: consecutive elements usually share most of their chain, so the
  cost is the difference between two chains. A later change may visit in tree order to make it
  one push and one pop per element; correctness does not depend on order.
- On-demand parent computation: `computePropertiesInto` computes a missing parent before
  inheriting from it, which synchronizes the filter to the parent's chain; the child's match
  happened earlier, before that call, and any later match synchronizes again first.
- Selective pass: dirty entities arrive in arbitrary order and each is synchronized.
- Shadow trees and `computeStylesFor`: the adapter's chain is used, so shadow and light trees
  never mix.
- External-use documents: each registry's traversal owns its own filter.

The filter belongs to one style pass object, created when `computeAllStyles` or
`computeStylesFor` starts and destroyed when it returns. A pass abandoned by a budget rejection
destroys its filter with it, so no later pass can see a half-maintained set. Chain
synchronization charges `SelectorTraversalBudget` one step per ancestor it pushes, and when the
budget refuses, the filter is cleared and disabled for the rest of the pass, which only loses the
rejections it would have made.

**Expected effect.** See [Ancestor Filter Opportunity](#ancestor-filter-opportunity): no
measurable time on the real documents, and up to 902 of 1,913 candidates on the descendant-only
synthetic document. The cost is one synchronization per element (a few hashes per ancestor
changed) and up to four probes per candidate. The goal guards against a regression on the real
documents and measures the effect by counter.

### Shared Computed Styles and the Matched Properties Cache

**Storage.** A new immutable `ComputedStyle` record holds what a computed style is today:
`PropertyRegistry properties` and `ComputedResourceOpacities resourceOpacities`.
In Part B it also holds the element's computed custom properties.
`ComputedStyleComponent` holds `std::shared_ptr<const ComputedStyle> style` and keeps
`externalFill` and `externalStroke`, which depend on the element's external-use context. Readers
change from `style.properties.value()`, `*style.properties` and `style.properties->` to a const
accessor, `style.properties()`, returning `const PropertyRegistry*` (null while the style is not
computed). The change is mechanical in the 32 reader files listed in
[Donner Today](#donner-today), and the compiler enforces it: the record is `const`, so a reader
that wrote through it would fail to compile. `ShadowTreeSystem::createShadowEntity` keeps
emplacing an empty component; `ExternalUseStyleContext::parent` holds the host's
`ComputedStyleComponent`, now a shared pointer instead of a copy. Milestone A8 makes this change
with no caching, so its correctness is checked by the existing suites alone.

**Cache key.** The cache maps a key to a `std::shared_ptr<const ComputedStyle>`. The key holds
everything a computed style depends on in `computePropertiesInto`:

- The matched rules, in order, with the specificity each was applied at: the stylesheet entity,
  sheet ordinal, rule index and specificity of each rule, hashed incrementally while
  `applyStylesheetRules` runs.
- The local declarations: a hash of `StyleComponent::properties`, computed and stored on the
  `StyleComponent` whenever it changes (parse, `setStyle`, presentation-attribute writes), with
  full `PropertyRegistry::operator==` on a hash match.
- The parent's record, by pointer identity. Two elements whose parents share a record inherit the
  same values, including font sizes resolved against the parent.
- Whether the parent carries `DoNotInheritFillOrStrokeTag`, and whether the element is a symbol
  that is a direct `<use>` instance root (`ApplySymbolInstanceDisplay`).
- The root element's record, for `rem` lengths (`ComputedRootFontSize`).

Elements are not cached when they have `AnimatedValuesComponent` (animation and contextual font
size), when they are an external-use root (`ExternalRootContext`), or when the cascade reported a
warning to the sink (a cached result would skip the diagnostic). The measured opportunity used
the same inputs and found no element whose key matched another's while its computed style
differed (see [Shared Computed Style Opportunity](#shared-computed-style-opportunity)).

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

**Memory budget.** Retained bytes are charged per distinct record instead of per entity:
`StyleResourceBudget::reserveComplexPropertyBytes` charges a record's heap bytes when it is
created and releases them when the last reference goes. The cache holds at most 4,096 entries and
at most 8 entries per hash bucket; past either bound it stops inserting until the next clear, so
an adversary who forces distinct keys gets today's behavior, not unbounded growth.

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
its specificity, which wins or loses the cascade exactly as a parsed value would. A shorthand sets
a pending entry on every longhand it covers. After the custom map is computed, each pending entry
is substituted (the fallback is used when the referenced name is absent), and the result is
parsed with `PropertyRegistry::parseProperty` at the entry's specificity into a property slot
reset to unset. A parse failure, a guaranteed-invalid reference without fallback, or a value past
the size cap makes the property invalid at computed-value time: it is left unset, so an inherited
property inherits and any other takes its initial value. Inheritance then runs as today.

**Presentation attributes.** A presentation attribute whose value contains `var()` is stored as a
pending-substitution entry at presentation-attribute priority and substituted like any other,
which is the behavior the SVG Working Group resolved to allow and Firefox and Safari implement.
The parse model is not yet specified, so this is an open question below.

**Limits.** A substituted value is capped at 65,536 component values and 1 MiB of source bytes,
below the 2 MiB both Blink and Stylo use, because an SVG document's whole parsed-payload budget is
smaller than a web page's. Substitution charges each produced component value to
`StyleResourceBudget::reserveDeclarationApplication`'s component work and its bytes to the byte
work, so one document's total expansion is bounded by the existing aggregate limits (16 Mi units
each). Past either cap the value is guaranteed-invalid and a diagnostic is reported once.

**Interactions.** The rule index is unchanged: custom properties do not affect matching. The
matched properties cache key is complete without change, because the custom map is a function of
the matched rules, the local declarations and the parent record. Narrowed invalidation already
recomputes the descendants of every restyled element, which is what a changed custom property
needs; the future "stop when equal" optimization must include the custom map in its equality.
`collectMatchedStyleRules` and the editor's annotations show custom declarations like any other.

### Cascade Layers

**Parsing.** `StylesheetParser` parses `@layer a, b;` statements and `@layer name? { ... }` blocks,
recursively, with nested blocks producing dotted names (`@layer a { @layer b {} }` is `a.b`) and
nameless blocks producing anonymous layers with a unique internal name. `@import url() layer(x)`
and `@import url() layer` are retained with their layer instead of dropped. Each `SelectorRule`
gains a `layer` id into the stylesheet's layer table; unlayered rules use a reserved id. Nesting is
capped at 32 levels and a stylesheet at 4,096 layers; past either cap the block is dropped with a
diagnostic.

**Order.** The document's layer order is the order of first declaration across all applicable
stylesheets in cascade order (imports first, as today), computed when stylesheets are parsed or
replaced. Each rule's layer id maps to a rank: the first declared layer has rank 1, later layers
higher ranks, and unlayered author rules the highest rank. Presentation attributes get rank 0,
below every author layer, placing them in the presentational-hint origin the specification
describes; with no layers present this keeps today's result, since presentation attributes
already lose to every author rule.

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

**Entry point.** `ProjectStyleContents` keeps building the element's concatenated text and source
map; for a node-value mutation that carries an edit range, `SVGDocument` maps the edit into the
stylesheet's local offsets through the source map and calls `StylesheetComponent::applyTextEdit`
instead of `parseStylesheet`.

**Rule window.** Every retained rule already carries its `ruleSourceRange`. The window starts at
the start of the last top-level rule that begins at or before the edit start (or at the sheet
start) and ends at the end of the first top-level rule that ends at or after the edit end (or at
the sheet end). The window text is parsed with the same rule parser and limits. The parse
resynchronizes when it consumes exactly the window: it ends at the window's end at top level, with
no open block, comment or string. If it does not (an edit that deletes a closing brace or opens a
comment swallows what follows), the window extends to the end of the sheet. If the window contains
or produces an `@import`, `@namespace`, `@font-face` or `@layer` rule, or the stylesheet's
resource stats would exceed a limit, the whole sheet is reparsed as today.

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
whose text equals the rule it replaces is unchanged. For each changed rule, old and new, the
elements it matches are found by testing it against the elements that carry its index keys (or
all elements for a universal entry) through the same matcher, charged to the style budget. Those
elements, their descendants and their `<use>` instances are marked for the selective pass; the
matched properties cache is cleared because rule identities changed. A changed rule that uses
`:has()`, or a change in layer order, falls back to one full restyle.

### CSS Highlighting and Completion

This is a scoped plan: what the engine provides and how the editor consumes it, not a visual
design.

**Regions.** CSS regions come from the XML token stream the editor already uses
(`xml::Tokenize`): the text content of `<style>` elements and the values of `style` attributes.
Presentation attribute values are CSS values too, and are classified as values of their property.

**Classification.** Each region is tokenized with `css::parser::details::Tokenizer`, and a small
state machine over the tokens tracks whether it is in a selector prelude, an at-rule prelude, a
declaration name or a declaration value. Tokens map to the existing `ColorIndex` classes
(selector parts, property names, keywords, numbers with units, strings, functions, comments,
punctuation); unknown property names and invalid tokens use the error class. Classification works
on one region at a time and, after an edit, reclassifies only the edited region.

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
- **Layers.** Nesting depth and layer count are capped in the parser, and the layer rank is 16
  bits by construction of the cap.
- **Incremental reparse.** The window parse runs the same parser with the same limits, and the
  stylesheet's resource stats are recomputed for the spliced result; any limit failure falls back
  to the full reparse, which already enforces them. The differential fuzzer feeds random edits,
  including unbalanced braces, comments and strings.
- **Editor data.** Completion lists built from document ids and classes are capped (256
  suggestions per request) and strings are displayed, never executed.

## Testing and Validation

### Part A Tests

- **Differential cascade test** (`//donner/svg/components/style:style_index_differential_tests`,
  new). For every SVG in the repository's test corpora (the resvg suite, renderer test data, the
  splash and icon files), compute all styles with the index and with the candidate list replaced
  by every rule, then compare every entity's computed `PropertyRegistry` with
  `PropertyRegistry::operator==`, plus `MatchedStyleRule` lists from `collectMatchedStyleRules`.
  The "every rule" candidate source is a test-only option on `StyleSystem`; the matcher, cascade
  and inheritance code are shared, so the oracle differs from production only in skipping no
  rule. The same test runs with the ancestor filter and the matched properties cache each on and
  off. Documents whose unindexed pass ends budget-rejected are reported separately and must not
  produce fewer matched rules with the index.
- **Differential fuzzer** (`//donner/svg/components/style:style_cascade_differential_fuzzer`,
  new). Generates a small element tree and a stylesheet from the fuzzer input, built on the
  existing `selector_parser_fuzzer` and `stylesheet_parser_fuzzer` inputs, and asserts identical
  computed styles with the index, filter and cache on and off. Run under `--config=asan-fuzzer`.
- **Differential edit test** (`//donner/svg/components/style:style_invalidation_differential_tests`,
  new). For each corpus document and each edit kind (class add, remove and replace; `id`;
  `style`; every presentation attribute the document uses), apply the edit, prepare a frame with
  narrowed invalidation, then compare every computed style with a document that received the same
  edit followed by a forced full restyle. Includes `<use>`, patterns, markers, text, sibling and
  descendant selectors, and filtered structural selectors (`:nth-child(1 of .selected)`,
  `:nth-last-child(2 of .a, .b)`, and both nested in `:is()` and left of a combinator) where the
  edited element is before, after and equal to the subject.
- **Index unit tests** for key extraction on every selector shape in
  [Donner Today](#donner-today), including namespaces, case, `:is()`, `:not()` and `:has()` in
  the subject compound; **filter unit tests** for selector-hash extraction (stopping at sibling
  combinators, ignoring pseudo-class arguments) and for chain synchronization in each traversal
  mode, including a pass abandoned by a budget rejection.
- **Base library.** `BloomFilter` property, saturation, removal and false-positive-count tests in
  `//donner/base:base_tests`, and `//donner/base:bloom_filter_fuzzer`.
- **Existing suites** must stay green unchanged: `//donner/css/...` tests,
  `//donner/svg/components/style:style_system_tests`, the CSS-related renderer tests and
  `//donner/svg/renderer/tests:resvg_test_suite`. Milestone A8's storage change relies on them.
- **Performance.** `css_cascade_perf_bench` (benchmark binary) and a `donner_perf_cc_test` whose
  correctness target asserts exact counters (elements cascaded, rules tested, full clears, filter
  rejections, cache hits) for fixed documents and edits, so a regression in work done fails the
  PR gate on any machine. Wall clock is recorded only by the `manual`/`perf` wall-clock target;
  correctness tests carry no time limits.
- The invariant "computed style is unchanged" is enforced by the three differential targets
  above; the rule "a full restyle cascades each element once" and "the filter rejects the
  expected candidates" by the counter test.

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
  attribute). The resvg suite has no custom-property or layer rows.
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
  new): a stylesheet and a sequence of random edits (including braces, quotes and comment
  delimiters); after each edit, the incrementally updated `css::Stylesheet` must equal a fresh
  parse of the new text, rule ranges included.
- **Stylesheet edit cases** in the differential edit test: the four measured keystrokes, a rule
  insertion and deletion, and an edit that changes a selector so the rule moves between elements.
- **Editor tests** with headless `TextEditorCore` and the autocomplete functions: classification
  of every CSS byte in the corpus' `<style>` elements and `style` attributes, and one test per
  completion context returning suggestions from its source.

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
- **A general incremental parser (tree-sitter or Lezer style).** Reusing arbitrary subtrees
  needs an error-tolerant grammar of its own; Donner's rules are flat and carry source ranges, so
  a rule window gives the same reuse with the existing parser and its limits.

## Decisions

- **Order of work.** The rule index lands before narrowed invalidation, as in the
  [Implementation Plan](#implementation-plan). The single-cascade fix already halves the cost of
  every edit, and the index shrinks what each remaining full restyle costs.
- **Budget semantics.** The rule-element budget is charged per candidate tested. Documents that
  exhaust the budget today only because of rules that cannot match style completely afterwards,
  under unchanged limits. This is the one intended behavior change of Part A.
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
- **Document scale.** Documents above the XML parser's default 8,192-node cap are not a target of
  these goals. The techniques scale with elements and rules, so larger documents benefit, but no
  goal is stated or measured for them.

## Open Questions

- **Retained-byte accounting for shared records.** Charging the computed-style budget per
  distinct record instead of per entity lets documents that exceed the 64 MiB retained-property
  limit today only through duplication style completely. Recommendation: charge per record, the
  same reasoning as the per-candidate decision.
- **`var()` in presentation attributes.** The SVG Working Group resolved to allow it but has not
  specified the parse model; Firefox and Safari support it, Chromium partially. Recommendation:
  support it as a pending substitution at presentation-attribute priority, matching
  `variable-presentation-attribute.html`, and revisit if the specified parse model differs.
- **Invalid text while typing.** The engine renders what the current text parses to, so typing
  through an unbalanced brace changes the rendering until it is closed. Recommendation: keep the
  engine exact (it equals export and reload) and, if the flicker bothers users, add an editor-side
  debounce that defers applying a stylesheet edit while the source has an unclosed block.

## Future Work

- [ ] Narrow invalidation for element insertion and removal using the sibling and structural
      dependency bits.
- [ ] Stop a descendant recompute where the recomputed style equals the previous one, so an edit
      that changes no inherited value does not walk the subtree.
- [ ] Mine `:is()` and `:where()` arguments for index keys.
- [ ] `@property` registration, as a separate design.
