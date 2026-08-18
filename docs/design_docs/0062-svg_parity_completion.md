# Design: SVG Paint, Text, Clip, and Mask Parity Completion

**Status:** In Progress
**Author:** GPT-5.6 Sol
**Reviewed by:** GPT-5.6 Sol
**Review role:** DesignReviewBot
**Created:** 2026-08-17

## Summary

Complete the requested static SVG rendering and interaction gaps while reducing justified
exceptions in the resvg test matrix. The work covers paint and image sampling semantics,
interaction properties, rendering hints, non-scaling strokes, bidirectional and advanced text,
text clipping, nested clips, and mask edge cases.

This is an evidence-first completion program, not a rewrite. The current implementation already
honors `paint-order` on both renderers and implements the complete typed `image-rendering` value
flow. It also has partial support for `pointer-events`, `vector-effect: non-scaling-stroke`,
`textLength`, `lengthAdjust`, and several
`<textPath>` attributes. Each slice begins by running its disabled conformance cases and inspecting
the existing focused tests, then fixes the causal gap instead of adding a second implementation.

The current source-declared resvg baseline has 1,679 vendored cases, 43 cases in one disabled
category, 116 skip expressions, 78 effective render-only cases, 103 explicit pixel-budget call
sites, 36 shared golden overrides, and 6 Geode-specific golden overrides. Those numbers mix real
product gaps, deprecated features, undefined behavior, and independent reference-oracle choices.
Reducing the raw totals is useful, but correctness and classification are the actual goals.

## Goals

- Give every requested CSS or SVG feature a typed, cascaded semantic model and a real runtime
  consumer.
- Keep TinySkia and Geode on one shared interpretation of SVG and CSS values.
- Remove a resvg skip, threshold, or custom golden only after a focused test proves the behavior and
  every applicable renderer lane passes.
- Complete `image-rendering` value semantics, including non-integer scaling and the distinction
  between `pixelated`, `crisp-edges`, and quality-oriented values.
- Complete `pointer-events` hit-testing and expose resolved `cursor` behavior to interactive hosts.
- Give `color-rendering`, `shape-rendering`, `text-rendering`, and `color-interpolation` typed
  cascade and documented backend policies.
- Make `non-scaling-stroke` exact under general affine transforms, including paths, text, dashes,
  culling, clipping, and hit-testing.
- Complete bidirectional text, `textLength` and `lengthAdjust`, SVG 2 textPath features, SVG 2 text
  decoration, modern vertical writing modes, `text-orientation`, and `font-size-adjust`.
- Complete clip paths containing or applied to text, nested clip expressions, and mask unit, type,
  transform, and color-space edge cases.
- Preserve bounded processing and deterministic failure for hostile SVG, CSS, text, path, clip,
  mask, and reference inputs.

## Non-Goals

- Treating every resvg golden as normative when the SVG or CSS specification permits a different
  result or an independent browser oracle proves the reference wrong.
- Widening pixel thresholds, adding backend disables, or converting comparisons to render-only to
  make a test lane green.
- Implementing animation, scripting, SVG fonts, `enable-background`, `BackgroundImage`,
  `BackgroundAlpha`, or CSS2 `clip: rect(...)`.
- Implementing features removed from SVG 2, including `<tref>`, the `<cursor>` element,
  `glyph-orientation-horizontal`, SVG fonts, the `kerning` attribute, and alternate-glyph elements.
  Obsolete writing-mode values and `glyph-orientation-vertical` receive only compatibility aliasing
  explicitly required by SVG 2 and CSS Writing Modes, not independent legacy behavior.
- Implementing the at-risk `vector-effect` values `non-scaling-size`, `non-rotation`, or
  `fixed-position`. They parse as typed values but currently render with `none` behavior; a separate
  interoperability case is required before implementing their rendering semantics.
- Promising exact pixels for advisory rendering hints where the specification intentionally allows
  implementation choice. The selected policies and their testable effects will be documented.
- Adding unrestricted network access for any resource.

## Next Steps

- Run the currently disabled cases for existing `textLength`, `lengthAdjust`, image-sampling, clip,
  and mask machinery before editing their implementations.
- Land the property and interaction model in small slices, beginning with the already-localized
  cursor and pointer-event gaps.
- Keep the resvg catalog, feature matrix, and this checklist synchronized with each completed slice.

## Implementation Plan

- [ ] Milestone 1: Freeze the baseline and remove stale exceptions
  - [x] Correct the README, FAQ, resvg guide, resvg catalog totals, and static-subset roadmap where
        they contradict the live implementation.
  - [x] Add a generated parity report that lists every skip, render-only case, pixel budget, custom
        golden, backend mode, and reason in the requested categories.
  - [ ] Run each requested disabled case in its applicable TinySkia, text-full, and Geode modes and
        preserve the actual, expected, and diff images.
  - [ ] Enable any case that already passes without changing a threshold or golden.
  - [ ] Triage the 43 disabled `filters/filter-functions` cases separately so the dark category does
        not distort the parity baseline.
  - [x] Verify the generated counts with `//tools/resvg_parity:parity_report_tests`.
  - [ ] Run `//donner/svg/renderer/tests:resvg_test_suite` without changing comparison policy.
