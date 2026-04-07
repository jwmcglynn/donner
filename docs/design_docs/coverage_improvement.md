# Design: Test Coverage Improvement (74% → 80%, stretch 90%)

**Status:** In Progress (Round 1 complete, Round 2 in progress)
**Updated:** 2026-04-07
**Tracking:** v1.0 milestone

## Summary

After the filter support merge, Codecov reports 74.3% line coverage (down from ~82% pre-merge).
The drop comes from ~10K new lines in the filter pipeline and vendored tiny-skia filter library.
This plan restores coverage to 80%+ and charts a path to the v1.0 target of 90%.

### Round 1 Results (PR #478, merged)

- Excluded `third_party/` from coverage via `codecov.yml` and `filter_coverage.py`
- Added 228 new tests across 8 new test files and 5 expanded files (~4,600 lines)
- Codecov impact: 74.3% → 74.6% (+0.3%) — third_party exclusion not yet reflected in
  codecov baseline; local measurement shows 71.5% lines with `--config=latest_llvm`
- Note: codecov.yml must be on the default branch (main) to take effect on project-level
  coverage; it landed with PR #478

### Remaining gap

~6% from undertested production code in renderer backends, text engine, and rendering context.
The largest uncovered files are RendererTinySkia.cc (2,241 lines), RendererSkia.cc (4,232 lines),
TextEngine.cc (1,715 lines), and RenderingContext.cc (1,148 lines).

## Current State

### Coverage by subsystem (estimated)

| Subsystem | Source Lines | Test Lines | Ratio | Status |
|-----------|-------------|-----------|-------|--------|
| XML parser | 1,890 | 2,294 | 121% | Excellent |
| CSS parser/cascade | 3,500 | 5,000+ | 143% | Excellent |
| Core/base utilities | 245 | 5,449 | 2225% | Excellent |
| SVG parsers | 4,133 | 3,381 | 82% | Good |
| ECS systems | 3,500 | 3,200 | 91% | Good |
| Text engine | 1,715 | 292 | **17%** | Critical gap |
| Renderer backends | 6,473 | ~850 | **13%** | Critical gap |
| RendererDriver | 2,069 | 456 | **22%** | Critical gap |
| RenderingContext | 1,148 | 286 | **25%** | Major gap |
| FilterGraphExecutor | 788 | 298 | **38%** | Moderate gap |
| SVG element classes | ~900 | ~1,200 | 133% | Good (but 12 types untested) |
| Third-party (vendored) | ~15,000 | 0 | 0% | Excluded from metrics |

### Untested SVG element types

These element types have no dedicated `*_tests.cc` file:

| Element | Source Lines | Notes |
|---------|-------------|-------|
| SVGSVGElement | 78 | Root container — viewBox, viewport |
| SVGImageElement | 73 | External image loading |
| SVGUseElement | 71 | Shadow tree instantiation |
| SVGTextPathElement | 43 | Text along paths |
| SVGStopElement | 68 | Gradient stops |
| SVGLineElement | 57 | Basic shape |
| 24× SVGFe*Element | ~14 each | Filter primitive stubs |

## Implementation Plan

### Phase 0: Exclude third-party from coverage metrics

Remove vendored `third_party/` code from the LCOV coverage denominator. This code is tested
via the resvg integration test suite, not unit tests.

- [x] Update `filter_coverage.py` to skip LCOV records for `third_party/` paths
- [ ] Verify coverage increases by ~3–4% after exclusion
- [x] Add codecov.yml with `ignore: ["third_party/**"]` for redundancy

**Expected impact:** 74% → ~77–78%. Landed in PR #478.

### Phase 1: Text engine tests (est. +2–3%)

The text engine is the largest gap in first-party code. `TextEngine.cc` (1,715 lines) has only
292 lines of tests.

- [ ] **Per-character positioning** — Test x/y/dx/dy/rotate attribute lists with multi-span
  text, verify glyph positions match expected coordinates.
- [ ] **letter-spacing / word-spacing** — Test CSS property application in both TextLayout
  (stb_truetype) and TextShaper (HarfBuzz) paths.
- [ ] **text-anchor** — Test start/middle/end alignment with known text widths.
- [ ] **dominant-baseline / alignment-baseline** — Test vertical positioning for different
  baseline values.
- [ ] **textPath sampling** — Test `PathSpline::pointAtArcLength()` with known paths, verify
  glyph rotation along curves.
- [ ] **textLength / lengthAdjust** — Test spacing vs spacingAndGlyphs stretch modes.
- [ ] **text-decoration** — Test underline/overline/line-through positioning.
- [ ] **Font matching** — Test family name resolution, weight matching, style matching via
  FontManager with registered `@font-face` rules.
- [ ] **WOFF/WOFF2** — Test font data loading paths (already partially covered by
  WoffParser_tests).

### Phase 2: Renderer tests (est. +2–3%)

The renderer backends (RendererTinySkia.cc: 2,241 lines, RendererSkia.cc: 4,232 lines) are
primarily tested via golden image comparisons. Add targeted unit tests for specific code paths.

- [ ] **RendererDriver paint resolution** — Test fill/stroke paint resolution for solid colors,
  gradients, patterns, and context-fill/context-stroke. Verify correct paint parameters
  are passed to the backend.
