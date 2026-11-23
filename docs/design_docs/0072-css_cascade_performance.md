# Design: CSS Cascade Performance

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
and the frame that follows runs the whole-document cascade twice. This design measures that cost,
surveys how browser engines make style resolution fast, and commits to the subset that pays for
Donner:

1. Cascade each element once per full restyle (today a full restyle cascades every element twice).
2. A per-stylesheet rule index keyed on the rightmost compound selector, so an element tests only
   the rules that can match it.
3. Constant-factor fixes in the selector matcher, each kept only if its benchmark moves.
4. Narrow style invalidation for single-element edits (class, `id`, `style`, presentation
   attributes) from the whole document to the elements whose style can change.

Computed style stays bit-identical. Load time and editor edit cost both benefit: on the measured
documents style is 47 to 89 percent of preparing the first frame without rasterization, and 80 to
95 percent of preparing the frame after a one-element edit.

## Goals

All numbers are against the baseline in [Measured Baseline](#measured-baseline), measured with the
same harness, machine class and method. Each goal names the counter or timer that decides it.

- **One cascade per element per full restyle.** A frame that follows a full restyle cascades each
  styled entity once. Today the splash document cascades 1,069 elements after a one-element class
  edit and the bootstrap-icons sprite cascades 10,274, twice its 5,137 entities.
- **Rules tested per element.** On `donner_splash.svg` (89 author rules, 4 user agent rules),
  rules tested per first frame fall from 57,838 to at most 5,000 (from 93 per element to at most
  8). On the synthetic 2,000-element, 200-rule document they fall from 408,608 to at most 20,000.
- **First-frame style time.** On `donner_splash.svg` style time falls from a 10.8 ms median to at
  most 5 ms. On the synthetic 2,000-element, 200-rule document it falls from 77.6 ms to at most
  15 ms. Style time on the rule-free synthetic and bootstrap-icons documents does not regress by
  more than 5 percent.
- **Edit cost.** A class, `style` or presentation-attribute write on one leaf element whose new
  and old values no selector depends on in a combinator position cascades at most the element, its
  descendants and its `<use>` instances. On the splash document the style time of the next frame
  falls from 18.8 ms to at most 1 ms, and on the bootstrap-icons sprite from 63 ms to at most 1 ms.
- **Budget headroom.** The rule-element match budget charges rules actually tested, so the
  synthetic 4,000-element, 1,000-rule document, which exhausts the 1,048,576-match budget today,
  styles completely under unchanged limits.
- **No behavior change.** For every document whose cascade completes within its budgets today,
  every `ComputedStyleComponent` is identical before and after each step, enforced by a differential
  test and fuzzer (see [Testing and Validation](#testing-and-validation)).
- **Bounded memory.** The rule index costs at most 16 bytes per selector-list entry plus shared
  key strings, and is charged to the document's existing parsed-payload stylesheet budget.

## Non-Goals

- No change to computed style, cascade order or selector semantics. Known deviations found while
  doing this work are reported separately (see [Decisions](#decisions)).
- No selector JIT or code generation.
- No parallel style traversal. Style resolution stays single-threaded inside the document lock.
- No `var()`, custom-property cascade or `@layer` support; Donner supports neither today, so their
  fast paths do not apply.
- No change to the public API. The index, counters and invalidation summaries are internal; public
  documentation keeps describing documents, elements and styles.
- No full document reparse, and no new trigger for one.
- Narrowing invalidation for stylesheet text edits and element insertion or removal. Those keep
  the full restyle (once per frame after this design) and are listed under
  [Future Work](#future-work).
- Raising the XML parser's default 8,192-node cap, which bounds the element count of documents
  parsed with default options.

## Next Steps

- Land the cascade counters and the benchmark (Milestone 1), so every later step reports its
  effect with the same instrument.
- Land the single-cascade fix (Milestone 2): the smallest change with the largest measured effect
  on edits.

## Implementation Plan

Each milestone is one independently reviewable PR unless noted. Estimates are engineering days,
including tests and review fixes.

- [ ] Milestone 1: Cascade counters and benchmark (1.5 days)
  - [ ] Add internal per-pass counters to `StyleSystem` (elements cascaded, rules tested, rules
        matched, declarations applied, full clears, budget rejections), readable through the
        existing `RenderingDiagnostics` snapshot.
  - [ ] Add `//donner/benchmarks:css_cascade_perf_bench` with the documents and edits in
        [Reproducing the Baseline](#reproducing-the-baseline).
  - [ ] Add a `donner_perf_cc_test` whose correctness target asserts the counter values for fixed
        documents (CPU-invariant, PR-gated) and whose wall-clock target records times (manual,
        `perf` tag).
  - Done when: the counters reproduce this document's counter columns exactly for the splash,
    bootstrap-icons and synthetic documents.
- [ ] Milestone 2: One cascade per full restyle (1 day)
  - [ ] Clear computed styles once per rebuild in `RenderingContext::createComputedComponents`,
        and make its second `StyleSystem::computeAllStyles` call compute only entities without a
        computed style (the shadow-tree clones the first call could not see).
  - [ ] Establish with a test, before the change, whether `ApplyAnimationPresentation` depends on
        the second pass discarding its overrides; keep animated documents on their current path if
        it does.
  - Done when: the correctness counter test shows elements cascaded equal to the styled entity
    count on the edit frames, and the next-frame style time after a splash class edit is at most
    10 ms (from 18.8 ms).
- [ ] Milestone 3: Rule index (4 days)
  - [ ] Build `RuleIndex` for a stylesheet's own rules and for each imported sheet when
        `StylesheetComponent::rebuildSelectorCaches` runs.
  - [ ] Collect candidates per element in `StyleSystem::applyStylesheetRules` and
        `collectMatchedStyleRules`; charge the budget per candidate.
  - [ ] Add the differential test and the differential fuzzer.
  - Done when: rules tested and style time meet the load goals, the differential test and fuzzer
    are green, and the Wasm size check passes with its current ceiling.
- [ ] Milestone 4: Matcher constant factors (2 days, one commit per change)
  - [ ] Remove the per-attempt heap allocation of the backtracking stack in
        `ComplexSelector::matchChain`.
  - [ ] Store each complex selector's specificity at parse time instead of recomputing it in
        `ComplexSelector::matches`.
  - [ ] Match `ClassSelector` against a class list split once per element instead of copying and
        splitting the `class` string for every class selector tested.
  - [ ] Resolve pseudo-class names to an enumeration at parse time.
  - Done when: each kept change shows a median improvement of at least 5 percent in candidate
    match time on the benchmark; a change that does not is dropped.
- [ ] Milestone 5: Narrowed invalidation for single-element edits (5 days)
  - [ ] Compute a `SelectorDependencySummary` per stylesheet at parse time.
  - [ ] Route class, `id`, `style` and presentation-attribute writes in `SVGElement` through it,
        marking the affected entities dirty for the existing selective style pass instead of
        setting `RenderTreeState::needsFullStyleRecompute`.
  - [ ] Restyle the `<use>` shadow instances of a written element.
  - [ ] Add the narrow-versus-full differential edit test.
  - Done when: the edit goals are met and the differential edit test is green.
- [ ] Milestone 6: Per-element base cost (2 days)
  - [ ] Break down the measured 4.6 microseconds of per-element style work outside rule matching
        with timers.
  - [ ] Remove avoidable whole-`PropertyRegistry` copies in `StyleSystem::computePropertiesInto`,
        `inheritComputedParent` and `inheritProperties`.
  - Done when: style time on the rule-free 1,000-element synthetic document falls from 6.1 ms by
    at least 20 percent, or the breakdown shows the cost is inherent and the result is recorded
    here.

Total committed estimate: 15.5 engineering days. The conditional items under
[Future Work](#future-work) are not included.

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
`:last-of-type`, `:only-of-type`, `:nth-child`, `:nth-last-child`, `:nth-of-type`,
`:nth-last-of-type`, `:is`, `:where`, `:not` and `:has`. Pseudo-class names are compared as
strings while matching (`PseudoClassSelector`). `ClassSelector::matches` copies the element's
`class` string and splits it for each class selector tested. Every traversal step is charged to
`css::SelectorTraversalBudget`.

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

**Computed style.** After the cascade, `ApplyAnimatedStyle`, contextual font sizes, scalar and
paint animation styles and symbol display are applied, then `inheritComputedParent` merges the
parent's computed `PropertyRegistry` (computing the parent first if needed). The result is stored
by value in `ComputedStyleComponent::properties` and memoized: `computePropertiesInto` returns
early when it is set. Fonts are resolved to absolute values against the parent.

**Shadow trees.** `<use>`, pattern, marker and mask instances are shadow entities.
`ShadowedElementAdapter` matches selectors against the shadow tree's structure (parent, siblings,
children) while reading `id`, `class` and attributes from the referenced light element. Each shadow
entity is cascaded like any other element.

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
false, so the second call only fills in the new shadow entities.

[0005-incremental_invalidation](0005-incremental_invalidation.md) introduced the dirty flags and
deferred a selector index for stylesheet-level changes; this design supplies that index and the
selector-dependency side of invalidation.

### Measured Baseline

Machine: an Arm Neoverse-N1 Linux machine, optimized build (`-c opt`), the benchmark process
pinned to one core while unrelated work ran on other cores (load average 40 to 72 during the
runs). Revision `a4cd11c9`. Each load case ran 9 times on a freshly parsed document; each edit
ran 19 times on one loaded document, discarding the first. Tables give the median with the
minimum and maximum in parentheses. "Style" is the inclusive time of the outermost
`StyleSystem::computeAllStyles` and `computeStylesFor` calls; "match" is the time inside
`applyStylesheetRules` (rule matching plus declaration application), from a separate run with
per-element timers that added under 0.5 ms to any style time. "Null frame" is
`RendererDriver::draw` with a renderer that records nothing, so it covers style, layout, shapes,
shadow trees and render-tree instantiation but no rasterization. "TinySkia frame" is the same draw
with the software renderer. Counters are exact and identical across runs.

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

### Reproducing the Baseline

The measurements used a local harness that Milestone 1 commits. Until then:

1. Add process-global counters and timers to `StyleSystem.cc`: increment counters in
   `applyStylesheetRules` (per rule tested and matched), `ApplyRuleDeclarations` (per
   declaration), `computePropertiesInto` (per element), and the full and selective branches of
   `computeAllStyles`; accumulate `std::chrono::steady_clock` time over the outermost
   `computeAllStyles` and `computeStylesFor` calls; optionally time `applyStylesheetRules` per
   element; record `StyleResourceBudget::rejected()` when `computeAllStyles` returns.
2. Build a binary depending on `//donner/svg`, `//donner/svg/renderer`,
   `//donner/svg/renderer:renderer_driver` and `//donner/svg/components/style:style_system` with
   `bazel build -c opt`. For each document: parse with `SVGParser::ParseSVG`, reset the counters,
   time `RendererDriver::draw` with a renderer whose methods do nothing, then time
   `Renderer::draw` (TinySkia) on a second fresh parse. For edits: parse and draw once, then
   alternate between two values of the attribute on one element and time the next draw.
3. Documents: `donner_splash.svg`, `third_party/bootstrap-icons/bootstrap-icons.svg`,
   `donner/svg/renderer/testdata/Ghostscript_Tiger.svg`, and generated documents: groups
   `<g class="gK">` of ten `<rect class="cN">` with no whitespace text nodes (the default
   8,192-node parser cap applies), rules `.cN`, `.gK .cN`, `rect:nth-child(N)` and `[data-kN]`;
   and a `<symbol>` of 20 `<rect class="cN">` used by 200 `<use>` elements.
4. Edit targets: the first `.cls-71` path in the splash (class toggled to `cls-88`), element
   `#e1000` of the synthetic document (class `c3` and `c7`), the first `symbol > path` of the
   sprite (classes `a` and `b`).
5. Pin the process to one core (`taskset`) and report medians with minimum and maximum.

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
It needs shareable, immutable computed-style storage.

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

**Cascade ordering.** Stylo sorts matched declarations by layer, specificity, scope proximity and
source order, packed into a 24-byte record because its size drives sort cost
([applicable_declarations.rs](https://github.com/servo/stylo/blob/main/style/applicable_declarations.rs)).

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

### Which Techniques Apply

Ranked by expected effect on the measured baseline per unit of complexity.

| Rank | Technique                                      | Decision        | Reason                                                                                                                                                                                                 |
| ---: | ---------------------------------------------- | --------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
|    1 | Cascade once per full restyle                  | Adopt           | Halves the style time of every measured edit; a few lines in one function.                                                                                                                             |
|    2 | Rule index on the rightmost compound           | Adopt           | On the stylesheet-heavy documents 97.6 to 99.2 percent of rule tests reject; the index removes most of them. Cost is one record per selector entry.                                                    |
|    3 | Narrowed invalidation for single-element edits | Adopt           | Turns an edit from two whole-document cascades into a subtree cascade. Reuses the existing selective pass and dirty flags.                                                                             |
|    4 | Matcher constant factors                       | Adopt, measured | 155 ns per rejected test points at per-attempt overhead; each fix is local and independently testable.                                                                                                 |
|    5 | Per-element base cost (copies)                 | Investigate     | 4.6 microseconds per element dominates rule-light documents; the breakdown is not yet measured.                                                                                                        |
|    6 | Ancestor Bloom filter                          | Defer           | After indexing, candidates mostly fail on the subject compound, not on ancestors. Adopt only if the post-index counters show descendant candidates rejected in the ancestor walk at a meaningful rate. |
|    7 | Matched properties cache                       | Defer           | Needs shared immutable computed-style storage, a broad change to every `ComputedStyleComponent` reader. Revisit after Milestone 6.                                                                     |
|    8 | Sibling and cousin style sharing               | Reject          | Correctness depends on a complete revalidation set over Donner's full selector support, including shadow-tree adapters; the matched properties cache would capture the same reuse with a simpler key.  |
|    9 | Rule tree                                      | Reject          | Its benefit, restyle without re-matching, is delivered by narrowed invalidation with far less machinery.                                                                                               |
|   10 | Selector JIT                                   | Reject          | Large complexity, no executable memory on WebAssembly, and a 2x ceiling on a cost the index already removes.                                                                                           |
|   11 | Parallel traversal                             | Reject          | Style runs under the single-owner document lock; most documents are smaller than a work-stealing unit.                                                                                                 |
|   12 | Custom property and layer fast paths           | Not applicable  | `var()` and `@layer` are unsupported.                                                                                                                                                                  |

## Proposed Architecture

### Single Cascade per Full Restyle

`RenderingContext::createComputedComponents` performs the full-restyle clear itself, once, before
the first `computeAllStyles` call, and both calls then compute only entities without a computed
style. Concretely, `StyleSystem::computeAllStyles` gains an internal mode parameter (full clear or
fill missing), used only by `createComputedComponents`; other callers keep today's behavior. The
second call already only needs the shadow-tree clones that `InstantiateMainShadowTrees` and
`PaintSystem::createShadowTrees` created after the first call.

Risk: `ApplyAnimationPresentation` runs between the two calls and writes animated values into
computed styles. Documents with `AnimatedValuesComponent` are forced onto the full restyle in
`ensureComputedComponents`. Milestone 2 first adds a test that pins what an animated document's
computed style is after an edit frame, then changes the pass structure; if animation correctness
depends on the second clear, animated documents keep the current two-pass path and the change
applies to the rest.

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

**Wasm size.** The index adds code to the shipped editor and renderer. Milestone 3 runs the Wasm
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
combinator (affects following siblings and their descendants). It also records whether any
selector uses `:has()`, a structural pseudo-class (`:nth-*`, `:first-*`, `:last-*`, `:only-*`,
`:empty`), or a key-less attribute selector. The summary is the union over the sheet's own and
imported rules, and the document view is the union over applicable stylesheets.

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
  back to `needsFullStyleRecompute`. Structural pseudo-classes do not depend on attribute values
  and need no fallback for these edits.
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

**Stylesheet edits.** Appending a rule, editing rule text and loading imports keep the full
restyle, now one cascade per frame. With the index, a rule edit could restyle only elements
carrying the edited rules' keys; that is [Future Work](#future-work).

### Interaction with Existing Incremental Paths

- The selective style pass and its two-part invariant (dirty flag plus removed computed style)
  are unchanged; narrowing only chooses which entities enter it.
- `ExternalUseStyleContext` and `ExternalUseStyleRetention` cascade referenced external documents
  with their own registries and stylesheets; each registry has its own index and summary, and the
  external-use aggregate budget is charged per candidate exactly as above.
- Text-content-dependent selectors (`:empty`) keep their existing `selectorsDependOnTextContent`
  handling.
- No path in this design reparses the document.

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
- **Invalidation.** Under-invalidation is a correctness bug, not a safety bug, but it can show
  stale content. Every uncertain case (`:has()`, key-less attribute selectors, attributes with
  unidentified dependents) falls back to the full restyle.
- **Bloom filter, if ever adopted.** False positives are safe; a false negative is a wrong style,
  so insertion and removal must be exactly paired, and the filter must be cleared, not reused,
  when a traversal is abandoned by a budget rejection.

## Testing and Validation

- **Differential cascade test** (`//donner/svg/components/style:style_index_differential_tests`,
  new). For every SVG in the repository's test corpora (the resvg suite, renderer test data, the
  splash and icon files), compute all styles with the index and with the candidate list replaced
  by every rule, then compare every entity's `ComputedStyleComponent` with
  `PropertyRegistry::operator==`, plus `MatchedStyleRule` lists from `collectMatchedStyleRules`.
  The "every rule" candidate source is a test-only option on `StyleSystem`; the matcher, cascade
  and inheritance code are shared, so the oracle differs from production only in skipping no
  rule. Documents whose unindexed pass ends budget-rejected are reported separately and must not
  produce fewer matched rules with the index.
- **Differential fuzzer** (`//donner/svg/components/style:style_cascade_differential_fuzzer`,
  new). Generates a small element tree and a stylesheet from the fuzzer input, built on the
  existing `selector_parser_fuzzer` and `stylesheet_parser_fuzzer` inputs, and asserts identical
  computed styles with the index on and off. Run under `--config=asan-fuzzer`.
- **Differential edit test** (`//donner/svg/components/style:style_invalidation_differential_tests`,
  new). For each corpus document and each edit kind (class add, remove and replace; `id`;
  `style`; every presentation attribute the document uses), apply the edit, prepare a frame with
  narrowed invalidation, then compare every computed style with a document that received the same
  edit followed by a forced full restyle. Includes `<use>`, patterns, markers, text and sibling
  and descendant selectors.
- **Index unit tests** for key extraction on every selector shape in
  [Donner Today](#donner-today), including namespaces, case, `:is()`, `:not()` and `:has()` in
  the subject compound.
- **Existing suites** must stay green unchanged: `//donner/css/...` tests,
  `//donner/svg/components/style:style_system_tests`, the CSS-related renderer tests and
  `//donner/svg/renderer/tests:resvg_test_suite`.
- **Performance.** `css_cascade_perf_bench` (benchmark binary) and a `donner_perf_cc_test` whose
  correctness target asserts exact counters (elements cascaded, rules tested, full clears) for
  fixed documents and edits, so a regression in work done fails the PR gate on any machine. Wall
  clock is recorded only by the `manual`/`perf` wall-clock target; correctness tests carry no time
  limits.
- The invariant "computed style is unchanged" is enforced by the three differential targets
  above; the rule "a full restyle cascades each element once" by the counter test.

## Alternatives Considered

- **Port the differential engine approach (Ladybird #11088).** Retaining match results and
  computed styles as materialized views and routing typed deltas gives the most general
  invalidation, but it is a rewrite of the style system. The narrowed invalidation here reaches
  the measured edit cases with the existing selective pass. The reference-evaluator idea is kept
  as the differential tests.
- **Hash maps instead of sorted vectors for the index.** Constant-time lookups, but hashing
  attacker-chosen keys needs a seeded hash to resist flooding, and the maps are larger and
  scattered. Element lookups are few (id, classes, tag, attributes), so binary search costs
  little.
- **Index per document instead of per stylesheet.** Fewer lookups per element with several
  `<style>` elements, but every stylesheet edit would rebuild a merged structure and source order
  across sheets would have to be encoded. Documents rarely have more than a few sheets.
- **Pre-parse declaration values once per rule.** Today each matched declaration is parsed into
  typed values per element. Caching typed values per declaration would help documents where many
  elements match the same rules; the measurements combine matching and declaration application,
  so its share is not known yet. Milestone 1's counters separate them, and it becomes a candidate
  if declaration application is a large share after Milestone 3.

## Decisions

- **Order of work.** The rule index lands before narrowed invalidation, as in the
  [Implementation Plan](#implementation-plan). The single-cascade fix already halves the cost of
  every edit, and the index shrinks what each remaining full restyle costs.
- **Budget semantics.** The rule-element budget is charged per candidate tested. Documents that
  exhaust the budget today only because of rules that cannot match style completely afterwards,
  under unchanged limits. This is the one intended behavior change.
- **Test-only unindexed candidate source.** The differential tests use a test-only option on
  `StyleSystem` that offers every rule as a candidate. It is a switch on the shared matcher and
  cascade path, not a second implementation.
- **Selector-list specificity.** `MatchSelectorRule` applies the first matching entry's
  specificity, where
  [Selectors Level 4](https://www.w3.org/TR/selectors-4/#specificity-rules) uses the most specific
  matching selector. That is tracked as
  [#2663](https://github.com/jwmcglynn/donner/issues/2663) and fixed separately; the index
  preserves whichever behavior the matcher has.
- **Shared computed-style storage.** Storing computed styles as shared immutable records is
  acceptable as a later refactor. It is not part of this design; the decision to start it waits
  for the per-element cost breakdown of Milestone 6.
- **Document scale.** Documents above the XML parser's default 8,192-node cap are not a target of
  these goals. The techniques scale with elements and rules, so larger documents benefit, but no
  goal is stated or measured for them.

## Future Work

- [ ] Ancestor Bloom filter, if the post-index counters show descendant and child candidates
      rejected in the ancestor walk at a meaningful rate.
- [ ] Matched properties cache, after shared computed-style storage.
- [ ] Restyle only elements carrying the keys of added, removed or edited rules on stylesheet
      edits.
- [ ] Narrow invalidation for element insertion and removal using the sibling and structural
      dependency bits.
- [ ] Stop a descendant recompute where the recomputed style equals the previous one, so an edit
      that changes no inherited value does not walk the subtree.
- [ ] Mine `:is()` and `:where()` arguments for index keys.