- [ ] Milestone 2: Complete image-sampling semantics
  - [x] Carry the complete `ImageRendering` value through image and filter commands. Preserve the
        former public boolean only as a source-compatibility input that maps to `pixelated` when no
        typed value is set.
  - [x] Implement the specified two-stage non-integer `pixelated` policy independently from
        nearest-only `crisp-edges` and smooth or quality values in TinySkia, Geode, and both
        `<feImage>` executors.
  - [ ] Resolve pixel centers and non-integer nearest-grid behavior for `<image>` and `<feImage>` on
        both renderers, then enable `painting/image-rendering/on-feImage.svg` and
        `optimizeSpeed.svg` when the independent oracle agrees.
  - [x] Validate programmatic `ImageResource` and `<feImage>` payloads against overflow-safe exact
        `width * height * 4` byte counts before premultiplication, sampling, or upload. Reject
        hostile image-sampling axes before allocation and keep non-finite transforms transparent.
  - [ ] Verify property mapping, CPU filter sampling, Geode sampling, and the resvg category with
        `//donner/svg/properties/tests:properties_tests`,
        `//donner/svg/renderer/tests:image_sampling_tests`,
        `//donner/svg/renderer/tests:filter_graph_executor_tests`,
        `:renderer_geode_golden_tests`, and `:resvg_test_suite`.
- [ ] Milestone 3: Finish existing text-length machinery
  - [x] Apply `textLength=0` spacing semantics and cover them with a focused unit test. Keep the
        resvg case disabled until its 397-pixel overlapping-glyph raster residual is independently
        classified.
  - [x] Propagate a per-span `textLength` advance through following text until the first explicit
        inline-axis position or textPath reset. A single forward carry pass bounds this work by
        runs, glyphs, inline-position entries, and mapped text bytes. Horizontal, vertical,
        mid-run reset, DOM geometry, and deterministic traversal-count tests cover the
        current-position contract. The single-tspan resvg case remains classified for a 499-pixel
        small-text raster residual.
  - [ ] Fix single-cluster spacing, per-chunk adjustment, decoration extents, and the ordering
        between length adjustment and textPath placement.
  - [ ] Parse and apply `textLength` and `lengthAdjust` on `<textPath>`.
  - [ ] Verify both length-adjust modes, single-cluster and zero targets, decoration extents, and
        path ordering with `//donner/svg/renderer/tests:text_engine_tests`,
        `:text_span_positioning_tests`, and `:resvg_test_suite`.
- [ ] Milestone 4: Make non-scaling stroke general-affine
  - [ ] Transform path or glyph centerlines into the root host canvas, stroke them there in CSS
        pixel units, and rasterize the resulting outline before presentation or device scaling.
  - [ ] Apply non-scaling stroke semantics to text and tspans, dash patterns, culling bounds,
        clipping, and pointer hit-testing.
  - [ ] Cover non-uniform scale, shear, reflection, nested viewBox transforms, singular transforms,
        paths, text, dashes, clips, culling, and hit-testing in
        `//donner/svg/renderer/tests:renderer_driver_tests`, `:renderer_regression_tests`,
        `:renderer_geode_golden_tests`, and `//donner/svg/tests:svg_tests`.
- [ ] Milestone 5: Complete pointer and cursor interaction
  - [ ] Add the missing `auto` and complete value behavior to typed `pointer-events` parsing.
  - [ ] Make pointer-event hit-testing honor visibility, paint, fill, stroke, text, images,
        clip-path, transforms, units, and non-scaling strokes.
  - [ ] Add complete typed `cursor` grammar, including keyword and URL fallback lists, cascade, and
        inheritance.
  - [ ] Expose the resolved cursor through a DOM-shaped hit result and connect it to editor and host
        cursor selection without adding paint commands.
  - [ ] Verify every pointer-events value and cursor fallback path with
        `//donner/svg/properties/tests:properties_tests`,
        `//donner/svg/renderer/tests:renderer_driver_tests`, and `//donner/svg/tests:svg_tests`.
- [ ] Milestone 6: Complete rendering hints and paint color interpolation
  - [ ] Add typed inherited values for `color-rendering`, `shape-rendering`, `text-rendering`, and
        `color-interpolation`.
  - [ ] Map shape and text hints to documented antialiasing, hinting, shaping, and precision policy
        in both backends.
  - [ ] Apply `color-interpolation` to gradients, masks, and other applicable non-filter paint
        interpolation while preserving the separate `color-interpolation-filters` property.
  - [ ] Verify typed cascade, driver policy, crisp-edge output, text-quality commands, and sRGB
        versus linearRGB interpolation with `//donner/svg/properties/tests:properties_tests`,
        `//donner/svg/renderer/tests:renderer_driver_tests`, `:renderer_tests`,
        `:renderer_geode_golden_tests`, and `:donner_svg2_suite`.
