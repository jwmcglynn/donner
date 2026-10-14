# SVG2 Audit {#Svg2Audit}

This ledger is for developers evaluating Donner v1.0 against SVG2. It contains a complete chapter
navigation inventory and a small set of inspected requirements. **No chapter is fully audited yet.**
The [current chapter review](svg2_chapter_review.md) records the first-pass findings and concrete native test priorities.
The inventory counts do not establish coverage. Passing rows identify only the narrow assertions exercised by named tests.
The implementation plan is [0057](design_docs/0057-donner_svg2_test_suite.md).

## Source Identity

The current [SVG Working Group Editor's Draft](https://w3c.github.io/svgwg/svg2-draft/single-page.html),
retrieved on 2026-10-14 UTC, displays **14 October 2026**. Its exact response is 4,905,241 bytes with
SHA-256 `66eb6613c2227b0781b2aab3038657a478ee91548f7966cf4528c1bb1d92cba5`.
The served bytes match the [immutable publication](https://raw.githubusercontent.com/w3c/svgwg/4387529e585712696584a8cd45859e3dd3f2f8d6/svg2-draft/single-page.html)
in publication commit `4387529e585712696584a8cd45859e3dd3f2f8d6` exactly. Its revision metadata and
the publication commit message identify [source revision fc7791bd](https://github.com/w3c/svgwg/tree/fc7791bd6d829cda3ed1502485c38329cbd8b00f).
This verifies publication-byte identity and upstream source attribution; it is not an independent
rebuild of Bikeshed and its dependencies. [Baseline metadata](svg2_audit_baseline.json) preserves the binding.

The initial review used `https://svgwg.org/svg2-draft/single-page.html`, which still serves an older
2,990,512-byte response, SHA-256 `13f29ae860ec726579a35bfa17156eb93a07b07563772c7e23712d1847fa7128`,
displaying 2025-09-14. The official [repository README](https://github.com/w3c/svgwg/blob/fc7791bd6d829cda3ed1502485c38329cbd8b00f/README.md)
now directs readers to the GitHub Pages publication. **The older endpoint is not the current audit target.**
The navigation inventory and initial requirement rows below retain their older-snapshot scope until
explicitly reconciled. They must not be counted as complete review of the current draft.

### Current-draft differences requiring reconciliation

These observations guide the next tests; they are not executed conformance results.

| Surface | Change from the initial snapshot | Audit consequence |
| --- | --- | --- |
| [Geometry](https://w3c.github.io/svgwg/svg2-draft/geometry.html) | Negative radius and sizing values now substitute initial values; width/height applicability wording also changed. | Recheck cascade/used-value regressions against current prose and preserve conflicting applicability wording as an open question. Explicit `auto` rectangle coverage remains a separate narrow assertion. |
| [Paths](https://w3c.github.io/svgwg/svg2-draft/paths.html) | Adds CSS `path-length` mapped from `pathLength`; incompatible additive/from-by path animations produce no effect, including frozen state. Removes segment-completing closepath abstraction. | Add CSS calibration/cascade cases and coordinate animation coverage. Ordinary equivalent paths and marker rules need reconciliation. |
| [Painting](https://w3c.github.io/svgwg/svg2-draft/painting.html) | Removes `miter-clip` and `arcs` values and their algorithms. | Do not classify their absence as a current SVG2 gap. Existing compatibility features/tests may remain separately identified. |
| [Text](https://w3c.github.io/svgwg/svg2-draft/text.html) | Removes several shape-wrapping properties; clarifies empty/no-valid-segment textPath fallback and that a valid prefix before an error wins; adds side reflection. | Withdraw older-snapshot shape-property and empty-path hypotheses before assigning fixes. Existing text work needs assertion-level reconciliation. |
| [Interfaces](https://w3c.github.io/svgwg/svg2-draft/types.html) | Reworks SVG point/rect/matrix interfaces and geometric query behavior. Most `animVal` getters still alias `baseVal`. | Native product animation accessors are not proof of matching Web IDL. Reconcile each interface independently. |
| [Structure](https://w3c.github.io/svgwg/svg2-draft/struct.html) and [styling](https://w3c.github.io/svgwg/svg2-draft/styling.html) | Removes legacy use-instance accessors, changes language matching and removes the never-rendered UA display rules from the older stylesheet. | Preserve rendering-tree requirements while rechecking the mechanism and host APIs. |
| [Images](https://w3c.github.io/svgwg/svg2-draft/embedded.html), [scripts](https://w3c.github.io/svgwg/svg2-draft/interact.html), [links](https://w3c.github.io/svgwg/svg2-draft/linking.html) | Adds image/script referrer policy and updates HTML processing/reflection integration. | Keep loader/host scope and no-script execution separate; the audit still records host requirements. |
| [Paint servers](https://w3c.github.io/svgwg/svg2-draft/pservers.html) | Changes radial focal-coordinate initial-value text. | Reconcile template and default cases before changing rendering or reference images. |

The complete chapter/IDL/index/dependency delta remains open. Existing SVG1.1 resvg fixtures and
reference images stay unchanged; a current SVG2 disagreement needs a separate case and cited rationale.
The repository stores citations, hashes and paraphrased assertions rather than a specification copy.
Use the immutable publication above for reproducibility, and review later changes at the next audit
checkpoint before moving the baseline. The historical 2018 Candidate Recommendation does not replace
this Editor's Draft target.

## Reading the Ledger

`not-audited` means no complete requirement review. `missing-test` means no adequate evidence is
recorded in this suite ledger, not a claim that no unit test exists anywhere. `implementation-gap`
records a specific known limitation. `covered-pass` and `covered-fail` require exact executed cases,
engine revision, backend/font configuration, and retained results. `spec-ambiguity` preserves a
question requiring interpretation; `not-applicable` needs a cited mode/class rationale.

For each section, inspect prose, tables, algorithms, error rules, and IDL members; reconcile the
indexes after the chapter pass. Cite existing tests only after reading their assertion and oracle.
A resvg category name, source-code grep, skip reason, or inventory entry is not passing evidence.
Record engine implementation and test-coverage gaps separately. Full audit completion cannot be
inferred from the number of mapped tests.

## Historical Chapter and Section Inventory

Section links and HTML table counts below are mechanically inventoried from the older `13f29ae8` response.
They have not been regenerated or admitted as current-draft coverage.
Counts include informative/example tables; they are navigation aids, not normative-requirement totals.
Interface-member inspection and table review remain open for every chapter, including chapters with
selected inspected requirements below. Appendix A contains consolidated IDL; Appendix I is an index.

| Chapter / appendix                                                                                                           | Direct sections               | HTML tables | Complete review |
| ---------------------------------------------------------------------------------------------------------------------------- | ----------------------------- | ----------: | --------------- |
| [Chapter 1: Introduction](https://svgwg.org/svg2-draft/single-page.html#chapter-intro)                                       | 4 indexed subsection anchors  |           0 | not-audited     |
| [Chapter 2: Conformance Criteria](https://svgwg.org/svg2-draft/single-page.html#chapter-conform)                             | 5 indexed subsection anchors  |           7 | not-audited     |
| [Chapter 3: Rendering Model](https://svgwg.org/svg2-draft/single-page.html#chapter-render)                                   | 11 indexed subsection anchors |           1 | not-audited     |
| [Chapter 4: Basic Data Types and Interfaces](https://svgwg.org/svg2-draft/single-page.html#chapter-types)                    | 7 indexed subsection anchors  |           9 | not-audited     |
| [Chapter 5: Document Structure](https://svgwg.org/svg2-draft/single-page.html#chapter-struct)                                | 13 indexed subsection anchors |          15 | not-audited     |
| [Chapter 6: Styling](https://svgwg.org/svg2-draft/single-page.html#chapter-styling)                                          | 10 indexed subsection anchors |           7 | not-audited     |
| [Chapter 7: Geometry Properties](https://svgwg.org/svg2-draft/single-page.html#chapter-geometry)                             | 8 indexed subsection anchors  |           7 | not-audited     |
| [Chapter 8: Coordinate Systems, Transformations and Units](https://svgwg.org/svg2-draft/single-page.html#chapter-coords)     | 14 indexed subsection anchors |          19 | not-audited     |
| [Chapter 9: Paths](https://svgwg.org/svg2-draft/single-page.html#chapter-paths)                                              | 7 indexed subsection anchors  |           9 | not-audited     |
| [Chapter 10: Basic Shapes](https://svgwg.org/svg2-draft/single-page.html#chapter-shapes)                                     | 8 indexed subsection anchors  |           4 | not-audited     |
| [Chapter 11: Text](https://svgwg.org/svg2-draft/single-page.html#chapter-text)                                               | 13 indexed subsection anchors |          24 | not-audited     |
| [Chapter 12: Embedded Content](https://svgwg.org/svg2-draft/single-page.html#chapter-embedded)                               | 5 indexed subsection anchors  |           5 | not-audited     |
| [Chapter 13: Painting: Filling, Stroking and Marker Symbols](https://svgwg.org/svg2-draft/single-page.html#chapter-painting) | 12 indexed subsection anchors |          33 | not-audited     |
| [Chapter 14: Paint Servers: Gradients and Patterns](https://svgwg.org/svg2-draft/single-page.html#chapter-pservers)          | 4 indexed subsection anchors  |           6 | not-audited     |
| [Chapter 15: Scripting and Interactivity](https://svgwg.org/svg2-draft/single-page.html#chapter-interact)                    | 10 indexed subsection anchors |          13 | not-audited     |
| [Chapter 16: Linking](https://svgwg.org/svg2-draft/single-page.html#chapter-linking)                                         | 4 indexed subsection anchors  |           6 | not-audited     |
| [Appendix A: IDL Definitions](https://svgwg.org/svg2-draft/single-page.html#chapter-idl)                                     | 0 indexed subsection anchors  |           0 | not-audited     |
| [Appendix B: Implementation Notes](https://svgwg.org/svg2-draft/single-page.html#chapter-implnote)                           | 3 indexed subsection anchors  |          11 | not-audited     |
| [Appendix C: Accessibility Support](https://svgwg.org/svg2-draft/single-page.html#chapter-access)                            | 2 indexed subsection anchors  |           0 | not-audited     |
| [Appendix D: Animating SVG Documents](https://svgwg.org/svg2-draft/single-page.html#chapter-animate)                         | 0 indexed subsection anchors  |           0 | not-audited     |
| [Appendix E: References](https://svgwg.org/svg2-draft/single-page.html#chapter-refs)                                         | 2 indexed subsection anchors  |           0 | not-audited     |
| [Appendix F: Element Index](https://svgwg.org/svg2-draft/single-page.html#chapter-eltindex)                                  | 0 indexed subsection anchors  |           0 | not-audited     |
| [Appendix G: Attribute Index](https://svgwg.org/svg2-draft/single-page.html#chapter-attindex)                                | 2 indexed subsection anchors  |           1 | not-audited     |
| [Appendix H: Property Index](https://svgwg.org/svg2-draft/single-page.html#chapter-propidx)                                  | 0 indexed subsection anchors  |           1 | not-audited     |
| [Appendix I: IDL Index](https://svgwg.org/svg2-draft/single-page.html#chapter-idlindex)                                      | 0 indexed subsection anchors  |           0 | not-audited     |
| [Appendix J: Media Type Registration for image/svg+xml](https://svgwg.org/svg2-draft/single-page.html#chapter-mimereg)       | 2 indexed subsection anchors  |           0 | not-audited     |
| [Appendix K: Changes from SVG 1.1](https://svgwg.org/svg2-draft/single-page.html#chapter-changes)                            | 2 indexed subsection anchors  |           0 | not-audited     |

## Executed Static Corpus Evidence

At commit `4e95ce40f8d1da681fadf9d9b4434adfa2d9bb8d`, discovery contains 1,696 cases: all
1,679 paired cases in the pinned resvg corpus and 17 Donner extension cases. The native
`//tools/svg_suite:donner_extension_tests` target completed all 17 extension cases at that revision
with zero strict comparison failures or skips. This extension-only run does not qualify resvg at
that revision. Counts and results are revision-specific; later fixtures require fresh evidence.

The table below records a historical 1,689-case run: all 1,679 pinned resvg cases and the 10 extension
cases present in that snapshot. It predates the seven automatic-radius fixtures and provides no
result for them. Its totals must not be presented as a run of that 1,696-case corpus or
combined with the newer extension results into one execution. The retained historical run binds renderer SHA-256
`66c9e64de7294f3a8d17cb002f96ef8eb4d7ee15c654a3ab01403a1d0cd3b593` to captured PNGs and unchanged
source/resource fingerprints. Its declared engine revision is
`696894f6b236364475faa55674741d05407d8252` and declared backend is `tiny-skia-basic`; these labels
are caller-supplied, while the binary hash measures the executable bytes. Build flags are unspecified.

| Corpus | Cases | Strict pass | Strict pixel mismatch | Render error | Default-policy pass | Default-policy mismatch |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| resvg | 1,679 | 16 | 1,661 | 2 | 1,357 | 320 |
| Donner extension | 10 | 10 | 0 | 0 | 10 | 0 |
| Total | 1,689 | 26 | 1,661 | 2 | 1,367 | 320 |

Strict means threshold 0, maximum mismatched pixels 0, and antialiasing differences included.
The separate default-policy measurement reuses the exact captured PNGs with threshold
`0.019999999552965164` (the existing `0.02f` value), maximum mismatched pixels 100, and antialiasing
differences excluded. It applies no per-case skip, override, or alternate reference. The comparator
SHA-256 is `004f749a9508d28622433192f2d5a968e8c79e374c2692e463703f3257e6ac5e`.
The two render errors have no actual PNG and are not counted as policy comparisons or passes.

Of the strict pixel mismatches, 1,341 pass that comparison policy. This measures policy sensitivity;
it does not establish a common root cause or prove those images conform. The 320 remaining pixel
mismatches group by corpus directory as 136 text, 98 filters, 47 structure, 17 painting, 15 paint
servers, and 7 masking. These are triage groups, not counts of distinct implementation defects.

`structure/svg/not-UTF-8-encoding.svg` reports the parser's unsupported XML encoding error.
`paint-servers/radialGradient/unresolved-xlink-href.svg` reports child setup exit 126 without a
specific failed operation; its original execution remains an unresolved infrastructure failure.
Five isolated reruns render that case successfully with identical comparison counts (32,385 strict
mismatches; zero default-policy mismatches), but do not establish the setup failure's cause or replace
the original result. Neither original error is a timeout or a measured pixel mismatch in this run.

Representative inspection confirms that `structure/style/external-CSS.svg` leaves its rectangle red
instead of the reference green: the source imports a stylesheet that sets that fill, consistent
with the documented CSS import gap. `shapes/rect/simple-case.svg` has the same visible green
rectangle in both captures and zero default-policy mismatch despite its strict mismatch. This is
sample evidence, not a diagnosis of all policy-sensitive cases. All 10 historical extension cases
pass strict comparison, including the focused CSS geometry `x` precedence case. The chapter audit and
unresolved corpus failures remain open.

## Inspected Requirements and Concrete Gaps

These initial rows paraphrase selected passages in the older `13f29ae8` response. Their current-draft
reconciliation is pending; they are not an exhaustive
breakdown of those sections. `audit/...` identifiers are stable local handles, not W3C identifiers.

| ID / source                                                                                                                                                        | Requirement or scope decision                                                                                                             | Existing evidence                                                                                                                             | State / required evidence                                                                                                        |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------- |
| `audit/modes/animation-disabled`, [2.2.1](https://svgwg.org/svg2-draft/conform.html#features)                                                                      | Disabling declarative animation suppresses its effect, including animations beginning at zero; it differs from a zero-time sample.        | Native scenario integration is pending.                                                                                                       | missing-test: compare base state against a begin-at-zero animation in static and animated modes.                                 |
| `audit/modes/external-disabled`, [2.2.1](https://svgwg.org/svg2-draft/conform.html#features)                                                                       | Disabled external references behave as failed fetches. Same-document and data URLs are excluded from that external-reference definition.  | No scoped resource trace is recorded here.                                                                                                    | missing-test: assert requests and fallback behavior, including same-document/data cases.                                         |
| `audit/modes/input-disabled`, [2.2.1-2.2.6](https://svgwg.org/svg2-draft/conform.html#processing-modes)                                                            | Static and animated image modes disable interaction; secure variants additionally disable external references.                            | The mode feature tables were inspected, not implementation qualification.                                                                     | missing-test: injected input must leave focus, link activation, and event-triggered animation unchanged in disabled modes.       |
| `audit/interaction/hit-regions`, [15.5.1 and 15.6](https://svgwg.org/svg2-draft/interact.html#PointerEventsProp)                                                   | Hit regions depend on geometry, visibility and pointer-events; clips restrict hits, masks and filters do not remove them in the same way. | Existing `PointerEvents` enum/unit tests do not by themselves establish end-to-end routing.                                                   | missing-test: production hit targets and pixel samples for each value, text cells, image rectangles, clipping/masking/filtering. |
| `audit/interaction/focus`, [15.7](https://svgwg.org/svg2-draft/interact.html#Focus)                                                                                | Focus and keyboard routing follow the SVG-adapted HTML model; non-rendered elements cannot receive focus.                                 | No complete focus scenario evidence is recorded.                                                                                              | missing-test: focus identity, tabindex ordering, use-shadow traversal, keyboard input, and visible focus style.                  |
| `audit/style/import`, [6.9](https://svgwg.org/svg2-draft/styling.html#RequiredCSSFeatures)                                                                         | Required CSS features include external stylesheets and import rules, subject to mode restrictions.                                        | `StructureStyle` skips `structure/style/external-CSS.svg` with an explicit CSS import limitation in `resvg_test_suite.cc`.                    | implementation-gap: reproduce the fixture, implement bounded resolution in applicable modes, and remove the verified skip.       |
| `audit/style/dynamic`, [6.9](https://svgwg.org/svg2-draft/styling.html#RequiredCSSFeatures)                                                                        | CSS pseudo-classes include input-dependent states such as hover, active, and focus.                                                       | A static reference alone cannot establish the transition between input states.                                                                | missing-test: input sequence, selected state, recomputed style, and rendered result.                                             |
| `audit/link/activation`, [16.2](https://svgwg.org/svg2-draft/linking.html#Links)                                                                                   | Link activation uses linked rendered hit regions and keyboard access; nested links are inactive containers.                               | No navigation-intent scenario evidence is recorded.                                                                                           | missing-test: target URL/activation trace, nested-link rule, pointer and keyboard activation, with no real navigation.           |
| `audit/animation/scope`, [Appendix D](https://svgwg.org/svg2-draft/animate.html)                                                                                   | SVG2 does not mandate all animation technologies. Supported methods obey processing-mode and use-shadow requirements.                     | Appendix D's complete short text was inspected; referenced algorithms were not.                                                               | missing-test: declare implemented methods and bind native production scenarios to the relevant animation specification.          |
| `audit/geometry/inline-x`, [7.6](https://svgwg.org/svg2-draft/geometry.html#XProperty) and [6.6](https://svgwg.org/svg2-draft/styling.html#PresentationAttributes) | Inline CSS x overrides the rect presentation attribute.                                                                                   | `donner_svg2_corpus/tests/geometry/x/css-overrides-presentation.svg` and its analytically authored PNG exercise an integer-aligned rectangle. | covered-pass for this assertion on TinySkia through `//tools/svg_suite:donner_extension_tests`; not complete geometry coverage.  |

The implementation-gap row is evidence from the existing test policy, not a new execution result.
Its current reproduction, root cause, and exact applicability still require qualification. The inline-x reference has independent full-pixel review and a wrong-x negative control. The installed resvg 0.47.0 comparison differs at 1,600 pixels on this case; that is one measured engine/case result, not a general support claim. Many resvg
static categories already exercise structure, coordinates, paint, filters, shapes, and text; each
case's normative mapping remains unreviewed until an assertion-specific row is added.

## Specifications Referenced by This Work

The older [Appendix E.1](https://svgwg.org/svg2-draft/refs.html#NormativeReferences) supplied the initial
direct normative-reference inventory; it is historical and awaits current-draft reconciliation. The links below identify the principal dependencies for the initial
slices; their full contents, current edition selection, and transitive references are **not audited**.
A link is not a revision pin. Record the exact section and revision/content hash when an oracle
actually relies on it. Do not automatically substitute a newer CSS or platform edition when doing
so changes the SVG2 requirement under review.

| Surface                                        | Primary specifications                                                                                                                                                                                                                                                                              | Pin/review state                                                                 |
| ---------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------- |
| Cascade, selectors, geometry units, transforms | [CSS 2.1](https://www.w3.org/TR/CSS2/), [Cascade](https://www.w3.org/TR/css-cascade-4/), [Values](https://www.w3.org/TR/css-values-3/), [Transforms](https://www.w3.org/TR/css-transforms-1/)                                                                                                       | not-audited                                                                      |
| Text, fonts, writing modes                     | [Fonts](https://www.w3.org/TR/css-fonts-3/), [Text](https://www.w3.org/TR/css-text-3/), [Writing Modes](https://www.w3.org/TR/css-writing-modes-3/), [Unicode](https://www.unicode.org/versions/)                                                                                                   | not-audited                                                                      |
| Effects and color                              | [Masking](https://www.w3.org/TR/css-masking-1/), [Filter Effects](https://www.w3.org/TR/filter-effects-1/), [Compositing](https://www.w3.org/TR/compositing-1/), [Color](https://www.w3.org/TR/css-color-3/)                                                                                        | not-audited                                                                      |
| DOM, events, focus, URLs and loading           | [DOM](https://dom.spec.whatwg.org/), [UI Events](https://www.w3.org/TR/uievents/), [HTML](https://html.spec.whatwg.org/multipage/), [URL](https://url.spec.whatwg.org/), [Fetch](https://fetch.spec.whatwg.org/)                                                                                    | not-audited                                                                      |
| Declarative animation                          | [SVG Animations](https://svgwg.org/specs/animations/), [SMIL timing](https://www.w3.org/TR/SMIL3/smil-timing.html), [SMIL Animation](https://www.w3.org/TR/smil-animation/), [CSS Animations](https://www.w3.org/TR/css-animations-1/), [CSS Transitions](https://www.w3.org/TR/css-transitions-1/) | not-audited; Appendix D links animation technologies without mandating every one |
| Accessibility                                  | [WAI-ARIA](https://www.w3.org/TR/wai-aria-1.1/), [SVG AAM](https://www.w3.org/TR/svg-aam-1.0/), [Graphics ARIA](https://www.w3.org/TR/graphics-aria-1.0/)                                                                                                                                           | not-audited                                                                      |
| XML and interface semantics                    | [XML](https://www.w3.org/TR/xml/), [Namespaces](https://www.w3.org/TR/xml-names/), [XML Stylesheets](https://www.w3.org/TR/xml-stylesheet/), [Web IDL](https://webidl.spec.whatwg.org/)                                                                                                             | not-audited                                                                      |

The source bibliography places some animation references outside its normative-reference list.
Keep their classification and the requirements that actually invoke them explicit. Draft status is
reported in the baseline, not converted into an artificial failing test. A supported SVG feature
can legitimately be specified by a draft; passing evidence remains scoped to that pinned draft.

## Completion Criteria

The complete audit requires section-level assertions for all applicable requirements, reviewed
processing-mode/class scope, index reconciliation, external dependency pins for used behavior, and
implementation/test evidence. It must retain unresolved ambiguity and missing evidence. Passing a
static corpus, mechanically extracting all headings, or importing historical requirement JSON does
not satisfy those criteria. The next audited slices should turn the concrete rows above into native
regressions and then continue chapter by chapter.
