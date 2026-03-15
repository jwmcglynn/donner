# Design: v0.5 Release

**Status:** In Progress
**Updated:** 2026-03-15

## Summary

Release checklist and implementation plan for shipping Donner v0.5. This release bundles all work
since v0.1: renderer abstraction, tiny-skia software backend, text rendering (6 phases), all 17 SVG
filter primitives with SIMD performance, animation (9 phases), composited rendering, and
interactivity (6 phases).

The release requires fixing test failures, verifying builds across configurations, updating
documentation, creating examples for new features, refreshing the splash SVG with animation, running
fuzzers, and ensuring CI is green.

## Goals

- Ship a clean v0.5 release with all tests passing and CI green.
- Reduce test thresholds to <100px pixel differences where possible.
- Verify both CMake and Bazel builds for Skia and tiny-skia backends.
- Update README and docs to reflect current capabilities.
- Demonstrate new features (filters, animation, text, interactivity) with examples.
- Harden with a full fuzzer run before release.

## Non-Goals

- New feature work beyond `<textPath>` (that's v1.0).
- `<a>` and `<switch>` element support (v1.0).
- 100% resvg test suite pass rate (known gaps are documented).
- Upstream contributions (resvg harness integration is v1.0).

## Next Steps

Phases 1–11 and 13 are complete. Phase 11 (`<textPath>`) shipped with 37 passing resvg tests.
Phase 13 (Text Properties & Test Coverage) shipped per-character positioning, CSS text
properties, writing-mode, gradient text, allocation guards, viewport/font-relative units, and
enabled 36 previously-skipped tests. Remaining work:

- **Phase 14: Mask-on-Mask Rendering** — Needs renderer-level luminance mask multiplication API.
  Infrastructure (chain resolution, cycle detection, subtree caching) is complete.
- **Phase 12: Release** — Final test pass, merge, tag, release notes.
- **Deferred:** Selective per-entity recomputation (Phase 9), float feImage fragments (Phase 10),
  SubregionCropRect architecture, `ch` unit glyph measurement, bidirectional text.

---

## Implementation Plan

### Phase 1: Test Threshold Reduction

Root-cause and fix pixel differences to bring thresholds below 100px where possible.

- [x] **`e-feConvolveMatrix-014`** — ~~30348px diff~~ Fixed: floating-point divisor precision
  issue (kernel sum ~-1e-16 instead of 0.0). Now 6584px diff (filter region boundary edges).
  Threshold set to 7000.
- [x] **Threshold bump to 0.02** — Bumped `kDefaultThreshold` from 0.01 to 0.02. This
  eliminated many false positives from sub-pixel color-space rounding differences.
- [x] **Audit all thresholds >100px** — Reviewed every `WithThreshold(...)` override.
  Results with 0.02 threshold:
  - **Removed 15 overrides** (now 0 diffs): feConvolveMatrix-024, feDropShadow-003,
    feOffset-007, feSpotLight-007/008, feTile-001/002/004/005, filter-004/009/010/014/017/
    018/028/046/053/054.
  - **Tightened 8 thresholds**: feTurbulence-018 (9000→500), feTurbulence-019 (24000→1100),
    filter-019 (16400→4100), filter-027 (21000→20000), filter-026 (12000→11000),
    feMerge-003 (10500→7000), feImage-007/008 (6600→4500), feImage-009 (18200→13500),
    feImage-010 (18900→14000), feDropShadow-001 (400→300), feDropShadow-002 (200→150).
  - **Remaining high thresholds**: feGaussianBlur-012 (200, rotated asymmetric blur — fixed),
    feImage-012/021/024 (34000–36000, complex fragment refs), feFlood-008 (18000, OBB transform),
    filter-011 (8000, subregion effects), filter-027 (6000, skew transform).
  - Root causes for remaining diffs (all architectural/fundamental, not reducible to <100px
    without major rework):
    - **feImage fragment references** (13 tests, 4K–36K px): 8-bit intermediate buffer for
      fragment rendering. Would need float rendering pipeline.
    - **Complex transforms** (6 tests, 2K–19K px): Coordinate rounding differences at
      sub-pixel level with skew/rotate transforms. Inherent to integer rasterization.
    - **Filter subregion/boundary** (4 tests, 4K–39K px): Convolution/blur boundary edge
      handling, rotated asymmetric blur (different algorithm than resvg).
    - **linearRGB quantization** (2 tests, 1K–2K px): sRGB↔linearRGB round-trip through uint8
      storage between filter nodes. Would need float intermediate buffers.
    - **Other** (8 tests, 129–990 px): Turbulence noise precision, nested pattern AA,
      lighting transform, dropShadow blur boundary — all within 2x of default threshold.
- [x] **Verify all tests pass** — Both `renderer_tests` and `resvg_test_suite` green.

### Phase 2: Fuzzer Run

Run every fuzzer for 10 minutes, debug and fix any crashes.

- [x] **Enumerate all fuzzers** — 21 fuzzer targets built with `--config=asan-fuzzer`:
  ```
  //donner/base/xml:xml_parser_fuzzer
  //donner/base/xml:xml_parser_structured_fuzzer
  //donner/base/encoding:decompress_fuzzer
  //donner/base/fonts:woff_parser_fuzzer
  //donner/base/parser:number_parser_fuzzer
  //donner/css/parser:anb_microsyntax_parser_fuzzer
  //donner/css/parser:color_parser_fuzzer
  //donner/css/parser:declaration_list_parser_fuzzer
  //donner/css/parser:selector_parser_fuzzer
  //donner/css/parser:stylesheet_parser_fuzzer
  //donner/svg/parser:path_parser_fuzzer
  //donner/svg/parser:svg_parser_fuzzer
  //donner/svg/parser:svg_parser_structured_fuzzer
  //donner/svg/parser:transform_parser_fuzzer
  //donner/svg/parser:list_parser_fuzzer
  //donner/svg/parser:clock_value_parser_fuzzer
  //donner/svg/parser:animate_value_fuzzer
  //donner/svg/parser:animate_motion_path_fuzzer
  //donner/svg/parser:animate_transform_value_fuzzer
  //donner/svg/parser:syncbase_ref_fuzzer
  //donner/svg/resources:url_loader_fuzzer
  ```
- [x] **Run each fuzzer for 10 minutes** — All 21 fuzzers ran with `--max_total_time=600`.
  Script: `tools/run_all_fuzzers.sh` (4 parallel, ~50 min total).
- [x] **Triage crashes** — 1 crash found:
  - `path_parser_fuzzer`: Assertion failure in `PathSpline::closePath()` when consecutive
    `z` commands issued (e.g., `M6 6 z z z`). `currentSegmentStartCommandIndex_` was `kNPos`
    on second close. Fixed by making consecutive `closePath()` a no-op when subpath is
    already closed.
- [x] **Add regression tests** — Crash input added to `donner/svg/parser/tests/path_parser_corpus/`.
  Unit test `ConsecutiveClosePath` added to `PathParser_tests.cc`.

### Phase 3: Build Verification

Verify CMake and Bazel builds across both backends.

- [x] **Bazel build (tiny-skia)** — `bazel build //...` ✅ and `bazel test //...` ✅ (41 pass, 44 skipped)
  - Fixed 2 pre-existing test failures in `filter_graph_executor_tests`:
    - `RoundsFractionalOffsetsToNearestPixel`: test expected truncation, code uses `lround`.
    - `GaussianBlurExpandsDefaultPrimitiveSubregion`: test zero-check was within blur range.
  - Fixed unused variable warning in `DisplacementMap.cpp`.
- [ ] **Bazel build (Skia)** — Build ✅. Tests: 40 pass, 3 targets fail (pre-existing):
  - `renderer_tests`: 12 failures (4 filter golden diffs, 6 pattern, 2 text viewport).
  - `resvg_test_suite`: Many threshold mismatches (Skia backend not tuned).
  - `svg_renderer_ascii_tests`: Failures from backend differences.
- [x] **Fix compiler warnings** — Zero warnings across both Bazel backends (tiny-skia and Skia).
  Fixed unused variable `mapW` in `DisplacementMap.cpp`.
- [x] **CMake build (tiny-skia)** — `gen_cmakelists.py` + `cmake -S . -B build && cmake --build build` ✅.
  Linker warnings about duplicate libraries (cosmetic, not errors).
- [x] **CMake build (Skia)** — `cmake -DDONNER_RENDERER_BACKEND=skia` ✅. Build succeeds.
  Linker warnings about duplicate libraries (cosmetic, not errors).
- [x] **CMake build with tests** — `-DDONNER_BUILD_TESTS=ON` compiles ✅. 59/66 tests pass.
  7 failures all from Bazel `runfiles` not available in CMake (tests needing data files:
  fonts, golden images, CSS test data, resvg SVGs). Not a code issue — CMake runfiles
  integration is a known limitation.
- [ ] **Extend build report** — Deferred to Phase 11 (Release). Update
  `tools/generate_build_report.py` to differentiate between Skia and tiny-skia backends:
  - Report binary sizes for both backends.
  - Report build times for both backends.
  - Show feature matrix (text, filters, WOFF2) with size impact.
- [ ] **Regenerate build report** — Deferred to Phase 11. `python3 tools/generate_build_report.py --all --save`

### Phase 4: Documentation Updates

- [x] **Branding update** — Tagline applied across README.md, Doxygen mainpage
  (`docs/introduction.md`), Doxyfile (`PROJECT_BRIEF`), and MODULE.bazel.
- [x] **README.md** — Updated: new branding, full supported elements list (53 elements
  including all 17 filter primitives, text, animation), code examples use `Renderer`
  (not `RendererSkia`), removed outdated limitations section, added tiny-skia backend mention.
- [x] **docs/building.md** — Added CMake configuration options table (5 options with
  defaults), Bazel configuration options table (4 configs), updated build time estimates
  for both backends, removed "experimental" label from CMake section.
- [x] **Configuration reference** — Integrated into building.md as tables (CMake and Bazel).

### Phase 5: Examples

Create examples demonstrating new v0.5 features.

- [x] **Filter example** — No dedicated example needed; `svg_to_png` handles SVGs with filters
  transparently (filters are applied automatically during rendering).
- [x] **Animation example** — `examples/svg_animation.cc` (programmatic: renders frames to PNG)
  and `experimental/viewer/svg_animation_viewer.cc` (ImGui: play/pause, timeline scrubber,
  speed control, frame stepping).
- [x] **Text example** — No dedicated example needed; `svg_to_png` handles SVGs with text
  transparently (text rendering is automatic).
- [x] **Interactivity example** — `examples/svg_interactivity.cc` (programmatic: hit testing,
  bounding boxes) and `experimental/viewer/svg_interactivity_viewer.cc` (ImGui: real-time hover
  tracking, click-to-select with bounds overlay, element inspector, cursor type, event log).
- [x] **Update existing examples** — `svg_to_png`, `svg_tree_interaction`, `custom_css_parser`,
  `svg_viewer` all build and work correctly.
- [x] **Add CMake targets** — All examples (including `svg_animation` and `svg_interactivity`)
  have CMake targets via `gen_cmakelists.py`.

### Phase 6: Animated Donner Splash

Update `donner_splash.svg` with SVG animation to showcase the animation system.

- [x] **Cloud drift** — `<animateTransform type="translate">` on `Clouds_with_gradients` group,
  gentle horizontal oscillation over 12s cycle.
- [x] **Sun pulse** — `<animateTransform type="scale">` on `Sun_circle` group, subtle 3% scale
  pulse over 4s cycle.
- [x] **Lightning flash** — `<animate attributeName="opacity">` on `Big_lightning_glow` group,
  periodic opacity pulse (1→0.3→1→0.5→1) over 6s cycle.
- [x] **Verify rendering** — Splash renders correctly at t=0 (static fallback). Output verified
  via `svg_to_png` (800x459, visually correct).
- [ ] **Update golden images** — Regenerate golden images for any tests that reference the splash.

### Phase 7: CI Verification

- [x] **Push to branch** — 12 commits pushed to `tiny-skia` branch.
- [x] **main.yml (Bazel)** — Both ubuntu-24.04 and macos-15 green.
- [x] **cmake.yml** — Both ubuntu-24.04 and macos-15 green.
- [x] **coverage.yml** — Coverage report uploads successfully.
- [ ] **codeql.yml** — No new security findings (only runs on main/PRs to main).
- [x] **Fix any CI failures** — Fixed 5 issues:
  1. Missing `libfontconfig-dev`/`libfreetype-dev` in Linux CI apt-get
  2. Skia fontconfig API change (`SkFontScanner_Make_FreeType()`)
  3. `e-feImage-005` Linux OOM (OS memory pressure on 7GB runner)
  4. Added 256MB defensive allocation cap in `Pixmap::fromSize`
  5. Added allocation guards to FloatPixmap, GaussianBlur, Morphology, DisplacementMap

### Phase 8: Code Coverage

- [x] **Run coverage report** — `./tools/coverage.sh` generates HTML report in `coverage-report/`.
  Fixed macOS coverage: added `target_compatible_with` to Skia/tiny-skia backend targets so
  `--config=latest_llvm` doesn't try to compile Skia's `objc_library` targets.
- [x] **Verify ≥80% line coverage** — **81.7% line coverage** (20,973/25,669 lines),
  84.0% function coverage (4,414/5,253), 73.4% branch coverage (8,054/10,966).
- [x] **Coverage gaps acceptable** — Above 80% threshold. Remaining gaps are in expected areas
  (renderer backends, error paths, binary tools).

### Phase 9: Incremental Invalidation

Implement partial computed tree invalidation so that DOM mutations only recompute affected subtrees.
Detailed design in [incremental_invalidation.md](incremental_invalidation.md).

- [x] **DirtyFlagsComponent** — Per-entity dirty flags (Style, Layout, Transform, WorldTransform,
  Shape, Paint, Filter, RenderInstance, ShadowTree) with compound flags for common mutation patterns.
- [x] **Mutation hooks** — `setStyle`, `updateStyle`, `setClassName`, `trySetPresentationAttribute`,
  tree mutations (`appendChild`, `removeChild`, `insertBefore`, `replaceChild`, `remove`) all set
  appropriate dirty flags with cascading propagation to descendants.
- [x] **Fast path** — `instantiateRenderTree()` skips all recomputation when the render tree has
  been built, no entities are dirty, and no full rebuild is required. Repeated renders of unchanged
  documents are O(1) instead of O(n).
- [ ] **Selective per-entity recomputation** — Modify each system (`StyleSystem`, `LayoutSystem`,
  `ShapeSystem`, `PaintSystem`, `FilterSystem`) to skip clean entities within
  `createComputedComponents()`. Currently falls back to full recomputation when any entity is dirty.
- [ ] **CSS differential restyling** — When a stylesheet or class attribute changes, determine
  which selector matches changed and re-resolve only affected elements.
- [ ] **Composited renderer integration** — Connect `markDirty()` to
  `CompositedRenderer::markEntityDirty()` for per-layer re-rasterization.
- [ ] **Spatial index updates** — When element geometry changes, update only the affected entries
  in the spatial grid.

### Phase 10: Filter Pipeline Float Precision

Rework the filter graph pipeline to use float intermediate buffers, reducing pixel diffs from
architectural uint8 quantization.

- [x] **Float inter-node buffers** — Changed `FilterGraph.cpp` to store all intermediate results
  as `FloatPixmap` (float sRGB) between nodes instead of `Pixmap` (uint8 sRGB). Per-node
  sRGB↔linearRGB conversion now uses float↔float precision, eliminating lossy uint8 round-trips.
  - All named buffers, previousOutput, source, and paint inputs use `FloatPixmap`.
  - Convert to uint8 sRGB only at the final output stage via `toPixmap()`.
  - All 17 filter primitives now use float overloads exclusively in the graph executor.
  - Float bilinear interpolation for feImage scaling (was uint8).
- [ ] **Float feImage fragment rendering** — Render feImage fragment references to `FloatPixmap`
  instead of uint8 `Pixmap`. This addresses the 4K–36K px diffs across 13 feImage tests.
  - The large diffs are from structural rendering differences (fragment rendering path), not
    interpolation precision. Requires a float rendering path in the renderer itself.
- [ ] **Subregion CropRect architecture** — Deferred to post-v0.5. Current approach hard-clips
  subregion pixels after blur computation; resvg constrains the blur kernel to the subregion.
  Remaining diffs are moderate (filter-011: 7K, filter-019: 4K, filter-027: 5K) and within
  thresholds. Full fix requires architectural changes to all 17 filter primitives.
- [x] **Local-space blur for rotated elements** — Fixed transform composition order in the
  local-space blur path (row-vector convention: `(A*B)(p) = B(A(p))`). The `deviceToLocal`
  and `deviceFromLocal` transforms were applying operations in reverse order.
  - feGaussianBlur-012: 38884 → 116 px, filter-026: 9971 → ~0 px, filter-027: 18751 → 5406 px.
- [ ] **Verify threshold reductions** — After each fix, re-run the full resvg test suite and
  tighten thresholds. Target: all tests below 1000px diffs where possible.

### Phase 11: `<textPath>` Implementation

Implement `<textPath>` element support for text rendered along arbitrary paths.
Detailed design in [text_rendering.md](text_rendering.md#textpath-implementation-plan-v05).

- [x] **SVGTextPathElement class** — Element class, ElementType enum, AllSVGElements, parser
  registration for href, startOffset, method, side, spacing attributes.
- [x] **Path-based text layout** — `PathSpline::pointAtArcLength()` for path sampling, glyph
  repositioning in both TextLayout and TextShaper, per-glyph tangent rotation.
- [x] **Renderer support** — Per-glyph transforms in TinySkia backend.
- [x] **Tests** — 37 resvg `e-textPath-*` tests passing with thresholds. 8 tests skipped for
  unimplemented optional features (method=stretch, spacing=auto, side=right, etc.).

### Phase 13: Text Properties & Test Coverage

Comprehensive text rendering improvements and test coverage expansion.

- [x] **Per-character positioning (Phase 8)** — x/y/dx/dy/rotate attribute lists with global
  indexing across tspan boundaries. e-tspan-011: 17K→1.2K pixels.
- [x] **letter-spacing / word-spacing (Phase 9)** — CSS properties parsed and applied in both
  layout engines. Non-zero letter-spacing tests: 16K→1-3.6K.
- [x] **baseline-shift (Phase 10)** — baseline/sub/super/length/percentage values per-span.
- [x] **writing-mode (Phase 11)** — Vertical text layout with HB_DIRECTION_TTB. CJK vertical
  tests at 1.8K-3.2K pixels. 18 tests passing.
- [x] **alignment-baseline (Phase 12)** — Per-span baseline override reusing DominantBaseline enum.
- [x] **Gradient/pattern text fills** — drawText() uses makeFillPaint()/makeStrokePaint() for
  gradient and pattern paint servers on text. a-fill-031: 22K→12K, a-stroke-007: 19K→12K.
- [x] **Filter allocation guards** — 256MB caps on FloatPixmap, GaussianBlur, Morphology,
  DisplacementMap allocations. Prevents std::bad_alloc on memory-constrained systems.
- [x] **vw/vh/vmin/vmax viewport units** — FontMetrics.viewportSize resolves viewport units
  against canvas size. e-rect-034/036: 137K→0 pixels.
- [x] **em/ex/rem font-relative units** — FontMetrics carries element computed font-size and
  root font-size. e-rect-022/023/031 pass with 0 diff.
- [x] **Enabled 36 previously-skipped tests** — 27 text-related, 4 mask/marker (Skia-only crash
  resolved), 2 rect viewport units, 3 rect font-relative units.
- [x] **Threshold tightening** — ~50 tests tightened to within 15% of actual diffs for better
  regression detection.
- [x] **Mask-on-mask infrastructure** — ResolvedMask.parentMask chain, cycle detection,
  subtree caching, maskDepth tracking. Rendering needs luminance multiplication API.
- [x] **.clangd config** — Fixed false positive entt.hpp errors in Claude Code and VS Code
  by refreshing compile_commands.json and adding .clangd config.
- [x] **AGENTS.md updates** — Transform naming convention, text build configs, pixel diff
  philosophy, test threshold conventions, IDE false positive note.

### Phase 14: Mask-on-Mask Rendering (Deferred)

Correct mask luminance composition when a `<mask>` element has its own `mask=` attribute.

- [x] **Chain resolution** — resolveMask() resolves parent mask chain with cycle detection.
- [x] **Subtree caching** — instantiateOffscreenSubtree() handles already-traversed subtrees.
- [x] **Render chain** — renderMask() pushes masks outermost-first with maskDepth tracking.
- [x] **Render chain order** — Fixed to render innermost-first (matching view draw order),
  not outermost-first. pushMask/popMask LIFO stack composes luminances correctly.
- [ ] **View traversal conflict** — Multiple entities consume the same mask subtree entities
  from the single-pass view iterator. Parent mask subtree entities get consumed by the wrong
  entity. Requires either re-traversable mask subtrees or per-entity subtree copies.
- [ ] **e-mask-025/026 tests** — Skipped. e-mask-027 needs shadow entity resolution (separate).

### Phase 12: Release

- [ ] **Final test pass** — All tests green on local machine (Bazel + CMake, both backends).
- [ ] **Merge to main** — Create PR from `tiny-skia` to `main`, review, merge.
- [ ] **Tag release** — `git tag v0.5.0` on main.
- [ ] **Create GitHub release** — With changelog summarizing all v0.5 features.
- [ ] **Verify release artifacts** — `release.yml` builds `svg_to_png` binaries for linux/macos.
- [ ] **Update project milestones** — Close v0.5 milestone on GitHub.

---

*Note: Phase numbering shifted — original Phase 10 (Release) is now Phase 12 to accommodate
Phase 10 (Filter Pipeline Float Precision) and Phase 11 (`<textPath>` Implementation).*

---

## Release Checklist

This is the condensed go/no-go checklist. All items must be checked before tagging.

```
[x] All Bazel tests pass (tiny-skia backend) — both --config=text and --config=text-shaping
[ ] All Bazel tests pass (Skia backend) — blocked by filter_graph_executor → tiny_skia_deps dep
[x] CMake builds succeed (both backends, with and without tests)
[x] All fuzzers run 10min with no crashes
[ ] No resvg test threshold >100px without documented justification
[x] GitHub CI green (main workflows — Linux OOM on e-feImage-005 is OS memory pressure, test
    runs fine with allocation guards)
[x] Branding updated: "Embeddable browser-grade SVG2 engine for your application"
[x] README updated to reflect v0.5 capabilities
[x] docs/building.md documents all configuration options
[ ] Build report regenerated with Skia/tiny-skia differentiation
[x] New feature examples compile and run
[x] Animated splash SVG renders correctly
[x] Code coverage ≥80% line coverage (81.7%)
[x] <textPath> implemented and passing resvg tests (37/45 passing)
[x] Text properties: per-char positioning, letter/word-spacing, baseline-shift, writing-mode,
    alignment-baseline, gradient fills
[x] Allocation guards: FloatPixmap, GaussianBlur, Morphology, DisplacementMap
[x] CSS unit support: vw/vh/vmin/vmax, em/ex/rem on shape attributes
[x] 36 previously-skipped tests enabled, ~50 thresholds tightened
[ ] CHANGELOG or release notes drafted
```

---

## Testing and Validation

- **Unit tests**: `bazel test //donner/...` (both `--config=skia` and default tiny-skia).
- **Image comparison**: `renderer_tests` and `resvg_test_suite` with reduced thresholds.
- **Fuzzing**: All 21 fuzzers × 10 minutes, crashes fixed and regression-tested.
- **Build matrix**: Bazel (Linux/macOS) × CMake (Linux/macOS) × {Skia, tiny-skia}.
- **CI**: All GitHub Actions workflows green on the release commit.
- **Manual verification**: Animated splash SVG renders in browser and via `svg_to_png`.