- [ ] Milestone 7: Complete clip semantics
  - [x] Use the unified rendering object bounding box for text clip units instead of path-only shape
        bounds. The transformed-text resvg case remains disabled because its vendored golden uses
        ink bounds while SVG 2 and Firefox use full glyph cells; cross-browser classification is
        still required before changing comparison policy.
  - [ ] Extend the unified rendering object bounding box through image, group, and mask unit paths.
  - [x] Add placed vector glyph outlines to clip geometry and activate the four directly targeted
        resvg text-child cases in both TinyGolden and GeodeGolden modes. Keep a text child with its
        own nested clip fail-closed until the explicit clip-expression work below can preserve that
        child's grouping.
  - [ ] Define and implement shared clip silhouettes for bitmap-only glyphs.
  - [ ] Replace the integer nested-clip layer convention with an explicit union and intersection
        expression consumed identically by TinySkia and Geode.
  - [ ] Verify text clips, objectBoundingBox transforms, sibling unions, nested intersections, and
        bitmap-glyph policy with `//donner/svg/renderer/tests:renderer_driver_tests`,
        `:renderer_tests`, `:renderer_geode_golden_tests`, and `:resvg_test_suite`.
- [ ] Milestone 8: Complete mask semantics
  - [x] Add typed `mask-type` and carry alpha versus luminance mode through the renderer interface,
        snapshots, TinySkia, and Geode. All directly targeted resvg comparisons are active.
  - [ ] Preserve transformed mask region geometry instead of reducing rotated regions to one
        axis-aligned box.
  - [ ] Resolve `maskUnits`, `maskContentUnits`, default regions, non-positive dimensions,
        self-reference, nested masks, and `color-interpolation` with bounded transforms and
        surfaces.
  - [ ] Verify alpha and luminance masks, unit systems, transforms, invalid dimensions,
        self-reference, nesting, and color space with
        `//donner/svg/renderer/tests:renderer_driver_tests`, `:renderer_error_paths_tests`,
        `:filter_graph_executor_tests`, `:renderer_geode_golden_tests`, and `:resvg_test_suite`.
- [ ] Milestone 9: Complete modern vertical text
  - [ ] Replace the `codepoint < 0x2E80` vertical-orientation heuristic with Unicode Vertical
        Orientation data and per-span itemization.
  - [ ] Add typed `text-orientation` and `font-size-adjust` behavior, including fallback-font metric
        adjustment.
  - [x] Preserve the existing normalization of obsolete writing-mode aliases into modern computed
        values; do not add a separate legacy layout path.
  - [ ] Map only the SVG 2-required `glyph-orientation-vertical` compatibility values to
        `text-orientation` and reject other legacy values; do not add independent glyph-orientation
        layout.
  - [ ] Complete `vertical-lr`, `vertical-rl`, mixed-script punctuation, dx, dy, rotation,
        decoration, and textPath interactions.
  - [ ] Verify modern modes, orientation, compatibility aliases, mixed scripts, punctuation, and
        per-span positioning with `//donner/svg/components/text:text_system_tests`,
        `//donner/svg/renderer/tests:text_engine_tests`, `:text_engine_scripted_tests`,
        `:text_span_positioning_tests`, and `:resvg_test_suite`.
- [ ] Milestone 10: Complete SVG 2 textPath
  - [ ] Carry parsed `side`, `spacing`, and `method` from `TextPathComponent` into computed text.
  - [ ] Parse the SVG 2 inline `path` attribute and resolve supported shape references through
        computed geometry.
  - [ ] Implement `side=right`, `spacing=auto`, and `method=stretch` without backend-specific text
        placement.
  - [ ] Land `side`, `spacing`, inline path, and shape references before the independent nonlinear
        `method=stretch` slice.
  - [ ] Verify each attribute, invalid combinations, text length, bidi, vertical text, and filters
        with `//donner/svg/components/text:text_system_tests`,
        `//donner/svg/renderer/tests:text_engine_tests`, `:text_engine_scripted_tests`, and
        `:resvg_test_suite`.
- [ ] Milestone 11: Complete SVG 2 text decoration
  - [ ] Replace the single decoration bitmask and paint with resolved decoration layers for line,
        style, color, thickness, and ancestor decorations.
  - [ ] Generate decoration geometry once and consume it from both text renderers.
  - [ ] Make decoration geometry participate in paint order, text length, textPath, bidi, vertical
        layout, clipping, masking, and object bounds.
  - [ ] Verify line, style, color, thickness, ancestor layers, paint order, layout interactions, and
        both renderers with `//donner/svg/renderer/tests:text_engine_tests`,
        `:text_span_positioning_tests`, `:renderer_ascii_tests`, `:renderer_geode_golden_tests`, and
        `:resvg_test_suite`.
- [ ] Milestone 12: Introduce the text-layout seam and bidirectional text last
  - [ ] Replace the one-source-span to one-render-run assumption with a text layout result that maps
        logical source ranges to multiple visual shaping runs.
  - [ ] Migrate current one-run text through that result without changing pixels, then remove the
        old parallel run and span indexing before adding bidi behavior.
  - [ ] Select a bounded, Wasm-compatible Unicode Bidirectional Algorithm implementation after
        license, dependency-size, malformed-input, and fuzzability review.
  - [ ] Add typed inherited `direction` and `unicode-bidi` values, paragraph embedding resolution,
        isolates, overrides, and visual run ordering.
  - [ ] Pass explicit script, language, and direction to HarfBuzz for each resolved run while
        preserving SVG per-character positioning and DOM indices.
  - [ ] Verify the no-pixel-change seam, embeddings, isolates, overrides, mixed spans, positioning,
        and logical DOM indices with `//donner/svg/components/text:text_system_tests`,
        `//donner/svg/renderer/tests:text_backend_tests`, `:text_engine_tests`,
        `:text_engine_scripted_tests`, `:text_span_positioning_tests`, and `:resvg_test_suite`.
