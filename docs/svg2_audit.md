# SVG2 Audit {#Svg2Audit}

This ledger is for developers evaluating Donner v1.0 against SVG2. It contains a historical chapter
navigation inventory and current requirement reviews for chapters 6, 7 and 12, plus a narrow XML input review. **The overall audit
remains incomplete:** dependency closure, host applicability, implementation and test evidence still
have open items. Local chapter reading is distinct from conformance.
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
The current local reviews below supersede this historical reading state for chapters 6, 7 and 12.
Other chapters still need complete interface-member and table inspection. Appendix A contains
consolidated IDL; Appendix I is an index.

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

## XML input decoding and character validity

This is a narrow dependency review, not completion of the XML/interface audit. The examined
[XML 1.0 Fifth Edition](https://www.w3.org/TR/2008/REC-xml-20081126/#charencoding), retrieved on
2026-10-14, is 219,926 bytes, SHA-256
`f58a4b9e1e5b8bac13fb55c4e5c7c8c5e2c176aa45ee68e2f1ab2498511927ec`.
It requires UTF-8/UTF-16 support and fatal rejection of malformed UTF-8; other encodings are optional.
Its [Char production](https://www.w3.org/TR/2008/REC-xml-20081126/#charsets) separately excludes
forbidden XML characters. These rules concern XML processing, not HTML replacement behavior.

The official campaign's `structure/svg/not-UTF-8-encoding.svg` declares Windows-1251. A private
diagnostic preserved that original and its reference, then ran equivalent decoded content in three
encodings through the same native Donner child. The Windows-1251 input reports unsupported encoding;
a UTF-16 BOM copy fails at the first node; the UTF-8 copy renders but still has 6,580 strict mismatch
pixels. This identifies decoding outcomes, not a passing reference comparison. It does not change
the original campaign's denominator or statuses. Derivative inputs used the direct native child,
because the canonical Qt adapter deliberately fixes its corpus root.

`XMLParser::Parse` explicitly accepts UTF-8 bytes and tells callers to transcode other encodings.
That API contract is distinct from the whole-file/resource decoding boundary needed by an SVG XML
processor. Windows-1251 rejection alone does not establish a mandatory XML defect; the tested file
entrypoint's UTF-16 rejection remains an open decoding gap.

A second native diagnostic placed byte sequences in otherwise identical metadata content, followed
by a green rectangle. Both valid controls rendered the exact analytic 10x10 reference. Eight inputs
were invalid: five malformed UTF-8 sequences (lone continuation, overlong, truncated before the next
ASCII byte, encoded surrogate and above U+10FFFF), and three forbidden XML characters (NUL, U+000B
and U+FFFE). Only NUL was rejected. The other seven rendered the reference exactly. Thus **three of
ten input-acceptance assertions were satisfied**, despite nine passing static image comparisons.
A matching PNG cannot prove that malformed XML was rejected.

The immutable renderer was built from `4e95ce40`; the relevant XML parser and native render-entry
source bytes are unchanged in `9a896a09`. These observations are not a fresh build of the latter
revision. The raw-text scanner can consume metadata without a complete UTF-8/XML-character check;
validation of names and numeric references does not establish validation of raw character data.

Required repair evidence remains open: native parser and file-entrypoint regressions for raw data,
attributes, comments and CDATA; contiguous and chunked input; real U+FFFD and valid multibyte controls;
source offsets; no partial accepted document; and bounded validation under incremental edits. Choose
the decoding/validation boundary without adding repeated whole-document scans to ordinary edits.
No golden, tolerance, skip policy or implementation changed in these diagnostics.

## Chapter 6: styling requirement review

This completes the local reading of the pinned current `fc7791bd` draft (`66eb6613` publication):
6.1-6.10.1, the style-element summary, all seven tables, both UA stylesheet blocks, the required
property/feature lists, and SVGStyleElement including LinkStyle. One table records a historical SVG2
resolution; five define attributes and one maps presentation attributes. No separate numbered local
algorithm is present; imported processing algorithms remain dependencies. **Implementation, dependency
closure and conformance remain incomplete.** Core attributes, global event attributes and inherited
SVGElement members stay with their defining chapters; listing them in the style-element summary
does not establish their implementation.

Source observations below refer to foundation `9a896a09`; the inspected styling files are byte-identical
to `509f9d46`, whose completed full invocation supplies the named native-test evidence. Existing tests
are narrow assertions, not proof that every property, stylesheet route or host behavior works.

### Current requirements and evidence

| Section | Required behavior | Evidence and remaining assertion |
| --- | --- | --- |
| [6.1 CSS mechanisms](https://w3c.github.io/svgwg/svg2-draft/styling.html#StylingUsingCSS) | SVG2 requires the chapter's styling mechanisms, including embedded stylesheets and style attributes. | Basic native style/cascade tests pass; this does not cover external sheets, host integration or dynamic state. A static-only product mode needs an explicit applicability decision, not a blanket exemption from this chapter. |
| [6.2 style element](https://w3c.github.io/svgwg/svg2-draft/styling.html#StyleElement), type/text | Missing type means CSS; type/media/title are non-animatable. Style text and attributes follow HTML processing. Type is obsolete for authors but still has processing semantics. | `SVGStyleElementTests.DefaultsToCssType`, `SetTypeControlsCssParsing` and `SetContentsOnlyParsesCssType` pass. `StylesheetComponent::isCssType` accepts empty and case-insensitive text/css. These tests do not prove cached rules stop applying when type changes, or that detached/reattached styles behave correctly. Retain a type-toggle with existing parsed rules, text/CDATA concatenation and empty-type controls. |
| 6.2 media/title/processing | Media is a query list, absent media applies unconditionally; title participates in sheet-set selection. HTML style-block update also depends on connection, type, security policy and sheet lifetime. | `SVGStyleElement.h` explicitly marks media/title unsupported; the stored component has no media/title/disabled state. Add matching/nonmatching media and alternate-sheet controls after defining the host environment. Detached style eligibility and mutation are part of the open shared-order repair. CSP, load/error events and host rendering/script blocking remain unqualified integration requirements. |
| 6.2 never-rendered style | Style text is not rendered, and the chapter explicitly requires a UA display:none rule that outranks other CSS/presentation values. | Current UA text has no style-specific display rule. A never-rendered element path may still suppress pixels; that is not the same assertion as computed display and UA priority. Test author-important display on style, its computed value and no visible text separately. Removal of the old general never-rendered rule from 6.8 does not remove this clause. |
| [6.3 external sheets](https://w3c.github.io/svgwg/svg2-draft/styling.html#LinkElement) | HTML-namespace link/rel=stylesheet loads and applies sheets, including sheets outside inline SVG. XML namespace handling differs from HTML parsing. Import and XML stylesheet PI are alternative routes. | `SVGParser` retains foreign elements as unknown, rather than implementing HTML link processing; `StylesheetParser` explicitly rejects @import loading. No external CSS-loading pass is established. Add bounded local-loader order/relative-URL/type/media controls, failures and prohibited-mode controls; do not enable unrestricted fetching to make a reference pass. |
| [6.4 mixed documents](https://w3c.github.io/svgwg/svg2-draft/styling.html#StyleSheetsInHTMLDocuments) | SVG and HTML style elements in HTML apply across HTML/inline SVG; HTML style in SVG also applies to that document. | Current rule application admits SVG-namespace stylesheet components. HTML document ownership, cross-namespace sheets and host CSSOM remain gaps; retaining unknown HTML elements is not implementation of their styling semantics. |
| [6.5 class/style](https://w3c.github.io/svgwg/svg2-draft/styling.html#ElementSpecificStyling) | All SVG elements support class tokens and declaration-list style. Class is animatable; style is not. HTML semantics apply except SVG className reflection. | `CSSClassSelector`, `CSSIdSelector`, `InlineStyleOverridesAttribute` and `ComputedStyleAfterClassMutation` assert narrow computed-color behavior. Add multiple-token/whitespace/case controls, malformed declaration recovery, relative URLs and mutation/query-before-draw. Native class setters are not proof of animated className/Web IDL. |
| [6.6 presentation cascade](https://w3c.github.io/svgwg/svg2-draft/styling.html#PresentationAttributes) | Presentation values occupy author origin before other author sheets, specificity zero; embedded !important is invalid because the attribute contains a value, not a declaration. SVG attribute grammar extensions remain governed by 4.2. | `PresentationAttribute`, `StylesheetOverridesPresentationAttribute` and `InlineStyleOverridesAttribute` pass for fill. Add attribute `!important`, invalid-value recovery and equal-rank order controls. Unitless attribute lengths versus CSS lengths remain the separately scoped chapter-7/PR-1871 work. |
| 6.6 mapping table | Geometry attributes apply only to listed elements: cx/cy circle/ellipse; r circle; rx/ry ellipse/rect; d path; x/y/width/height foreignObject/image/rect/svg/symbol/use. Fill excludes animation-element fill. Transform maps through patternTransform/gradientTransform on the designated paint servers; the remaining listed common attributes apply to SVG-namespace elements. Nonmatching/foreign attributes must not affect CSS inheritance or cascade. | Registry allowlists and element parsing are source seams, not proof of the entire table. Add text/tspan x and radialGradient r negative controls, foreign-namespace controls, transform mapping and animation-fill disambiguation. Audit the full common-attribute list against actual consumers; parser acceptance alone is insufficient. New CSS properties do not automatically gain presentation attributes. Chapter-7 CSS sizing applicability conflicts remain open. |
| 6.6 animation | Animating a presentation attribute has the effect of animating its mapped property. | Main base `6f197042` and this foundation lack the Animation specificity tier. The separately developed animation branch `756165046` is coordination evidence only. Integration must preserve normal-author < animation < author-important and explicit native overrides; class animation, mapped paint-server transforms, base restoration and computed queries need source-bound native clock tests. |
| [6.7 required properties](https://w3c.github.io/svgwg/svg2-draft/styling.html#RequiredProperties) | All SVG-defined properties and every listed external property family are required, subject to the consuming feature's applicability. | Keep the complete family routing below. Broad resvg passes, registered names and optional text backend presence do not establish these semantics. |
| [6.8 UA sheet](https://w3c.github.io/svgwg/svg2-draft/styling.html#UAStyleSheet) | SVG/default and XML namespace selectors, overflow clipping for the listed elements, transform-origin defaults/exceptions, xml:space preservation, and link/visited cursor styling. Interactive UAs also need distinct focus and selection styling. | `UserAgentStylesheet.h` resembles the block but uses text-space-collapse where the pinned SVG2 block uses white-space-collapse. Resolve the dependency conflict below before choosing an oracle. Add non-square transform-origin and nested/root/foreignObject namespace controls, overflow pixels, preserved spaces and link cursor state. Selector state registration/dependency tracking does not prove matching or input-driven invalidation. |
| 6.8 OpenType note | The cited OpenType embedding profile adds text/foreignObject suppression and root context paint, opacity and stroke values adjusted for glyph coordinates. | This is an informative note pointing to a separate profile's requirements. Font availability or ordinary text rendering is not SVG-in-OpenType coverage. Pin the chosen OpenType profile and host contract before claiming applicability or testing context-value propagation. |
| [6.9 CSS rules](https://w3c.github.io/svgwg/svg2-draft/styling.html#RequiredCSSFeatures) | XML stylesheet PI, CSS2.1 media/import/charset, and font-face rules. | Stylesheet parsing collects font-face rules, warns and skips import, and does not apply media rules. PI association/encoding order is not established. Add import precedence, conditional application, charset/transport decoding precedence and font loading/failure/selection cases with pinned resources. Parsing font-face syntax does not prove fetching or font selection. |
| 6.9 selectors | CSS2.1 pseudo-classes, including first-child/lang and interactive/link states; first-letter and first-line pseudo-elements on text. | `Selector.h` implements structural matching; dynamic/link/lang names do not have matching branches in the inspected path. `PseudoElementSelector` is explicitly invalid/nonmatching. Add native state/input assertions for the chosen interactive mode; visited-history privacy, focus/selection rendering, language inheritance and first-line/letter layout remain gaps. No JavaScript test harness is necessary for native state tests. |
| [6.10.1 SVGStyleElement](https://w3c.github.io/svgwg/svg2-draft/styling.html#InterfaceSVGStyleElement) | Window exposure, SVGElement inheritance, reflected string type/media/title, boolean disabled, and LinkStyle.sheet. | The local interface matches the consolidated IDL. Native type/content helpers do not implement these Web IDL requirements. HTML disabled accesses associated-sheet state; setting it before a sheet exists is not persistent content-attribute reflection. CSSOM sheet is nullable. Need explicit native adapter/host applicability decisions and lifecycle tests, rather than assuming no-JavaScript means not applicable. |

Primary implementation seams are [StylesheetParser](../donner/css/parser/StylesheetParser.cc),
[SVGStyleElement](../donner/svg/SVGStyleElement.cc),
[StyleSystem](../donner/svg/components/style/StyleSystem.cc),
[PropertyRegistry](../donner/svg/properties/PropertyRegistry.cc),
[Specificity](../donner/css/Specificity.h), [selectors](../donner/css/Selector.h), and
[UA rules](../donner/svg/core/UserAgentStylesheet.h).

### Cascade work and evidence limits

The existing `SVGStyleCascadeTests` cover normal selector specificity, important versus normal,
inheritance, and equal specificity within one style element. They do not establish ordering across
multiple style elements, preservation of selector specificity within important declarations, or
valid deferred-property selection.

The separate CSS-length candidate `5b2f9a18` has 25 previously executed analytic cascade probes:
13 pass and 12 fail, no skips. These reproduce deferred x validity/ordering/priority failures and a
parsed-fill cross-stylesheet-order failure. They are branch-bound evidence, not new results from this
audit and not proof that every foundation path behaves identically.

The importance/origin repair is a separate implementation unit: preserve selector specificity,
inline identity, UA-important precedence and editor winner annotations in the existing rank type.
The second unit remains open: shared attached-document stylesheet order plus validated deferred
selection, with mutation/read-query lifetime and bounded traversal tests. Neither unit is credited
as merged coverage here. A declaration invalid at parse time is discarded before selection; a valid
winner that fails at a later value stage must not trigger a blind lower-declaration retry.

### Required-property routing and exact editions

The 6.7 list remains a checklist, not a list of supported features. Its bibliography names:

| Family required by 6.7 | Bibliography edition; remaining audit owner |
| --- | --- |
| All SVG-defined properties | This pinned SVG2 draft; each defining chapter must close grammar, cascade, computed/used values, rendering and animation assertions. |
| display/overflow/visibility | CSS2.1 REC 2011-06-07; rendering/layout and host clipping. |
| cursor/text-overflow | Overflow 3 WD 2018-07-31 is cited, but cursor's direct link goes to CSS UI 3; resolve that citation mismatch, then audit interactive cursor and text truncation. |
| clip-path/clip-rule/mask | Masking 1 CR 2014-08-26; clipping/masking chapter and reference/resource behavior. |
| color/opacity | Color 3 REC 2018-06-19; parsing, inheritance, group opacity and blending. |
| filter/color-interpolation-filters/flood-color/flood-opacity/lighting-color | Filter Effects 1 WD 2014-11-25; filters and primitive-specific consumers. |
| isolation | Compositing 1 CR 2015-01-13; isolated group compositing. |
| transform/transform-box/transform-origin | Transforms 1 WD 2017-11-30; coordinates, reference boxes and UA defaults. |
| letter-spacing/text-align/text-align-last/text-indent/word-spacing | Text 3 WD 2017-08-22; text layout and applicability. |
| white-space | Text 4 WD 2015-09-22; preserve the naming conflict with current SVG2 below. |
| vertical-align/dominant-baseline/alignment-baseline/baseline-shift | Inline Layout 3 WD 2018-08-08; baseline and inline layout. |
| direction/text-orientation/writing-mode | Writing Modes 3 CR 2018-05-24; bidi, vertical layout and inheritance. |
| font shorthand/family/feature-settings/kerning/size/size-adjust/stretch/style/variant and its subproperties/weight | Fonts 3 PR 2018-08-14; font shorthand resetting, selection, loading and shaping. |
| text-decoration/line/style/color | Text Decoration 3 CR 2018-07-03; text decoration layout/paint. |

This local reading records those exact dependencies without claiming their entire contents were
audited. The narrower processing dependencies examined are CSS2.1 sections 4-7 (syntax/encoding,
selectors, cascade/import and media), [Style Attributes REC 2013-11-07](https://www.w3.org/TR/2013/REC-css-style-attr-20131107/),
and [XML Stylesheet REC 2010-10-28](https://www.w3.org/TR/2010/REC-xml-stylesheet-20101028/).
Style Attributes requires declaration-list error recovery, author inline precedence and relative
URL resolution; these are independent assertions beyond accepting a simple style string.

Do not silently replace conflicting or mutable dependencies:

- The chapter links CSS2.1 cascade and a live Cascade 3 definition, while the wider SVG2 definitions
  also cite Cascade 4 CR 2016-01-14. UA-important/animation origins and later CSS-wide values require
  explicit reconciliation. User-origin styles and transitions are not covered by the current repair. CSS2.1 also requires
  user control over author-sheet influence; the applicable native/interactive host API remains open.
- Its direct font-face link is Fonts WD 2011-10-04, while the bibliography selects Fonts PR
  2018-08-14. Both were retained; newer descriptor/selection requirements cannot be inferred from
  the older link alone.
- The explicit current SVG2 UA rule says white-space-collapse. Its cited Text 4 WD 2015-09-22 names
  text-space-collapse; Donner currently uses that older spelling. Preserve this conflict and test
  resulting whitespace behavior before renaming a property or accepting a reference.
- The direct Media Queries alias currently resolves to Level 3 REC 2024-05-21, and the direct CSSOM
  alias to WD 2021-08-26. These retrieved responses are pinned separately, not represented as dated
  SVG2 bibliography selections. HTML style/link processing is a retained living snapshot, not an
  immutable dated recommendation. CSSOM LinkStyle and HTML reflection/disabled rules need an
  explicit adapter contract; media, CSP/origin checks, external loading and visited-state privacy
  remain host boundaries.

This chapter slice changes no implementation, thresholds, references or skip policy. It records
local obligations and bounded evidence while leaving dependency conflicts and host applicability visible.

## Chapter 7: geometry-property requirement review

This completes the local clause review of the pinned current `fc7791bd` draft (`66eb6613` publication
bytes): all eight subsections, seven property-definition tables, the negative-value table and the
sizing table. Chapter 7 defines no IDL interfaces; geometry reflection and live objects remain with
chapters 4, 8 and 10. **The chapter is not implementation- or conformance-complete.**

Foundation source observations refer to `efd169b1`, whose inspected geometry code matches `4e95ce40`.
The separate CSS-length proposal at `3bfd6270` is identified explicitly below. Source presence,
loop iterations and wrapper-test counts are not interchangeable with executed native cases.

### Current requirements and bounded evidence

The seven SVG-specific property tables all say non-inherited, visual, and animated by computed
value. Their computed values retain percentages and otherwise use absolute lengths. This does not
remove explicit CSS-wide inheritance or establish any particular animation technology. Width and
height obtain their definitions from CSS2.1 with the SVG-specific applicability and auto resolution
in 7.8.

| Section / property | Required behavior | Foundation evidence and remaining case |
| --- | --- | --- |
| [Negative-value handling](https://w3c.github.io/svgwg/svg2-draft/geometry.html#geometry-negative-values) | Negative coordinates remain valid. Negative r substitutes initial 0; negative rx/ry/width/height substitute initial auto, in attributes **and CSS declarations**. Normal used-value resolution follows. | This supersedes the older audit's proposed negative-declaration fallback oracle. Radius helpers and no-geometry branches show some matching used results, but computed-value normalization and cascade participation need targeted assertions. Compare a positive attribute/lower declaration against a winning negative value, plus negative-coordinate controls. |
| [7.1 cx](https://w3c.github.io/svgwg/svg2-draft/geometry.html#CX) | Circle/ellipse horizontal center; initial 0; viewport-width percentages; negatives allowed. | `CircleComponent`/`EllipseComponent` and `ShapeSystem` use X extent. `missing-test`: non-square viewport, translated negative center, CSS precedence and computed lengths under nondefault font metrics. |
| [7.2 cy](https://w3c.github.io/svgwg/svg2-draft/geometry.html#CY) | Circle/ellipse vertical center; initial 0; viewport-height percentages; negatives allowed. | Same path uses Y extent. Pair cx/cy percentages on the same non-square viewport so exchanging axes cannot pass. Existing element unit tests are assertion pointers, not a new executed chapter pass. |
| [7.3 r](https://w3c.github.io/svgwg/svg2-draft/geometry.html#R) | Circle radius; initial 0; percentages use the normalized viewport diagonal; nonpositive used radius produces no circle. | `Length::toPixels` uses mixed extent and `ShapeSystem` suppresses nonpositive radius. A negative-only blank fixture cannot distinguish initial substitution from invalid-declaration fallback. Add computed-radius/bounds and positive-then-negative cascade controls. |
| [7.4 rx](https://w3c.github.io/svgwg/svg2-draft/geometry.html#RX) | Ellipse/rect radius; initial auto. Auto copies the other radius's resolved absolute length; both auto use 0. Rect rx is capped at half used width. | Automatic-radius repair is merged and present. Seven named analytic pairs pass in the completed foundation extension run; typed tests also cover fallback-axis resolution. This closes the reproduced explicit-auto crash, not all radius semantics. Rect percentage basis remains disputed below. |
| [7.5 ry](https://w3c.github.io/svgwg/svg2-draft/geometry.html#RY) | Symmetric auto rules; rect ry is capped at half used height. Its percentage table names viewport height. | Same shared helper and independent clamp. Retain one-auto/one-length and both-auto controls; add unequal radii above their two different clamps. Negative-to-auto computed state remains distinct from a coincidentally matching rendered shape. |
| [7.6 x](https://w3c.github.io/svgwg/svg2-draft/geometry.html#X) | Initial 0; width-relative percentages; applies to svg/rect/image/foreignObject. | Existing strict CSS-x precedence pair passes. Foundation deferred geometry still admits unitless CSS numbers. Separate `3bfd6270` addresses rect grammar; it does not close general cascade or other element types. ForeignObject remains a host/element gap. |
| [7.7 y](https://w3c.github.io/svgwg/svg2-draft/geometry.html#Y) | Initial 0; height-relative percentages; same element applicability. | Rect and sized-element paths use Y extent. The separate rect component tests cover y grammar, but no equivalent narrow end-to-end y result is claimed here. Add negative y, stylesheet precedence and non-square percentage controls. |
| [7.8 width/height](https://w3c.github.io/svgwg/svg2-draft/geometry.html#Sizing), applicability | CSS sizing applies to rect, image, foreignObject and outermost svg. This section excludes nested svg/use/symbol CSS sizing. | The shared sized-element property path does not establish that exclusion. Chapter 5 contains conflicting applicability language; hold that boundary open instead of silently selecting one chapter. Attributes and CSS must be distinguished in the eventual regression. |
| 7.8, auto used values | SVG uses 100%; image uses intrinsic sizing; other applicable elements use 0. ForeignObject does not shrink-wrap. | Rect/layout width and height parsers accept lengths/percentages, not explicit auto. Omission is not sufficient evidence: test a positive authored size overridden by auto. Audit nullable `isSpecified()`/value accesses before admitting auto into those paths. Outer-svg viewport controls belong with chapter 8. |
| 7.8, image sizing | Referenced dimensions/ratio and CSS default sizing determine omitted axes. | The independently reviewed chapter-12 probe has 5 passes/8 failures over 13 analytic cases. A separate source issue resolves a specified percentage height with X extent and percentage width with Y extent. Add non-square raster percentage-axis controls; do not count the absolute-size intrinsic probe as that regression. |
| 7.8, constraints | Applicable min/max width/height constrain used dimensions. | No corresponding registrations or constraint pass were found in the inspected SVG path. Source candidate: one-axis min/max cases with intrinsic image ratios and SVG geometry, pinned to the applicable CSS rules. This does not authorize an unrelated HTML layout implementation. |

Primary source locations are [shape components](../donner/svg/components/shape/RectComponent.cc),
[radius resolution](../donner/svg/properties/RxRyProperties.cc),
[shape construction](../donner/svg/components/shape/ShapeSystem.cc),
[length conversion](../donner/base/Length.h), and
[sized-element layout](../donner/svg/components/layout/LayoutSystem.cc).

### Executed evidence and integration boundaries

- **Merged automatic radii:** repair merge `5e8f3e69` is an ancestor of foundation `efd169b1`.
  The completed affected run at `4e95ce40` contains 17 extension cases, including all seven
  `geometry/automatic-radii` pairs, with no failures or skips. These independently specified
  references exercise explicit rx/ry/both-auto and CSS rectangle auto; ellipse cases have a visible
  red control and add no blue geometry. This is the completed affected run, not a subsequently
  interrupted full invocation. It does not resolve rectangle percentage-radius interpretation.
- **Separate CSS-length PR [1871](https://github.com/jwmcglynn/donner/pull/1871), `3bfd6270`:**
  rect component assertions reject nonzero unitless CSS across all six rect geometry properties
  while preserving unitless zero and explicit units. End-to-end assertions are narrower: inline and
  stylesheet rect x, percentages, six zero spellings, attribute units and a valid unitless
  stroke-width control. Earlier source-bound results are 577 C++ tests, four Geode tests and three
  strict analytic pairs; the implementation/assets are unchanged at `3bfd6270`. The last change
  changes only three JSON metadata files. Its owning gates have 11 and 18 metadata unittests plus **one**
  unittest asserting all 19 manifest render cases. These are not 19 JUnit records, a fresh rerun of
  the 577 tests, foundation coverage, or a completed full-qualification claim.
- **Shared cascade remains unresolved:** two separate probe runs on the CSS-length candidate
  completed 25 cases: 13 pass and 12 fail, no skips. They expose ordering/validity/specificity and
  importance behavior, including parsed fill, not merely geometry parsing. They are coordination
  evidence for the pending shared repair. Foundation behavior is not inferred from those branch
  results, and the grammar PR does not claim to fix them.

### Shared requirements still needing evidence

| Surface | Inspected implementation | Narrow next assertions |
| --- | --- | --- |
| Attribute versus CSS grammar | Foundation computed rect/circle/ellipse and sized-element parsers use `AllowUserUnits`; the separate rect fix uses CSS grammar and recognizes numerical zero. | Preserve valid attribute numbers and CSS zero while rejecting nonzero unitless CSS on each applicable element/property. `stroke-width` explicitly admits numbers and must remain a positive control. |
| Cascade validity and priority | Deferred declarations are selected before element parsing; the unresolved probe identifies first-entry retention and shared importance/order issues. | Valid/invalid in both source orders, specificity, important/normal, inline versus stylesheet, multiple style elements and mutation. Do not repair by blind last-write replacement that loses a valid fallback. |
| Inheritance and computed values | Native properties have non-inherited defaults; `PropertyRegistry::inheritFrom` does not inherit deferred properties. | Separate implicit noninheritance from explicit inherit, initial, unset and revert. Resolve relative units using the right parent/root font metrics and preserve percentage computed values. Compare typed geometry with pixels. |
| CSS math | The inspected length-percentage parser accepts a single numeric/dimension/percentage token, not a function expression. | Source candidate: positive mixed length/percentage `calc()` with an independent expected value, followed by invalid-expression and out-of-range controls after resolving the range question below. Do not import Level 4-only functions as requirements. |
| Animation | Property tables specify computed-value interpolation, while supported animation methods are a separate product contract. | Coordination only: animation samples must retain their origin below author-important declarations and preserve explicit native override behavior. Integrate source-bound native clock tests for mixed units, percentage resolution, auto transitions, base restoration and computed queries. No unmerged animation results are credited to this foundation. |

### Specification and dependency boundaries

The bibliography-selected dependencies remain [CSS Values 3 CR 2018-08-14](https://www.w3.org/TR/2018/CR-css-values-3-20180814/),
[Cascade 4 CR 2016-01-14](https://www.w3.org/TR/2016/CR-css-cascade-4-20160114/),
[CSS2.1 REC 2011-06-07](https://www.w3.org/TR/2011/REC-CSS2-20110607/), and
[CSS Images 3 CR 2012-04-17](https://www.w3.org/TR/2012/CR-css3-images-20120417/) for image sizing.
Use their exact sections: Values 4.3-5 and 8.1 (percentages, dimensions, lengths, math); Cascade 5-7
(filtering, ranking and defaulting); CSS2.1 4.2 and 10.2/10.4/10.5/10.7 (invalid syntax and sizing);
Images 5.1-5.3 (intrinsic/default sizing). SVG2 4.2/6.6 governs presentation-attribute parsing and
cascade position; 8.9 defines viewport percentage bases. No newer undated alias silently replaces
these editions.

Keep these interpretation questions visible:

- Chapter 7's current negative-value substitution is a specific rule for literal negative geometry
  values. It must not be replaced with generic invalid-declaration fallback. CSS Values 3's
  `calc()` range-clamping rule is a different mechanism; negative calculated width versus initial
  auto needs explicit reconciliation before an oracle is chosen.
- Chapter 7 assigns rect rx/ry percentages to viewport dimensions, while chapter 10's rect algorithm
  uses the rectangle's used width/height. The merged auto fix deliberately avoids this conflict.
- Chapter 7 excludes nested svg/use/symbol CSS width/height while chapter 5 retains geometry-property
  wording. Its negative-size summary narrows the SVG case to outermost svg, while the auto table
  simply says svg. Do not broaden a fix across that boundary without resolving applicability.
- The rx/ry tables list computed length/percentage but their prose explicitly uses computed auto.
  Preserve auto in the data model; do not use the abbreviated table to erase it. The negative table
  omits x/y from its coordinate row, but the preceding prose expressly allows both.

This chapter review adds no tests or implementation. It closes the local requirement-reading slice
while preserving the actual gaps, conflicts and integration boundaries.

## Chapter 12: embedded-content requirement review

This section completes the local chapter reading against the current pinned `fc7791bd` draft
(`66eb6613` publication bytes). All seven subsection headings, both element summaries, six tables
and both interfaces were inspected. Three tables are historical requirement/resolution annotations;
the other three define `crossorigin`, `referrerpolicy` and `href`. The annotations are not extra
unconditional requirements. The two interface declarations and the image URI-reference mixin agree
with Appendix A. **Implementation, test coverage and host applicability remain open.** This does not
supersede the ledger's incomplete-audit warning.

Implementation observations below are bound to Donner `efd169b1`; the inspected production files
are unchanged from renderer source `4e95ce40`. Existing unit-test pointers describe assertions read
in source, not newly executed passes. A resvg category or its historical reference image is not a
normative oracle. The main source locations are [image API](../donner/svg/SVGImageElement.cc),
[layout](../donner/svg/components/layout/LayoutSystem.cc),
[resource loading](../donner/svg/components/resources/ResourceManagerContext.cc),
[image decoding](../donner/svg/resources/ImageLoader.cc),
[rendering](../donner/svg/renderer/RendererDriver.cc), and
[request interface](../donner/svg/resources/ResourceLoaderInterface.h).

### Placement and image rendering

| Current section / assertion | Implementation and evidence | Remaining disposition |
| --- | --- | --- |
| [12.1](https://w3c.github.io/svgwg/svg2-draft/embedded.html#embedded-Overview), definitions and resource hints | `image` is recognized; `foreignObject` has no dedicated element type. The Resource Hints sentence describes compatibility, not a new mandatory prefetch operation. | Keep image and foreign-content host scope distinct; do not add fetching solely for this sentence. |
| [12.2](https://w3c.github.io/svgwg/svg2-draft/embedded.html#Placement), geometry and bounds | `LayoutSystem::calculateSizedElementBounds` supplies the positioning rectangle; `SVGImageElementTests` reads parsed lengths/percentages. | `missing-test`: computed bounds after CSS cascade, transforms and overflow, separate from visible ink bounds. Negative/explicit-auto cases depend on chapter 7. |
| 12.2, intrinsic/automatic sizing | A local analytic probe completed 13 cases on TinySkia: **5 strict passes, 8 failures, no skips or process errors**. Explicit SVG sizes, omitted raster size and zero-width control pass. Omitted/one-axis SVG sizes and no-ratio fitting fail. `ResourceManagerContext::getImageSize` sees raster metadata only; `RendererDriver::drawAdmittedSubDocument` uses concrete child canvas size as an intrinsic ratio. | `covered-fail` for these bounded assertions; implement optional intrinsic metadata and correct viewport sizing. Add non-2:1 ratio-only, explicit `auto`, percentage and shared-child restoration controls. The probe is diagnostic evidence, not 13 additional registered suite cases. |
| 12.2, zero dimensions | Analytic zero-width SVG image paints nothing; existing resvg zero-size cases provide additional comparison inputs. | Zero height and decoded-but-suppressed resource assertions remain distinct tests. Do not equate a failed load with correct zero-size handling. |
| 12.2 and [12.3](https://w3c.github.io/svgwg/svg2-draft/embedded.html#ImageElement), fitting and positioning order | No `object-fit`/`object-position` consumers were found in the inspected property/render path. Existing `preserveAspectRatio` API tests inspect values, not CSS fitting. | Source candidate: test fill/contain/cover/none/scale-down and noncenter position, with independent positioning, concrete-object and rendering rectangles. Apply SVG PAR after fitting; `none` and absent intrinsic ratio need separate controls. |
| 12.3, referenced root PAR | `drawAdmittedSubDocument` prepares an ordinary child and composes its canvas transform; no explicit referenced-root PAR suppression was found. | Source candidate: same child with mismatched absolute-size/viewBox ratios and two root-PAR values, while host image PAR stays fixed. A passing equal-ratio case is insufficient. |
| 12.2-3, overflow and two clips | UA CSS sets image overflow hidden; raster and SVG drawing paths also push an unconditional image clip. | Source candidate: author-visible versus hidden host overflow, crossed with visible/hidden child-root overflow. Check host positioning clip separately from child viewport clip. |
| 12.3, formats and channels | `ImageLoader` recognizes PNG/JPEG and SVG content and requests four decoded channels. `LoadsPngDataUrl` checks dimensions and byte count; SVG routing tests check returned bytes. Existing campaign images successfully exercise local raster/SVG loading. | `missing-test`: independent RGB and grayscale PNG/JPEG channel/alpha assertions. Decoder presence and a successful load do not establish complete format conformance. |
| 12.3, independent secure subdocuments | The default `SVGParser` child callback creates a separate secure-static document. Secure modes currently return early in `ResourceManagerContext::loadResources`. | Test no host fill/font inheritance, denied external calls, allowed embedded data resources and inert embedded events. Secure-animated selection belongs with the animation implementation. Existing secure-data repair work is separate from this source baseline. |
| 12.3, SVG views and raster fragments | No image-specific view/media-fragment selection pipeline was identified in `ImageLoader`/`UrlLoader`; an image differs from an arbitrary-element `use` reference. | Source candidate: named SVG view and in-bounds pixel `xywh` crop with changed ratio. Add percent-encoding, malformed and out-of-range cases only after resolving the cited dependency's edge arithmetic. Crosslink chapter 16 rather than duplicating its URL algorithms. |
| 12.3, same-file/target-only image | `LoadImageEntity` returns for href beginning with `#`. This does not implement the chapter's independent same-document image example. | Source candidate: filename and target-only variants, exactly bounded nesting and loader-call assertions. Reconcile secure-mode same-document/data exceptions with chapter 2; do not relax resource restrictions merely to match a recursive golden. |
| Image element summary | `SVGImageElement::CreateOn` prevents child render traversal. The summary also permits descriptive/animation/style/script/clip/mask children and shared attribute families. | Child nonpainting does not prove metadata, style, referenced-effect, event or animation semantics. Those obligations remain crosslinked to chapters 5/6/13/15 and Appendix D; no-script mode must be tested rather than treated as a blanket exemption. |

The intrinsic probe used a 400x200 white host, an image origin of (10,10), pinned solid-red local
resources, and independently encoded PNG rectangles. The same 40x20 SVG paints correctly with
explicit image dimensions but expands to the inherited viewport when they are omitted. One-axis
cases center the correctly shaped content inside the wrong rectangle. A child with neither size
nor viewBox is incorrectly treated as square even inside an explicitly sized 300x150 image.
The renderer was SHA-256 `433b500fd3ef76a169149a1e4839e7cd851997d766cf2a66d6bcc30494083762`;
authenticated GoogleTest XML was `1291962fc0f0cbd6c32eef40ca5bb89f51673bec62ef663021b12b4b34d13d89`.
No upstream undefined-behavior golden was used.

### Attributes, host integration and interfaces

| Current surface | Implementation and evidence | Remaining disposition |
| --- | --- | --- |
| [12.3 href table](https://w3c.github.io/svgwg/svg2-draft/embedded.html#ImageElementHrefAttribute): absent initial value; animatable URL | `ParseHref` and `ParseXlinkHref` test each spelling separately. `ImageHrefChangeInvalidatesTinySkiaPremultipliedImageCache` compares a mutated raster image against a fresh render. | `missing-test`: empty-present versus absent, href precedence/namespace/base URL, SVG-child invalidation, and supported native animation. Existing mutation coverage is not URL reflection or timing coverage. |
| [12.3 crossorigin table](https://w3c.github.io/svgwg/svg2-draft/embedded.html#ImageElementCrossoriginAttribute): HTML states; animatable | No image-specific API/request-state binding was found. `ResourceLoaderInterface` receives only a URL. | Host decision pending. For a network-capable profile, test absent/empty/invalid/anonymous/use-credentials states with fake requests and responses, including request mode, credentials and origin outcome. An offline loader is not evidence of CORS implementation. |
| [12.3 referrerpolicy table](https://w3c.github.io/svgwg/svg2-draft/embedded.html#ImageElementReferrerpolicyAttribute): no initial attribute; not animatable | Current draft adds the attribute and `referrerPolicy` member. No matching request metadata was found. | Host decision pending. Test missing/invalid/known policy state, request propagation and restricted reflection without real outbound requests. Keep the dated policy reference and living HTML/Fetch default selection explicit. |
| [12.4 foreignObject](https://w3c.github.io/svgwg/svg2-draft/embedded.html#ForeignObjectElement): containing block and SVG effects | It is not in `ElementType`; unknown elements are nonrenderable. No foreign layout/effects adapter was found. | Arbitrary foreign-language rendering, including HTML/MathML, is expressly not universally required here. If supported, test transformed containing-block scale, positioned descendants, overflow, filters, clips, masks and compositing. CSS top/margin do not position the outer SVG element. |
| 12.4, namespaces and nested SVG | The XML parser retains foreign nodes as unknown. It is not the HTML parser's foreignObject integration-point algorithm. | HTML input scope pending. Separately test forbidden direct SVG children versus a valid nested SVG fragment; suppressing every subtree does not establish the latter behavior. |
| 12.4, fallback | `SVGSwitchElementTests::RendersOnlyFirstMatchingChild` and `SkipsChildWithFailingRequiredExtensions` inspect rendered fallback behavior. | Add a foreignObject-specific supported/unsupported alternative pair. SVG fallback remains required even when foreign rendering is unsupported. The draft does not prescribe a requiredExtensions-to-language registry. |
| [12.5.1 image IDL](https://w3c.github.io/svgwg/svg2-draft/embedded.html#InterfaceSVGImageElement) | Reviewed all seven own members: x/y/width/height, preserveAspectRatio, crossOrigin and referrerPolicy; also graphics inheritance and SVGURIReference. Native geometry/PAR getters return values rather than the five specified SameObject animated wrappers. | DOM profile pending. Test computed geometry reflection separately from authored values, stable wrapper identity and delegated baseVal/animVal semantics. Nullable crossOrigin and known-value-only referrerPolicy reflection are separate obligations. C++ setters do not contradict a readonly wrapper attribute by themselves. |
| [12.5.2 foreignObject IDL](https://w3c.github.io/svgwg/svg2-draft/embedded.html#InterfaceSVGForeignObjectElement) | All four SameObject animated geometry members and graphics inheritance reviewed; no corresponding native element API exists. | DOM/host profile pending. Record Window exposure and live identity/reflection requirements if that binding is promised; native rendering alone does not prove them. |

### Dependency and applicability decisions

The sizing/fitting dependency is **CSS Images 3 CR 2012-04-17**, sections 5.1-5.3 and 5.5-5.6,
as identified by SVG2's bibliography. CSS2.1 REC 2011-06-07 supplies initial sizing, containing-block,
positioning and overflow definitions. SVG2's explicit image-overflow rule takes precedence over the
2012 CSS Images statement that ordinary replaced elements always clip. Its object-position grammar
links to Values 3 WD 2012-03-08; reconcile edge syntax with SVG2's direct Values 3 CR 2018 dependency
before choosing a grammar oracle.

The image fragment dependency is Media Fragments REC 2012-09-25, sections 4.2.2, 5.1.1-5.1.2,
6.1.2, 6.2.3 and 6.3.3. Its dated examples contain apparent percentage and vertical-clamping arithmetic
inconsistencies; they are an explicit interpretation question, not permission to invent an oracle.
Start with an in-bounds integer-pixel crop.

Referrer Policy CR 2017-01-26 and WebIDL REC 2016-12-15 retain their bibliography editions. The directly
linked HTML CORS/referrer attributes, reflection, image request construction and foreign-content
parsing, plus Fetch request state and URL parsing, were read as separately hashed living snapshots.
This review identifies the relevant contracts; it does not certify all transitive HTML/Fetch algorithms
or select a network/DOM host profile. Resource Hints, illustrative foreign vocabularies and historical
working-group resolution tables do not justify unrequested fetching or an HTML engine.

The native static profile still owes image pixels, resource isolation and applicable SVG fallback.
Network request semantics, HTML parsing, foreign layout, Window bindings and supported animation
need explicit product decisions and scoped evidence. None is waived solely because the harness
contains no JavaScript. These open decisions and the reproduced sizing failures prevent a complete
chapter conformance claim.

## Completion Criteria

The complete audit requires section-level assertions for all applicable requirements, reviewed
processing-mode/class scope, index reconciliation, external dependency pins for used behavior, and
implementation/test evidence. It must retain unresolved ambiguity and missing evidence. Passing a
static corpus, mechanically extracting all headings, or importing historical requirement JSON does
not satisfy those criteria. The next audited slices should turn the concrete rows above into native
regressions and then continue chapter by chapter.
