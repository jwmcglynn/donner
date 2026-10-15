# SVG2 chapter review and test priorities

This supplement records a first reading of SVG2's chapters and their native test implications. It
is a checklist for closing gaps, not a conformance certificate. Imported standards, host behavior,
interface semantics and missing execution evidence still prevent a complete audit claim.

The target is the [SVG Working Group Editor's Draft](https://w3c.github.io/svgwg/svg2-draft/single-page.html)
retrieved on 2026-10-14: 4,905,241 bytes, SHA-256
`66eb6613c2227b0781b2aab3038657a478ee91548f7966cf4528c1bb1d92cba5`. The response matches
[publication commit 4387529e](https://raw.githubusercontent.com/w3c/svgwg/4387529e585712696584a8cd45859e3dd3f2f8d6/svg2-draft/single-page.html).
Its metadata attributes source revision
[fc7791bd](https://github.com/w3c/svgwg/tree/fc7791bd6d829cda3ed1502485c38329cbd8b00f).
This establishes publication-byte identity and upstream attribution, not an independent specification
build. The older svgwg.org response is historical evidence only; its changed requirements have been
reconciled below. See [baseline details](svg2_audit.md#source-identity).

Implementation observations refer to Donner
[24d05194](https://github.com/jwmcglynn/donner/tree/24d05194583d7d1753e23c344692b22d01d05da3).
A source candidate identifies missing or conflicting code behavior and needs a controlled native
regression before an engine fix. A test pointer identifies inspected assertions, not a passing run.
Historical executed cases and proposed changes are identified separately.

## Chapter checklist

| Chapter | First-pass scope and outstanding work |
| --- | --- |
| [1 Introduction](https://w3c.github.io/svgwg/svg2-draft/intro.html) | XML versus HTML syntax, relationship to SVG1.1/Tiny and normative terminology reviewed. Determine applicable product/conformance classes; introductory capability descriptions alone are not independent unconditional requirements. |
| [2 Conformance](https://w3c.github.io/svgwg/svg2-draft/conform.html) | Processing-mode tables, document/software classes and error handling reviewed. Test denied input/resources/animation independently. No-script testing does not waive native interaction or host obligations. XML well-formedness does not imply an unconditional DTD-validity requirement. |
| [3 Rendering](https://w3c.github.io/svgwg/svg2-draft/render.html) | Rendering tree, document paint order, atomic stacking contexts, opacity, effects, isolation and overflow reviewed. Verify actual nested-svg isolation and non-rendered subtree bounds; a clip alone does not establish an isolated backdrop. |
| [4 Types and interfaces](https://w3c.github.io/svgwg/svg2-draft/types.html) | Parsing/default/error rules, reflection, list identity, geometry queries and IDL reviewed. Native value types do not establish live WebIDL objects or host APIs. Most animVal getters alias baseVal; evaluate each interface rather than assuming a universal animated-value API. |
| [5 Structure](https://w3c.github.io/svgwg/svg2-draft/struct.html) | Element tables, use cloning/style/events/timing, conditional processing, descriptions and interfaces reviewed. Test empty requiredExtensions, shadow selector boundaries and descendant descriptive text. Language-prefix and sizing contradictions remain open. Removed use-instance accessors are not missing requirements. |
| [6 Styling](https://w3c.github.io/svgwg/svg2-draft/styling.html) | Stylesheet routes, cascade, presentation attributes, required CSS and UA rules reviewed. External CSS, media conditions and dynamic pseudo-classes need production tests. General never-rendered UA rules were removed, but specific display clauses survive elsewhere. |
| [7 Geometry](https://w3c.github.io/svgwg/svg2-draft/geometry.html) | Property tables, automatic radii, negative values and sizes reviewed. Current negative dimensions/radii substitute initial values, including CSS declarations. Nested svg/use/symbol CSS sizing conflicts with chapter 5; avoid a blanket behavior change there. |
| [8 Coordinates](https://w3c.github.io/svgwg/svg2-draft/coords.html) | Viewports, transforms, viewBox/PAR, units, bounds, intrinsic sizing, vector effects and interfaces reviewed. Negative viewBox is treated as absent; zero disables rendering. Exact bounds and zero-size behavior need semantic assertions in addition to pixels. |
| [9 Paths](https://w3c.github.io/svgwg/svg2-draft/paths.html) | Grammar, errors, directions, distances and animation rules reviewed. Add CSS path-length cascade, zero and percentage controls. Incompatible additive/from-by path animations produce no effect, including frozen state. Segment-completing close was removed. |
| [10 Shapes](https://w3c.github.io/svgwg/svg2-draft/shapes.html) | Geometry, equivalent paths, point lists and IDL reviewed. Equivalent rect/circle/ellipse paths now use ordinary closepath; marker placement still follows shape-specific vertex rules. Automatic-radii tests do not close all geometry requirements. |
| [11 Text](https://w3c.github.io/svgwg/svg2-draft/text.html) | Positioning/shaping, layout algorithms, paths, painting, CSS, selection and queries reviewed. Vertical inline-size, spacing, glyph-cell queries and selection need evidence. Shape-based wrapping properties were removed. Empty/no-valid-segment textPath path falls back to href; a valid prefix before error wins. |
| [12 Embedded content](https://w3c.github.io/svgwg/svg2-draft/embedded.html) | Image placement/overflow, intrinsic sizing, subdocuments, foreignObject and IDL reviewed. Test object-fit/position and independent clipping. Image referrerpolicy is a new host/request obligation. Arbitrary foreign rendering is not universally mandatory, but applicable fallback and restrictions remain. |
| [13 Painting](https://w3c.github.io/svgwg/svg2-draft/painting.html) | Paint grammar, stroke/dash/cap/join, markers, colors and hints reviewed. miter-clip/arcs were removed; their absence is no longer a current SVG2 gap. Marker units, calibration and color-space cases remain. Hints do not mandate a particular rasterizer's edge pixels. |
| [14 Paint servers](https://w3c.github.io/svgwg/svg2-draft/pservers.html) | Templates, gradients, stops, patterns and IDL reviewed. Test template transforms/styles, degenerate gradients, pattern sizes and PAR. Current fx/fy default is 50%, with explicit template focal values inherited before defaults. |
| [15 Interactivity](https://w3c.github.io/svgwg/svg2-draft/interact.html) | Event/hit/default-action/focus rules, pointer matrix and script interfaces reviewed. Native scenarios need target/state/action assertions, not only images. Script processing now follows HTML, including module type; no-script modes still require inertness and denied-fetch evidence. |
| [16 Linking](https://w3c.github.io/svgwg/svg2-draft/linking.html) | URLs, references, links, fragments and IDL reviewed. Preserve href presence/namespace/base semantics; test nested-link inactivity and view navigation. Current hyperlink URL algorithms import HTML behavior; a raw href string is not complete URL processing. |

## Twenty testable assertions

All paths are repository-relative and refer to the implementation baseline above. These are bounded
starting points, not an exhaustive requirement list. Pair native state/geometry assertions with
independent SVG/PNG references where the behavior has visible output. Preserve imported resvg files.

| # | Current assertion and status | Native source / existing test pointer | Minimum useful test |
| --- | --- | --- | --- |
| 1 | [2.2.1](https://w3c.github.io/svgwg/svg2-draft/conform.html#features): disabled animation means no effect, including begin-at-zero. Source candidate. | `donner/svg/core/ProcessingMode.h`; `donner/svg/components/animation/AnimationSystem.cc`. | Base-state pixels at time 0, later time, backward seek and restart; animated-mode positive control through production APIs. |
| 2 | [2.2.1](https://w3c.github.io/svgwg/svg2-draft/conform.html#features): secure resource restriction excludes data images. Proposed fix; separate use prohibition still applies. | `donner/svg/components/resources/ResourceManagerContext.cc`; `donner/svg/renderer/tests/RendererPublicApi_tests.cc`; PR1869. | Data PNG/SVG renders, forbidden external reference makes zero loader calls, subdocument mode remains restricted. |
| 3 | [5.6.4](https://w3c.github.io/svgwg/svg2-draft/struct.html#ConditionalProcessingRequiredExtensionsAttribute): present empty requiredExtensions is false. Source/test conflict. | `donner/svg/components/ConditionalProcessingComponent.cc`; `donner/svg/components/tests/ConditionalProcessingComponent_tests.cc`, `EmptyRequiredExtensionsEvaluatesToTrue`. | Direct shape and switch cases for absent/empty/whitespace/unsupported token; selected element and analytic pixels. |
| 4 | [5.4.1](https://w3c.github.io/svgwg/svg2-draft/struct.html#SymbolAttributes): transformed symbol instances follow SVG2 geometry. Legacy-oracle candidate. | `donner/svg/SVGSymbolElement.cc`; `third_party/resvg-test-suite/tests/structure/symbol/with-transform.svg`. | Separate SVG2 translated-rectangle pair; retain the imported SVG1.1 expectation unchanged. |
| 5 | [5.5.3](https://w3c.github.io/svgwg/svg2-draft/struct.html#UseStyleInheritance): ordinary selectors do not cross the use shadow boundary. Source candidate. | `donner/svg/components/style/StyleSystem.cc`; `donner/svg/components/style/tests/StyleSystem_tests.cc`. | Host `.scope rect` cannot select inside solely through host ancestry; internal cloned-group selector and inherited host color positive controls. |
| 6 | [6.3/6.9](https://w3c.github.io/svgwg/svg2-draft/styling.html#RequiredCSSFeatures): applicable external CSS imports affect rendering. Historical mismatch plus source limitation. | `donner/css/parser/StylesheetParser.cc`; `third_party/resvg-test-suite/tests/structure/style/external-CSS.svg`. | Counting local loader, import-order/color result, cycle/budget and forbidden-mode controls. |
| 7 | [4.2](https://w3c.github.io/svgwg/svg2-draft/types.html#syntax) and [7.6](https://w3c.github.io/svgwg/svg2-draft/geometry.html#XProperty): rect x has length-percentage grammar; nonzero unitless CSS is invalid, while the presentation attribute admits numbers. Source candidate. | `donner/svg/properties/PropertyRegistry.cc` stores deferred x; `donner/svg/components/shape/RectComponent.cc`, `ComputedRectComponent`, passes AllowUserUnits to `donner/svg/parser/LengthPercentageParser.cc`. | A rect with `x="20" style="x:4"` must stay at x=20; the discriminating positive control `style="x:4px"` moves it to x=4. Also test unitless attribute `x="4"` and CSS `x:0` as valid controls. Assert computed geometry and analytic pixels. |
| 8 | [4.2](https://w3c.github.io/svgwg/svg2-draft/types.html#syntax): invalid presentation value substitutes initial value with attribute priority. Source candidate. | `donner/svg/properties/PropertyRegistry.cc`, `parsePresentationAttribute`; `donner/svg/core/UserAgentStylesheet.h`. | Nested svg overflow invalid versus absent/visible/hidden, plus higher-priority author override; selected value and clipped pixels. |
| 9 | [7](https://w3c.github.io/svgwg/svg2-draft/geometry.html): negative r/sizes/radii substitute initial 0/auto in CSS and attributes. Current-draft candidate. | `donner/svg/properties/RxRyProperties.cc`; `donner/svg/components/shape/ShapeSystem.cc`; `donner/svg/components/shape/tests/ShapeSystem_tests.cc`. | Positive earlier declaration followed by negative r or rect size; negative radius versus explicit auto; retain negative coordinate controls. Exclude disputed nested/use/symbol sizing. |
| 10 | [8.6](https://w3c.github.io/svgwg/svg2-draft/coords.html#ViewBoxAttribute): negative viewBox behaves absent; zero disables rendering. Verification needed. | `donner/svg/parser/ViewBoxParser.cc`; `donner/svg/components/layout/tests/LayoutSystem_tests.cc`, `ZeroWidthViewBoxDoesNotCrash`. | Absent/negative/zero/valid both axes, actual transform and pixels. Survival alone is insufficient. |
| 11 | [9.6.2](https://w3c.github.io/svgwg/svg2-draft/paths.html#PathLengthProperty): CSS path-length overrides mapped pathLength and calibrates absolute distances; percentages exempt. Source candidate. | `donner/svg/components/PathLengthComponent.h`; `donner/svg/properties/PropertyRegistry.cc`; `donner/svg/renderer/RendererDriver.cc`. | 100-unit line with attribute 50, CSS 100px, none/zero and invalid controls; independently calculated absolute and percentage dash intervals. |
| 12 | [13.7](https://w3c.github.io/svgwg/svg2-draft/painting.html#Markers): marker stroke units use resolved stroke width. Source candidate. | `donner/svg/renderer/RendererDriver.cc`, `drawMarker`; `donner/svg/SVGMarkerElement.cc`. | 1em at known font size versus equivalent px, nonsquare percentage basis, non-scaling-stroke and transform controls. |
| 13 | [11.4.1](https://w3c.github.io/svgwg/svg2-draft/text.html#InlineSize): wrapping measure follows inline axis, including vertical writing. Source candidate. | `donner/svg/text/TextLayoutParams.h`; `donner/svg/text/TextEngine.cc`; `donner/svg/renderer/tests/TextEngine_tests.cc`, `InlineSizeWrapsIntoStackedLines`. | Horizontal/vertical pairs with absolute/percentage measure, line positions and anchor controls using deterministic glyph metrics. |
| 14 | [11.10.2.8-9](https://w3c.github.io/svgwg/svg2-draft/text.html#WordSpacingProperty): word-spacing percentage uses affected character width; letter-spacing percentage is invalid. Source candidate. | `donner/svg/properties/PropertyRegistry.cc`, `ParseSpacingValue`; `donner/svg/text/TextEngine.cc`. | Same font/text at two viewport widths; extra space remains font-relative. Earlier valid letter spacing survives invalid percentage. |
| 15 | [11.13.1](https://w3c.github.io/svgwg/svg2-draft/text.html#InterfaceSVGTextContentElement): extent/hit APIs use glyph cells, with last overlapping hit and UTF16 query indexing. Source candidate with existing UTF16 controls. | `donner/svg/text/TextEngine.cc`; `donner/svg/renderer/tests/TextEngineScripted_tests.cc`, `DomUtf16SupplementaryUnitsShareGlyphGeometry`; `donner/svg/tests/SVGTextElement_tests.cc`. | Cell-only point outside ink, rotated quad/AABB, overlap order, non-BMP cluster; preserve correct UTF16/codepoint distinction. |
| 16 | [12.2-3](https://w3c.github.io/svgwg/svg2-draft/embedded.html#Placement): image fitting and overflow distinguish positioning/rendering rectangles. Source candidate. | `donner/svg/renderer/RendererDriver.cc`; `donner/svg/components/layout/tests/LayoutSystem_tests.cc`, `ImageIntrinsicSizingBranches`. | Tiny raster/SVG with contain/cover/none and noncenter position, visible/hidden overflow, independent child-root clip controls. |
| 17 | [14.1.1](https://w3c.github.io/svgwg/svg2-draft/pservers.html#PaintServerTemplates): template transforms/styles apply before defaults. Source candidate. | `donner/svg/components/paint/GradientComponent.cc`; `donner/svg/components/paint/PatternComponent.cc`; `donner/svg/components/paint/PaintSystem.cc`; `donner/svg/SVGGradientElement.h`. | Transformed base versus explicitly transformed child; explicit identity override; one-stop currentColor distinguishes cloned stop style from original style. |
| 18 | [14.2.2.2](https://w3c.github.io/svgwg/svg2-draft/pservers.html#LinearGradientNotes): coincident endpoints paint last stop color/opacity for all spreads. Source candidate. | `donner/svg/renderer/RendererTinySkia.cc`; `donner/gpu/shader/programs/SlugGradientSource.h`. | Flat analytic PNG, pad/reflect/repeat, both backends, distinct stops and nondegenerate control. |
| 19 | [14.2.3.1](https://w3c.github.io/svgwg/svg2-draft/pservers.html#RadialGradientAttributes): absent fx/fy default 50% after template fallback, not own cx/cy. Current-draft source conflict. | `donner/svg/renderer/RendererTinySkia.cc`, radial coordinate resolution; `donner/svg/components/paint/GradientComponent.cc`. | Noncentral center, omitted versus explicit 50% focal; inherited explicit focal and own override controls. Preserve older resvg references. |
| 20 | [15.6](https://w3c.github.io/svgwg/svg2-draft/interact.html#PointerEventsProp), [11.12](https://w3c.github.io/svgwg/svg2-draft/text.html#TextSelection), [16.2](https://w3c.github.io/svgwg/svg2-draft/linking.html#Links): native input has targets and observable effects beyond pixels. Source candidates; split into separate regressions. | `donner/svg/renderer/RenderingContext.cc`; `donner/svg/DonnerController.cc`; `donner/svg/tests/DonnerController_tests.cc`, `NestedLinksResolveToInnermost`; `donner/svg/SVGTextContentElement.cc`, no-op `selectSubString`. | Paint-independent stroke hit, clip exclusion, cell/image hits; separate focus/selection/clipboard trace; nested link inactive and empty-present href distinct from absence. Use fake host navigation, no real outbound action. |

## Evidence boundaries

The historical native campaign includes **all 1,679 paired cases in the pinned resvg corpus**, plus 10
Donner extension pairs: 1,689 total. It recorded 26 strict passes, 1,661 pixel mismatches and 2 render
errors. Recomparison of the same captured PNGs with the existing resvg default comparison policy
recorded 1,367 passes and 320 mismatches, with the two errors still separate. No per-case skip or
alternate reference was applied in that recomparison. [Recorded run identity and policy](svg2_audit.md#executed-static-corpus-evidence)
bind executable hashes, declared revision/backend, resource fingerprints and error outcomes.
These are historical results, not execution on the source baseline above or proof of normative
conformance. Pixel thresholds measure comparison policy, not whether an SVG2 requirement is satisfied.

The CSS geometry x extension is a historical strict passing assertion with an independently reviewed
integer-aligned reference. It does not close geometry. Published proposals
[1866](https://github.com/jwmcglynn/donner/pull/1866) (suite),
[1867](https://github.com/jwmcglynn/donner/pull/1867) (automatic radii), and
[1869](https://github.com/jwmcglynn/donner/pull/1869) (secure data images) are separate from the inspected main baseline.
Check their actual revision, execution evidence and merge state before claiming that a
candidate gap is closed. Animation feature work likewise needs its own source-bound native scenarios.

## Specification questions and dependency work

- Current 7.8 excludes nested svg/use/symbol CSS width/height, while chapter 5 retains geometry-property
  wording and different auto-size treatment. Keep that boundary unresolved.
- Current systemLanguage algorithm and new examples use opposite prefix directions. Exact match,
  absent/empty and ASCII comparison are separable from that ambiguity.
- The general 6.8 UA display block is gone, but 6.2 style, 14.1 paint-server and 15.9 script clauses still
  require important UA display:none. Direct nonrendering remains required; do not substitute a blanket
  claim about computed display.
- SVG geometry interfaces are described as standalone while generated links still target DOM geometry
  equivalents. Remaining list/animated-interface wording needs per-interface reconciliation. Obvious
  text-layout pseudocode errors should not become test oracles without interpretation.

Appendix A's consolidated IDL changes were checked against chapter deltas; no binding conformance is
claimed. B's numerical implementation notes need independent curve oracles, not endpoint-only tests.
C accessibility and D animation retain applicable host/method obligations. E imports and their exact
editions/content remain open. F-I indexes are navigation aids; some omit new properties or retain
legacy entries, so they are not a coverage denominator. J encoding/media/security rules need actual
input/host tests. K is informative change history and cannot override operative contradictory prose.

Required dependency work includes CSS syntax/cascade/selectors/values/transforms/color/text/fonts/
writing modes/images/masking/filters/compositing, XML/namespaces, DOM/WebIDL/geometry, HTML/UI events/
clipboard/URL/fetch/referrer policy, accessibility mappings, image/font formats and supported animation
methods. Pin each dependency used by an oracle. Keep renderer, native API, host and authoring scope
explicit. A C++ gtest/pixelmatch suite can test native animation and interaction without a JavaScript
harness, while retaining honest gaps for unsupported host surfaces.