- [ ] Milestone 13: Burn down exceptions and finalize documentation
  - [ ] Remove every superseded skip, threshold, backend override, and custom golden in the
        requested categories that satisfies the exception-removal gate.
  - [ ] Run the full default, tiny, text-full, Geode, resvg, and Donner SVG 2 matrices with exact
        counts and named failures.
  - [ ] Update developer documentation and generated support data from verified behavior.
  - [ ] Convert this design to an implemented summary and link the resulting developer references.

## Delivery Slices and Reversibility

Each numbered milestone above is an upper bound, not one pull request. The implementation lands as
small in-place slices with one causal behavior and its tests. No slice keeps a dead parallel path or
requires a feature flag to make the next slice possible.

- Image sampling uses the typed enum in both renderers and filter executors. The former public
  `ImageParams::imageRenderingPixelated` field remains only as a source-compatible input and maps
  to `pixelated` when the typed value is still `auto`.
- Text-length fixes modify the live `applyTextLength` stages in place. Each zero, single-cluster,
  range, decoration, and textPath-order fix is independently revertible with its red test.
- Host-space stroking replaces the scalar branches in `toStrokeParams` and cull-bounds adjustment
  only after path, text, clip, hit-test, and both backend consumers use the host-space outline.
- Pointer-events changes remain internal. The cursor public hit-result field lands only with parser,
  cascade, controller, and host-consumer tests, so reverting it removes the complete API addition.
- Typed rendering hints remove their corresponding raw-property keys only when the live driver
  consumes the typed policy. There is no typed-but-unused intermediate state.
- Explicit clip expressions replace `ResolvedClip::layer` only after both backends consume the
  expression. The old layer evaluator and its final live callers are removed in that same slice.
- Mask interface changes replace the bounds-only `pushMask` call only after TinySkia and Geode
  consume region transform, type, and color space. Revert restores the old signature and callers.
- `TextLayoutResult` first wraps the current one-run behavior with pixel identity. RendererDriver,
  TinySkia, and Geode migrate in the same slice, and their final parallel run-index callers are
  removed before bidi support lands.
- The bidi dependency is added only in the slice that calls it from the live paragraph analyzer and
  includes native, Wasm, fuzz, and malformed-input tests. Reverting that slice removes the caller,
  dependency, lock state, and build edges together.
- Unicode vertical orientation replaces the `codepoint < 0x2E80` branches in both text backends in
  one slice. The generated table and generator are removed by the same revert.
- TextPath features land as separate side, spacing, inline-path, shape-reference, and stretch
  slices. Decoration layers land afterward and remove the final backend-local decoration geometry
  callers in the same slice that activates shared geometry.

## Background

The existing [resvg feature catalog](0021-resvg_feature_gaps.md),
[SVG 2 test-suite design](0057-donner_svg2_test_suite.md), and
[static-subset roadmap](0058-svg_static_subset_roadmap.md) identify most of these gaps. Direct code
inspection changes several classifications:

- `paint-order` is complete for shapes, markers, text, and tspans on both renderers. All 14 resvg
  category cases are active.
- `image-rendering` carries the full CSS value set through `<image>` and `<feImage>` on both
  renderers. Two legacy `optimizeSpeed` reference cases retain a nearest-grid oracle disagreement.
- `non-scaling-stroke` changes output and adjusts culling, but its determinant-based scalar is exact
  only for uniform scale and rotation. Text and hit-testing need separate coverage.
- `pointer-events` affects path and link hit-testing, but its complete SVG value matrix is not
  applied to every drawable kind or clipping case.
- `cursor` has an enum but no complete property parser, cascade, hit result, or host consumer.
- `textLength`, `lengthAdjust`, and textPath `method`, `side`, and `spacing` already have parser or
  layout components. Several skips therefore represent activation bugs rather than absent features.
- Text clip children can reuse existing computed glyph paths, while nested clips need an explicit
  boolean representation to eliminate backend-specific ordering assumptions.

## Requirements and Constraints

- The public API remains DOM-shaped. New user-facing interaction APIs do not expose ECS entities,
  registries, or components.
- Property values are parsed once into typed computed style. Backends receive resolved commands,
  not raw CSS strings.
- TinySkia and Geode may use different raster algorithms, but share layout, clip expressions, mask
  transforms, paint order, color-space choices, and text geometry.
- Text-full behavior uses FreeType and HarfBuzz. The compact backend must either implement the same
  semantic layout result or report a build-time capability boundary for features it cannot shape.
- New dependency code must support C++20 integration, native and Wasm builds, deterministic builds,
  compatible licensing, and bounded malformed-input behavior.
- No feature is considered complete while its targeted resvg case remains skipped for an
  implementation reason or while one renderer silently ignores the value.

## Feature Acceptance Criteria