- [ ] **Coordinate transforms** — Test viewBox → viewport → device pixel transform chain.
  Test nested SVG coordinate resolution.
- [ ] **Clip path application** — Test clip path intersection with various geometries.
- [ ] **Mask rendering** — Test mask-on-mask composition, mask luminance computation.
- [ ] **Isolated layer compositing** — Test opacity, mix-blend-mode layer bracketing.
- [ ] **RenderingContext state** — Test render tree instantiation, dirty flag fast path,
  computed component creation.

### Phase 3: Filter executor tests (est. +1%)

Expand `FilterGraphExecutor_tests.cc` (298 lines) to cover more graph topologies.

- [ ] **Multi-node chains** — 3+ nodes with named buffer routing.
- [ ] **Color space transitions** — Graphs mixing sRGB and linearRGB primitives.
- [ ] **Filter region edge cases** — Oversized regions, zero-size regions, rotated regions.
- [ ] **feImage fragment refs** — Test same-document element references via the executor.
- [ ] **CSS shorthand functions** — Test `blur()`, `brightness()`, `grayscale()`, etc.
  through the filter graph construction path.
- [ ] **Error/edge paths** — Empty graph, invalid inputs, missing named buffers.

### Phase 4: SVG element tests (est. +1–2%)

Add dedicated test files for untested element types.

- [ ] **SVGSVGElement_tests.cc** — viewBox parsing, preserveAspectRatio, width/height
  attribute handling, nested SVG viewport computation.
- [ ] **SVGImageElement_tests.cc** — href attribute, image loading callbacks, aspect ratio.
- [ ] **SVGUseElement_tests.cc** — href resolution, shadow tree creation, attribute
  inheritance, cycle detection.
- [ ] **SVGTextPathElement_tests.cc** — href, startOffset, method, side, spacing attributes.
- [ ] **SVGStopElement_tests.cc** — offset, stop-color, stop-opacity attributes.
- [ ] **SVGLineElement_tests.cc** — x1, y1, x2, y2 attributes, computed path.

### Phase 5: Secondary gaps (est. +1%)

Lower-priority files with moderate gaps.

- [ ] **FontMetadata.cc** (136 lines) — Font metadata extraction tests.
- [ ] **RendererUtils.cc** — Utility functions used by both backends.
- [ ] **Property resolution edge cases** — CSS shorthand expansion, inheritance across
  shadow tree boundaries, `!important` in presentation attributes.
- [ ] **Incremental invalidation paths** — Test dirty flag propagation, selective
  recomputation in StyleSystem, fast path verification.

### Phase 6: Long-tail coverage (stretch goal: 90%)

After Phases 0–5 achieve 80%+, these items push toward 90%:

- [ ] **Renderer backend error paths** — OOM conditions, invalid surface creation, corrupt
  image data handling.
- [ ] **Parser error recovery** — Malformed SVG/CSS input that triggers specific error paths.
  Leverage fuzzer corpus for inspiration.
- [ ] **Rare element combinations** — Nested patterns, recursive use references, filter on
  clipPath content, mask with gradient fill.
- [ ] **Platform-specific paths** — macOS vs Linux font loading differences, Skia-specific
  fallbacks.
- [ ] **Debug/diagnostic code** — `operator<<` formatters, verbose logging paths. Mark
  genuinely untestable paths with LCOV_EXCL.
- [ ] **DOM mutation edge cases** — appendChild to self, removeChild of non-child, replaceChild
  with the same node, setAttribute with empty value.
- [ ] **FE element attribute tests** — All 24 filter element types have stub implementations
  (~14 lines each). Add attribute roundtrip tests for each.

## Coverage Targets

| Milestone | Target | Strategy |
|-----------|--------|----------|
| Immediate (Phase 0) | 77–78% | Exclude third_party |
| v0.5 release | 80%+ | Phases 0–2 |
| v1.0 release | 90% | All phases |

## Priority Order

| Phase | Impact | Effort | Priority |
|-------|--------|--------|----------|
| Phase 0 (third_party exclusion) | +3–4% | 1 hour | Immediate |
| Phase 1 (text engine) | +2–3% | 3–5 days | High |
| Phase 2 (renderer) | +2–3% | 3–5 days | High |
| Phase 3 (filter executor) | +1% | 1–2 days | Medium |
| Phase 4 (SVG elements) | +1–2% | 2–3 days | Medium |
| Phase 5 (secondary gaps) | +1% | 2–3 days | Low |
| Phase 6 (stretch to 90%) | +5–8% | 2–3 weeks | v1.0 |

## Running Coverage

```bash
# Full coverage run (Linux CI only — requires --config=latest_llvm)
./tools/coverage.sh

# View HTML report
open coverage-report/index.html

# Scoped to a module (faster iteration)
./tools/coverage.sh //donner/svg/components/text/...
```

## Validation

- Coverage ≥80% on Codecov after Phases 0–2
- No test regressions
- New tests use project conventions (`donner_cc_test`, gTest/gMock)
- Each new test file has a corresponding fuzzer where applicable
- Test across all build configs: default, `--config=text-full`, `--config=skia`