Every requested feature has an observable semantic contract and an owning test target. Advisory
hints may share an output policy where the specification allows user-agent choice, but the typed
value must still reach an explicit driver policy that a renderer mock can observe.

### Paint order

- `normal` and every valid partial permutation complete to the SVG 2 fill, stroke, markers order.
- Invalid tokens and duplicates follow CSS invalid-value behavior.
- Shapes, markers, text, and tspans use the same resolved order on both renderers.
- All 14 files in `painting/paint-order` stay active. Grammar is owned by
  `//donner/svg/properties/tests:properties_tests`; command order by
  `//donner/svg/renderer/tests:renderer_driver_tests` and `:renderer_ascii_tests`; pixels by
  `:resvg_test_suite` and `:renderer_geode_golden_tests`.

### Image rendering

- `auto` uses the backend's stable default smooth policy.
- `smooth`, `high-quality`, and deprecated `optimizeQuality` prohibit nearest-only sampling.
  Donner does not dynamically degrade image quality under load, so `high-quality` uses the same
  stable smooth algorithm; its distinct typed value remains observable at the driver seam.
- `crisp-edges` and deprecated `optimizeSpeed` preserve source colors without blending; nearest
  neighbor is the initial implementation.
- `pixelated` performs nearest-neighbor scaling to the nearest positive integer multiple on each
  axis, followed by smooth scaling for the remaining fractional ratio.
- The full enum reaches `<image>`, nested SVG images, patterns that sample images, and `<feImage>`.
  Property mapping is owned by `//donner/svg/properties/tests:properties_tests`; CPU sampling by
  `//donner/svg/renderer/tests:filter_graph_executor_tests`; Geode sampling by
  `:renderer_geode_golden_tests`; end-to-end cases by `:resvg_test_suite`.

### Pointer events and cursor

- `pointer-events` supports `auto`, `bounding-box`, `visiblePainted`, `visibleFill`,
  `visibleStroke`, `visible`, `painted`, `fill`, `stroke`, `all`, and `none` with the specified
  visibility and paint predicates.
- The same predicates apply to paths, text ink, images, links, clips, transforms, and
  non-scaling-stroke outlines. Clipped-out pixels never hit.
- `cursor` accepts ordered URL values with optional hotspots followed by every CSS Basic UI keyword.
  URL failure advances to the next URL or final keyword. Cascade and inheritance produce one
  resolved cursor in the DOM-shaped hit result.
- Grammar and cascade are owned by `//donner/svg/properties/tests:properties_tests`; geometry by
  `//donner/svg/renderer/tests:renderer_driver_tests`; link and cursor results by
  `//donner/svg/tests:svg_tests`.

### Rendering hints and color interpolation

- `shape-rendering:auto` and `geometricPrecision` use analytic or antialiased coverage.
  `crispEdges` and `optimizeSpeed` request binary edge coverage. The policy is per element and does
  not mutate global renderer state for siblings.
- `text-rendering:auto`, `optimizeLegibility`, and `geometricPrecision` request the full shaping and
  raster-quality policy. `optimizeSpeed` requests the fast raster policy but never skips required
  bidi, script shaping, cluster mapping, or SVG positioning. A backend may fulfill fast with the
  full policy when it has no distinct safe fast path.
- `color-rendering:auto` requests the default accurate conversion policy, `optimizeQuality`
  requests accurate conversion, and `optimizeSpeed` requests the fast conversion policy. Backends
  may produce identical pixels, but the driver command records the selected policy.
- `color-interpolation:auto` and `sRGB` interpolate applicable non-filter paint in sRGB;
  `linearRGB` uses linear-light interpolation. It remains independent from
  `color-interpolation-filters`.
- Typed cascade is owned by `//donner/svg/properties/tests:properties_tests`; policy mapping by
  `//donner/svg/renderer/tests:renderer_driver_tests`; edge and color output by `:renderer_tests`,
  `:renderer_geode_golden_tests`, and `:donner_svg2_suite`.

### Non-scaling stroke

- The host coordinate space is the root renderer canvas in CSS pixel units after all SVG element,
  nested viewport, and viewBox transforms, but before presentation zoom, device-pixel ratio, and
  backend raster scaling.
- Path and glyph centerlines are transformed forward into host space, then dashed and stroked there.
  Authored stroke widths, dash lengths, and dash offsets resolve against their owning SVG viewport
  and become host-space CSS pixel lengths without affine scale or shear.
- Fill geometry is unchanged. Stroke paint servers map onto the host-space outline through their
  existing resolved paint transforms.
- Reflection preserves geometry and cap or join behavior. A singular transform uses the finite
  forward-transformed centerline: collapsed zero-length subpaths follow normal cap rules; non-finite
  geometry is rejected. No inverse transform or determinant approximation is used.
- Culling, clipping, hit-testing, and text strokes consume the same host-space outline or its exact
  bounds. Named affine cases run in `//donner/svg/renderer/tests:renderer_driver_tests`,
  `:renderer_regression_tests`, `:renderer_geode_golden_tests`, and
  `//donner/svg/tests:svg_tests`.

### Text layout

- `direction` supports `ltr` and `rtl`. `unicode-bidi` supports `normal`, `embed`, `isolate`,
  `bidi-override`, `isolate-override`, and `plaintext` with paragraph-level UAX 9 ordering.
- Logical DOM indices, SVG x/y/dx/dy/rotate lists, selection, and hit results remain mapped through
  visual run reordering. HarfBuzz receives explicit direction, script, and language per run.
- `textLength` accepts zero and non-negative lengths. `lengthAdjust=spacing` changes eligible
  inter-cluster gaps; `spacingAndGlyphs` scales cluster advances and outlines. Adjustment occurs
  before decoration geometry and textPath placement.
- `font-size-adjust` supports the current CSS grammar: `none`, metric keywords, `from-font`, and
  non-negative numbers. Used font size changes while computed font size and numeric line-height
  semantics remain unchanged.
- `vertical-rl`, `vertical-lr`, and `text-orientation` use Unicode Vertical Orientation data rather
  than code-point ranges. Only current-spec compatibility aliases map legacy values.
- Text analysis and positioning are owned by `//donner/svg/components/text:text_system_tests`,
  `//donner/svg/renderer/tests:text_backend_tests`, `:text_engine_tests`,
  `:text_engine_scripted_tests`, and `:text_span_positioning_tests`; end-to-end output by
  `:resvg_test_suite` and `:donner_svg2_suite`.

### TextPath and text decoration

- textPath supports `side=left|right`, `method=align|stretch`, `spacing=exact|auto`, inline `path`,
  local `href`, inline-path precedence, invalid combinations, and supported shape references.
- `method=stretch` warps glyph outlines continuously along the baseline path; it is not implemented
  as one rigid glyph scale or a backend-only transform.
- Text decoration independently resolves line set (`none`, `underline`, `overline`,
  `line-through`), style (`solid`, `double`, `dotted`, `dashed`, `wavy`), color, and thickness.
  Multiple ancestor decorations remain distinct layers.
- Both renderers consume shared placed glyph and decoration geometry. Ownership is
  `//donner/svg/components/text:text_system_tests`,
  `//donner/svg/renderer/tests:text_engine_tests`, `:text_engine_scripted_tests`,
  `:renderer_ascii_tests`, `:renderer_geode_golden_tests`, and `:resvg_test_suite`.

### Clip and mask

- A clipPath unions sibling child geometry and intersects each child with its nested clip. Text
  contributes placed glyph outlines. Bitmap-only color glyphs contribute their decoded alpha
  silhouette through a shared transformed alpha-mask clip leaf consumed by both renderers.
  Object-bounding-box clips use the rendered object's text, image, group, or path bounds.
- Masks resolve default region, `maskUnits`, `maskContentUnits`, transforms, non-positive
  dimensions, self-reference, and nesting. `mask-type=alpha|luminance` and
  `color-interpolation=sRGB|linearRGB` reach both backend compositors.
- Clip ownership is `//donner/svg/renderer/tests:renderer_driver_tests`, `:renderer_tests`, and
  `:renderer_geode_golden_tests`. Mask ownership adds `:renderer_error_paths_tests` and
  `:filter_graph_executor_tests`. End-to-end cases run in `:resvg_test_suite`.

### Removed SVG 1.1 features

- `<tref>`, the `<cursor>` element, `glyph-orientation-horizontal`, SVG fonts, `kerning`,
  alternate-glyph elements, and other removed features remain intentionally unsupported.
- Their resvg cases stay explicitly classified and do not count as SVG 2 parity failures.

## Proposed Architecture

All features follow one shared semantic pipeline:

```text
SVG attributes and CSS
        |
        v
typed PropertyRegistry values and cascade
        |
        +--> interaction policy --> hit result and cursor
        |
        +--> text analysis --> logical runs --> bidi/vertical itemization --> shaping
        |                                            |
        |                                            v
        |                                  placed glyph/decor geometry
        |
        +--> shape/image/paint policy
        |
        v
RenderingContext instances
        |
        +--> explicit clip expression
        +--> mask transform, region, type, and color space
        +--> image sampling policy
        +--> root-host-canvas stroke geometry
        |
        v
RendererInterface commands
        |
        +--> TinySkia
        +--> Geode
```

### Property and interaction model

The property registry owns grammar, inheritance, and computed values. Interaction code consumes
the same computed visibility, paint, clip, and stroke state as rendering. `cursor` is returned as
part of a DOM-facing hit result; an editor or embedder maps it to platform cursor APIs.

### Text layout result

Replace parallel run and span vectors with a `TextLayoutResult` containing visual runs, logical
source ranges, shaped clusters, placed glyphs, decoration layers, and stable DOM indices. Bidi,
vertical orientation, length adjustment, and textPath placement operate on this representation in
that order. Both renderers consume the same final glyph and decoration geometry.

### Clip and mask expressions

Represent clip content as explicit boolean groups rather than integer depth tags. A group preserves
the required union of sibling geometry and intersection with nested clips. Vector glyphs contribute
paths; bitmap-only color glyphs contribute a `ClipLeaf::AlphaMask` containing the shared decoded
alpha bitmap and its host transform. Mask commands carry the full region transform, content
transform, alpha or luminance mode, and interpolation color space.

### General-affine non-scaling strokes

The host space is the root renderer canvas in CSS pixel units, after the complete SVG transform
chain through nested viewports and viewBoxes, and before presentation zoom, device-pixel ratio, or
backend raster scaling. Transform each path or placed glyph centerline forward into this host space,
resolve dash and stroke lengths against the owning SVG viewport, and construct the stroke outline
there. Fill geometry keeps its normal path. Stroke paint transforms map onto the host-space outline.

This is not an immediate-viewport approximation and not physical device-space stroking. A nested
SVG viewport still contributes to the centerline transform, while later presentation or device
scaling scales the final CSS-pixel result normally. The forward-only construction does not require
an inverse transform. Finite collapsed geometry follows ordinary zero-length cap behavior;
non-finite geometry fails through the existing renderer error path. Culling, clipping, and
hit-testing share the resulting outline or its exact bounds.

The root-host choice is deliberate interoperability policy. Current Chrome, Firefox, and WebKit
behavior uses the outer SVG context boundary rather than the immediate nested viewport described by
literal draft wording. Preserve that decision with a three-browser reference SVG and project-owned
oracle linked to [SVG WG issue 582](https://github.com/w3c/svgwg/issues/582), plus Donner nested
viewBox, external-use, and presentation-zoom cases. A future spec-text cleanup must not silently
change this coordinate space without re-running those oracles.

## Error Handling

- Invalid property values follow normal CSS invalid-at-computed-value behavior and preserve the
  inherited or initial value as appropriate.
- Missing cursor resources fall through without aborting the document.
- Bidi controls, malformed UTF-8 replacement, recursive references, singular transforms, empty
  paths, and non-positive mask regions produce deterministic bounded results.
- Resource or geometry budget exhaustion returns the existing explicit renderer or parser error;
  it does not retry with an unbounded fallback.

## Performance

- Cache bidi paragraph analysis and shaping by text content, computed text properties, font
  selection, and relevant geometry generation.
- Keep textLength carry and logical-to-visual maps linear in source clusters, runs, and glyphs, and
  cap reference, clip-expression, mask, and placed-glyph growth.
- Avoid per-frame reparsing of property strings or rebuilding unchanged glyph outlines.
- Benchmark mixed bidi paragraphs, long textPath content, deep clip expressions, large masks, and
  non-scaling dashed paths before and after each owning slice.

## Security / Privacy

SVG, CSS, text, fonts, paths, images, masks, clips, and references are untrusted input. New work
must preserve the existing no-exception error model and resource budgets.

- `TextLayoutResourceBudget` limits one prepared text subtree to 1,000,000 source code points,
  65,536 visual runs, the UAX 9 maximum explicit embedding depth of 125, and 1,000,000 placed
  glyphs. Exhausting any dimension returns a text-preparation resource error. Small injected test
  budgets exercise each limit in `//donner/svg/renderer/tests:text_engine_scripted_tests`, and
  malformed controls run through the planned `//donner/svg/text:text_layout_fuzzer`.
- `ClipExpressionResourceBudget` limits one resolved clip to 4,096 boolean nodes, depth 64,
  1,000,000 flattened path segments, and 262,144 glyph outlines. Negative tests live in
  `//donner/svg/renderer/tests:renderer_error_paths_tests` and
  `:svg_document_render_fuzzer`.
- `MaskResourceBudget` limits nesting to 32, either surface axis to 16,384 pixels, one surface to
  16,777,216 pixels, and aggregate live mask surfaces to 67,108,864 pixels. Non-finite,
  non-positive, or over-budget regions fail before allocation. Negative tests live in
  `//donner/svg/renderer/tests:renderer_error_paths_tests` and
  `:filter_graph_executor_tests`.
- Pixelated image sampling limits a materialized intermediate to 16,384 pixels per axis and
  16,777,216 pixels total. Larger logical intermediates use the shared procedural sampler against
  the already-bounded output surface; an output above the same surface budget fails closed before
  allocation. `RendererPublicApiTest.PixelatedLargeLogicalIntermediateUsesBoundedSampler` owns the
  fallback invariant.
- Programmatic raster images and `<feImage>` primitives reach premultiplication, CPU sampling, or
  GPU upload only when their payload is exactly the overflow-safe tightly packed RGBA8 size for
  the declared dimensions. The procedural sampler rejects oversized axes before allocation and
  leaves its bounded output transparent when source, inverse, scale, or mapped coordinates are
  non-finite. `//donner/svg/renderer/tests:image_sampling_tests`,
  `:filter_graph_executor_tests`, `:renderer_public_api_tests`, and `:renderer_geode_tests` own
  these failure contracts.
- General-affine stroking retains the existing path command, subdivision, and dash-work limits.
  Non-finite transforms and expansion overflow run through `//donner/base:path_fuzzer`,
  `:path_ops_fuzzer`, `//donner/svg/parser:path_parser_fuzzer`, and
  `//donner/svg/renderer/tests:svg_document_render_fuzzer`.
- Per-span `textLength` current-position carry visits each run once and maps reset text only when an
  active carry reaches that run. `ApplyTextLengthTest.CumulativeCarryHasLinearTraversal` in
  `//donner/svg/renderer/tests:text_engine_helpers_tests` enforces the operation-count bound.
- Parser and document-render fuzz targets cover every new grammar and structured interaction.

The following CI targets enforce the relevant boundaries:

- `//donner/svg/renderer/tests:svg_document_render_fuzzer`
- `//donner/svg/parser:svg_parser_structured_fuzzer`
- `//donner/svg/parser:path_parser_fuzzer`
- `//donner/base:path_fuzzer` and `//donner/base:path_ops_fuzzer`
- the planned `//donner/svg/text:text_layout_fuzzer`
- `//donner/svg/renderer/tests:text_backend_tests` and `:text_engine_tests` for bounded shaping
  behavior
- `//donner/svg/renderer/tests:renderer_error_paths_tests` and `:filter_graph_executor_tests` for
  surface-dimension rejection

## Testing and Validation

### Exception-removal gate

A resvg exception is removed only when all three conditions hold:

1. A focused test fails on the untouched baseline and passes after the causal change.
2. The expected behavior is anchored to a normative requirement or an independent oracle.
3. The affected case passes in every applicable active comparison mode and text tier.

### Named targets

- `//donner/svg/properties/tests:properties_tests` for grammar, cascade, inheritance, and typed
  serialization.
- `//donner/svg/renderer/tests:renderer_driver_tests` for shared paint, stroke, clip, mask, image,
  and interaction command behavior.
- `//donner/svg/renderer/tests:renderer_tests` and `:renderer_regression_tests` for CPU output and
  cross-feature regressions.
- `//donner/svg/renderer/tests:renderer_geode_tests` and `:renderer_geode_golden_tests` for Geode
  command and output parity.
- `//donner/svg/renderer/tests:text_engine_tests`, `:text_engine_scripted_tests`, and
  `:text_span_positioning_tests` for logical runs, shaping, positioning, length adjustment,
  decorations, and textPath.
- `//donner/svg/renderer/tests:resvg_test_suite` for end-to-end conformance and exception removal.
- `//donner/svg/renderer/tests:donner_svg2_suite` for normative SVG 2 requirement coverage.
- `bazel test //...` plus repository lint, formatting, CMake generation, and configured build
  variants before a pull request.

### High-value conformance groups

- Paint and image: all 14 `painting/paint-order` cases and the 2 skipped
  `painting/image-rendering` cases.
- Text sizing: direct `text/textLength` and `text/lengthAdjust` skips, plus decoration and textPath
  interactions.
- Bidi: `text/direction`, `text/unicode-bidi`, mixed-direction tspan, letter-spacing, and core text
  cases.
- TextPath: `path`, `side`, `method`, `spacing`, shape reference, filter, invalid reference, and
  vertical path cases.
- Vertical: modern `vertical-rl`, `vertical-lr`, `text-orientation`, and vertical
  alignment-baseline interactions. Obsolete SVG 1.1 aliases remain intentionally classified unless
  SVG 2 or CSS Writing Modes requires a direct mapping to a modern computed value.
- Clip: text-on-clip, text-as-clip, nested children, and shorthand geometry cases.
- Mask: unit, content-unit, type, color-interpolation, transform, self-reference, and nested cases.

## Dependencies

Use SheenBidi 3.0.0 for paragraph-level UAX 9 and script-run analysis. It is a lightweight Apache
2.0 C library with UTF-8 support, no dependencies beyond the standard C library, native and Wasm
compatibility, line-level run output, mirroring, and script location. Pin the release archive and
digest in Bazel, compile only the library sources Donner calls, preserve its license, and run its
Unicode conformance data plus Donner's malformed-input fuzzer before enabling the dependency in a
live text path. The integration wraps allocation with `TextLayoutResourceBudget` preflight and
releases every SheenBidi object on all explicit error returns.

Unicode Vertical Orientation data should come from a generated, version-pinned table with a small
runtime representation and a reproducible generator or checked source provenance.

## Alternatives Considered

- **Mechanical exception removal:** rejected because skipped cases include deprecated behavior,
  undefined output, and reference-oracle defects in addition to product bugs.
- **Backend-specific CSS parsing:** rejected because it guarantees semantic drift and duplicates
  invalid-value and inheritance behavior.
- **HarfBuzz direction guessing as bidi:** rejected because shaping one guessed run does not perform
  paragraph embedding, isolates, overrides, or visual reordering.
- **FriBidi:** rejected for this integration because its LGPL license and UTF-32-oriented API add
  avoidable distribution and conversion complexity. SheenBidi supplies the required run, script,
  mirroring, UTF-8, native, and Wasm surfaces under Apache 2.0.
- **Raster text clips:** rejected because they lose scale-independent geometry and make clip output
  diverge between renderers.
- **Another nested-clip layer heuristic:** rejected because the current failure comes from an
  implicit ordering model that cannot represent sibling union plus nested intersection.
- **Scalar improvements for non-scaling stroke:** rejected as the final design because no scalar can
  preserve device-space width under arbitrary non-uniform affine transforms.

## Open Questions

- Do independent browser renders classify the two image-rendering residuals as Donner pixel-center
  defects, resvg reference-grid differences, or distinct semantics for the legacy
  `optimizeSpeed` value?
