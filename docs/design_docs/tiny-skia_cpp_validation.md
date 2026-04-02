# Design: Rust-to-C++ Translation Validation Audit

**Status:** Complete (WI-12 through WI-18 closed). Note: the C++ port has
diverged from pixel-exact parity with the Rust original due to additional
features (analytic AA rasterizer, SVG filter primitives) and SIMD optimizations.
Golden-image tests now use configurable tolerance rather than bit-exact comparison.
**Author:** Claude
**Created:** 2026-03-02
**Updated:** 2026-03-03

## Summary

Systematic audit of every C++ file against its Rust counterpart to confirm
line-by-line translation fidelity and file/module topology parity. The audit
proceeds in two phases:

1. **Phase 1 — Inventory**: file-by-file, function-by-function comparison
   cataloguing every structural divergence between Rust and C++.
2. **Phase 2 — Fixing**: ordered work items that bring each divergence to parity,
   with `bazel test //...` passing after every change.

Second-opinion review on 2026-03-03 found additional topology divergences
that were previously treated as acceptable (notably shader module collapsing
and `floating_point.rs` scattering). Those divergences are now marked as
required fixes.

The goal is that every C++ function reads as a direct, reviewable
transliteration of the corresponding Rust function, and module boundaries are
directly traceable to Rust source boundaries.

## Goals

- Produce a complete inventory of structural and semantic differences between
  the Rust source and the C++ port.
- Enforce Rust-to-C++ module/file topology parity for `tiny-skia-path` and
  shader code (no merged/scattered module boundaries).
- Identify C++ abstractions that should be introduced so the code reads more
  like the Rust original (e.g., a `U16x16` SIMD-style wrapper so lowp code
  mirrors Rust's `u16x16` usage).
- Create an ordered fix plan where each work item is independently testable.
- Maintain a green build at every step: `bazel build //...` and `bazel test //...`.

## Non-Goals

- No performance optimization beyond what is needed for translation fidelity.
- No new features or API surface changes.
- No SIMD performance work in this audit doc itself; SIMD enablement now has a
  dedicated follow-on design (`tiny-skia_cpp_cfg_if_simd.md`).
- PNG I/O remains out of scope.

## Next Steps

1. Review and approve this updated topology audit.
2. If desired, run a final `✅` parity sign-off sweep across all function-map
   files after spot-checking recent wrapper-layer additions.

## Implementation Plan

### Phase 1: Inventory (complete — findings below)

- [x] Inventory wide/ module types vs Rust `wide` crate types
- [x] Inventory pipeline/ (lowp, highp, blitter, mod) structural differences
- [x] Inventory scan/ module structural differences
- [x] Inventory core modules (color, math, fixed_point, geom, edge, etc.)
- [x] Inventory path modules (path, path_builder, stroker, dash, transform)
- [x] Inventory shader modules (gradient, linear, radial, sweep, pattern)
- [x] Inventory path64/ module structural differences
- [x] Catalogue WIP bug fixes in path64 (Cubic64, LineCubicIntersections)
- [x] Second-opinion topology audit (module/file boundary parity)

### Phase 2: Fixing (work items below)

- [x] **WI-12**: Promote function-map statuses to `🟢` after final doc/map sync
- [x] **WI-13**: Split `shaders/Mod.*` into per-Rust-module files
- [x] **WI-14**: Extract `path/floating_point.rs` equivalents into dedicated C++ module files
- [x] **WI-15**: Move `Transform` out of `pipeline/Mod.*` into dedicated transform module
- [x] **WI-16**: Split `PathVec.h` into distinct `f32x2_t`/`f32x4_t` mapped files
- [x] **WI-17**: Separate mixed path geometry ownership (`src/path_geometry.rs` vs
  `path/path_geometry.rs`) into explicit mapped files or wrappers
- [x] **WI-18**: Decompose merged path geometry/size/rect/scalar boundaries
  (`Geom.*`, `Scalar.*`, `Color.*`) into Rust-traceable module units

---

## Phase 1: Inventory Findings

### 0. Second-Opinion Topology Audit (2026-03-03)

The following topology divergences were identified by second-opinion review:

| Rust source topology | Current/previous C++ topology | Divergence type | Status |
|----------------------|------------------------------|-----------------|--------|
| `src/shaders/{gradient,linear_gradient,radial_gradient,sweep_gradient,pattern}.rs` | Previously merged in `src/tiny_skia/shaders/Mod.h/.cpp` | Module collapse | ✅ resolved by WI-13 |
| `path/src/floating_point.rs` | Previously spread across `Color.*`, `Scalar.h`, `Geom.*`, `FixedPoint.cpp` | Module scattering | ✅ resolved by WI-14 (`FloatingPoint.h/.cpp`) |
| `path/src/transform.rs` | Previously implemented in `src/tiny_skia/pipeline/Mod.*` | Cross-module placement | ✅ resolved by WI-15 (`Transform.h/.cpp`) |
| `path/src/f32x2_t.rs` + `path/src/f32x4_t.rs` | Previously combined in `src/tiny_skia/PathVec.h` | File merge | ✅ resolved by WI-16 (`F32x2.h`, `F32x4.h`) |
| `src/path_geometry.rs` + `path/src/path_geometry.rs` | Previously combined in `src/tiny_skia/PathGeometry.*` | Dual-source merge | ✅ resolved by WI-17 (`PathGeometryCoreRs.h`, `PathGeometryPathRs.h`) |
| `path/src/{rect,size,scalar}.rs` | Previously folded into shared `Geom.*`, `Math.h`, `Scalar.h`, `Color.h` | Boundary blur | ✅ resolved by WI-18 (`PathRectRs.h`, `PathSizeRs.h`, `PathScalarRs.h`) |

No remaining topology items.

### 1. Wide Module (`src/tiny_skia/wide/`)

#### 1.1 Type Representation

All C++ wide types use `std::array<T, N>` as their backing store. Rust uses
conditional compilation (`cfg_if!`) to select native SIMD types (SSE2, AVX,
NEON) or falls back to plain arrays. Since the C++ port targets scalar
fallback parity, `std::array` is the correct choice — but the API surface
must match Rust's full method set.

| Rust type | C++ type | Backing store | API parity |
|-----------|----------|---------------|------------|
| `f32x4` | `F32x4T` | `std::array<float, 4>` | Complete |
| `f32x8` | `F32x8T` | `std::array<float, 8>` | Complete |
| `f32x16` | `F32x16T` | `(F32x8T, F32x8T)` | Complete |
| `i32x4` | `I32x4T` | `std::array<int32_t, 4>` | Complete |
| `i32x8` | `I32x8T` | `std::array<int32_t, 8>` | Complete |
| `u16x16` | `U16x16T` | `std::array<uint16_t, 16>` | Complete for current tiny-skia usage |
| `u32x4` | `U32x4T` | `std::array<uint32_t, 4>` | Complete |
| `u32x8` | `U32x8T` | `std::array<uint32_t, 8>` | Complete |

Wide-module parity gaps listed in WI-01/WI-02 are resolved in code. Remaining
work in this design doc is topology alignment, not wide-lane operator coverage.

---

### 2. Pipeline Module (`src/tiny_skia/pipeline/`)

#### 2.1 Lowp Pixel Type (resolved)

| Aspect | Rust | C++ | Status |
|--------|------|-----|--------|
| Pixel type | `u16x16` (16 × `u16`) | `LowpChannel = U16x16T` | ✅ aligned |
| Value range | Integer [0, 255] | Integer [0, 255] in u16 lanes | ✅ aligned |
| div255 | `(v + 255) >> 8` (bitwise) | `(v + 255) >> 8` (bitwise) | ✅ aligned |

Lowp pixel channels now use an explicit u16-lane type in C++ and are no longer
an accepted divergence.

#### 2.2 Load/Store Patterns

| Aspect | Rust | C++ |
|--------|------|-----|
| Load input | `&[PremultipliedColorU8; 16]` structured array | Raw `uint8_t*` + stride + coordinates |
| Store output | `&mut [PremultipliedColorU8; 16]` structured array | Raw `uint8_t*` + stride + coordinates |

Rust uses typed `PremultipliedColorU8` accessors (`.red()`, `.green()`, etc.),
while C++ computes byte offsets manually. The Rust approach is more readable
and less error-prone.

#### 2.3 Tail Handling

| Aspect | Rust | C++ |
|--------|------|-----|
| Strategy | **Dual code paths**: `functions` (full) vs `functions_tail` (tail) | **Single path** with `tail` parameter |
| Dispatch | Pipeline switches between two function arrays at `start()` | Same function handles both via `count` parameter |

Rust avoids branches inside hot load/store stages by having separate function
arrays for full-width and tail-width processing. C++ uses a single function
with a `tail` parameter that controls the loop bound. This is a meaningful
structural divergence.

#### 2.4 Blend Mode Code Generation

Rust uses `blend_fn!` and `blend_fn2!` macros to generate blend mode stage
functions with a common pattern. C++ writes each function manually. The logic
is identical but the Rust macro abstraction makes patterns more visible. A C++
template or macro could improve readability.

#### 2.5 Lowp Helper Functions

| Rust helper | Purpose | C++ equivalent |
|-------------|---------|---------------|
| `split(f32x16) → (u16x16, u16x16)` | Reinterpret f32x16 as two u16x16 halves | **Not present** |
| `join(u16x16, u16x16) → f32x16` | Reinterpret two u16x16 halves as f32x16 | **Not present** |
| `mad(f, m, a)` | Multiply-add for coordinate transforms | Inline in C++ (no named helper) |
| `gather_ix()` (highp) | Compute texture gather indices | **Not present as named function** |

#### 2.6 FMA Contraction

C++ correctly disables FMA contraction (`#pragma clang fp contract(off)`) to
match Rust's software SIMD wrappers that prevent LLVM from fusing multiply-add.
This is a good alignment decision.

#### 2.7 STAGES Array

Both Rust and C++ use a flat function-pointer array indexed by `Stage` enum.
The C++ `lowp::STAGES` array had a missing `Luminosity` entry (now fixed).
Both arrays should be verified to have identical ordering and completeness.

---

### 3. Scan Module (`src/tiny_skia/scan/`)

The scan module has **complete structural parity**. All 43 functions across
5 files are marked `🟢` in the design doc.

| File | Rust functions | C++ functions | Status |
|------|---------------|---------------|--------|
| `mod.rs` / `Mod.cpp` | 2 | 2 | `🟢` all |
| `path.rs` / `Path.cpp` | 11 | 11 | `🟢` all |
| `path_aa.rs` / `PathAa.cpp` | 4 | 4 | `🟢` all |
| `hairline.rs` / `Hairline.cpp` | 13 | 13 | `🟢` all |
| `hairline_aa.rs` / `HairlineAa.cpp` | 16 | 16 | `🟢` all |

Translation patterns are consistent:
- `Option<T>` → `std::optional<T>`
- `match` → `if/else` chains
- `&mut dyn Blitter` → `Blitter&` or `Blitter*`
- Rust lifetimes → RAII / raw pointers
- Rust `impl Drop` → C++ destructors

No work items needed for this module.

---

### 4. Core Modules (`src/tiny_skia/`)

#### 4.1 Fully Verified (`🟢`) — No Changes Needed

| Module | Rust file | C++ file | Functions | Status |
|--------|-----------|----------|-----------|--------|
| AlphaRuns | `alpha_runs.rs` | `AlphaRuns.cpp` | 7 | `🟢` |
| BlendMode | `blend_mode.rs` | `BlendMode.cpp` | 2 | `🟢` |
| Blitter | `blitter.rs` | `Blitter.cpp` | 8 | `🟢` |
| Color | `color.rs` | `Color.cpp` | 30 | `🟢` |
| Edge | `edge.rs` | `Edge.cpp` | 10 | `🟢` |
| EdgeBuilder | `edge_builder.rs` | `EdgeBuilder.cpp` | 7 | `🟢` |
| EdgeClipper | `edge_clipper.rs` | `EdgeClipper.cpp` | 17 | `🟢` |
| FixedPoint | `fixed_point.rs` | `FixedPoint.cpp` | 16 | `🟢` |
| Geom | `geom.rs` | `Geom.cpp` | 20 | `🟢` |
| LineClipper | `line_clipper.rs` | `LineClipper.cpp` | 2 | `🟢` |
| Math | `math.rs` | `Math.cpp` | 4 | `🟢` |
| Mask | `mask.rs` | `Mask.cpp` | 14 | `🟢` |
| PathGeometry | `path_geometry.rs` | `PathGeometry.cpp` | 11 | `🟢` |
| Pixmap | `pixmap.rs` | `Pixmap.cpp` | 23 | `🟢` |

#### 4.2 Current Vetting Snapshot

Core, pipeline, scan, wide, and shader behavior is implemented and covered by
tests. Topology/file-boundary alignment and function-map/doc synchronization
work items (WI-12 through WI-18) are complete.

---

### 5. Path Modules (`tiny-skia-path`)

#### 5.1 Rust `f32x2_t.rs` and `f32x4_t.rs` — Resolved by Dedicated Files

Rust comment: *"Right now, there are no visible benefits of using SIMD for
f32x2/f32x4. So we don't."* These are thin wrappers over `[f32; 2]` and
`[f32; 4]` used internally by path geometry computations.

This topology divergence is now resolved by:
- `src/tiny_skia/F32x2.h`
- `src/tiny_skia/F32x4.h`

`PathVec.h` remains as a compatibility aggregator for existing includes.

#### 5.2 `floating_point.rs` — Resolved by Dedicated Module

Rust has a dedicated module with types (`NormalizedF32`, `NormalizedF32Exclusive`,
`NonZeroPositiveF32`, `FiniteF32`) and utility functions (`f32_as_2s_compliment`,
`is_denormalized`, `classify`).

This topology divergence is now resolved by:
- `src/tiny_skia/FloatingPoint.h`
- `src/tiny_skia/FloatingPoint.cpp`

Call sites now consume the shared module from `Color.*`, `Scalar.h`, `Geom.*`,
and `FixedPoint.cpp`, and saturating float/int helpers are centralized there.

#### 5.3 Path Modules — Logic Implemented, Topology Cleanup Pending

| Module | Rust file | C++ file | Status |
|--------|-----------|----------|--------|
| Path | `path.rs` | `Path.h` | Logic aligned; topology review not required |
| PathBuilder | `path_builder.rs` | `PathBuilder.cpp` | Logic aligned; topology review not required |
| Stroker | `stroker.rs` | `Stroker.cpp` | Logic aligned; topology review not required |
| Dash | `dash.rs` | `Dash.cpp` | Logic aligned; topology review not required |
| Scalar | `scalar.rs` | `PathScalarRs.h` wrapper over `Scalar.h` / `Math.h` | ✅ aligned ownership (WI-18) |
| Transform | `transform.rs` | `Transform.h/.cpp` | ✅ aligned (WI-15) |
| FloatingPoint | `floating_point.rs` | `FloatingPoint.h/.cpp` | ✅ aligned (WI-14) |
| f32x2_t | `f32x2_t.rs` | `F32x2.h` | ✅ aligned (WI-16) |
| f32x4_t | `f32x4_t.rs` | `F32x4.h` | ✅ aligned (WI-16) |
| Rect | `rect.rs` | `PathRectRs.h` wrapper over `Geom.*` | ✅ aligned ownership (WI-18) |
| Size | `size.rs` | `PathSizeRs.h` wrapper over `Geom.*` | ✅ aligned ownership (WI-18) |
| PathGeometry (path/core split) | `path_geometry.rs` + `src/path_geometry.rs` | `PathGeometry.cpp` + wrapper layers | ✅ aligned ownership (WI-17) |

---

### 6. Shader Modules (`src/tiny_skia/shaders/`)

Shader module topology is now split to match Rust source boundaries. `Mod.*`
is kept as a thin aggregation/dispatch layer.

| Rust file | Functions | C++ location | Status |
|-----------|-----------|-------------|--------|
| `shaders/mod.rs` | 5 | `shaders/Mod.h/.cpp` | ✅ aligned |
| `shaders/gradient.rs` | 4 | `shaders/Gradient.h/.cpp` | ✅ aligned |
| `shaders/linear_gradient.rs` | 4 | `shaders/LinearGradient.h/.cpp` | ✅ aligned |
| `shaders/radial_gradient.rs` | 8 | `shaders/RadialGradient.h/.cpp` | ✅ aligned |
| `shaders/sweep_gradient.rs` | 4 | `shaders/SweepGradient.h/.cpp` | ✅ aligned |
| `shaders/pattern.rs` | 4 | `shaders/Pattern.h/.cpp` | ✅ aligned |

---

### 7. Path64 Module (`src/tiny_skia/path64/`)

#### 7.1 Overall Status

Path64 bug fixes are landed and covered by tests:

| Bug | File | Issue | Fix |
|-----|------|-------|-----|
| `findInflections` coefficient | `Cubic64.cpp` | Used `ax` instead of `bx` | Changed to `bx * cy - by * cx` |
| `binarySearch` conditional | `Cubic64.cpp` | Inverted `if (!ok)` → `if (ok)` | Corrected branch direction |
| `horizontalIntersect` slicing | `LineCubicIntersections.cpp` | Passed full coords instead of y-offset slice | Pass `coords + 1` for y-axis |

#### 7.2 `cubeRoot` Divergence

| Aspect | Rust | C++ | Status |
|--------|------|-----|--------|
| Implementation | Custom Halley-method iteration with bit-hack seed | Custom Halley-method path (`cbrt5d`/`halleyCbrt3d`/`cbrtaHalleyd`) | ✅ aligned |
| Precision intent | Deterministic across platforms | Deterministic algorithmic path | ✅ aligned |

---

### 8. Rust Inline Test Porting Status

The previously missing inline test coverage listed for `color.rs`, `dash.rs`,
and `stroker.rs` is now present in C++ tests.

No remaining verification work items are open in this design tracker.

---

## Phase 2: Fix Plan

Completed work items WI-01 through WI-11 were removed from the active plan.
All tracked WI-12 through WI-18 items are now closed.

### Recently Completed

- **WI-12** (2026-03-03): Function-map status sync updated; stale `Mask` entries
  in `core.md` promoted to `🟢`.
- **WI-13** (2026-03-03): Split `shaders/Mod.*` into
  `Gradient.*`, `LinearGradient.*`, `RadialGradient.*`,
  `SweepGradient.*`, and `Pattern.*`.
- **WI-14** (2026-03-03): Added `FloatingPoint.h/.cpp` and moved
  `floating_point.rs`-mapped wrappers/helpers there; updated `Color.*`,
  `Scalar.h`, `Geom.*`, and `FixedPoint.cpp`.
- **WI-15** (2026-03-03): Moved `Transform` into dedicated `Transform.h/.cpp`
  and removed transform ownership from `pipeline/Mod.*`.
- **WI-16** (2026-03-03): Split path-vector wrappers into dedicated
  `F32x2.h` and `F32x4.h`, with `PathVec.h` kept as a compatibility aggregator.
- **WI-17** (2026-03-03): Added explicit ownership wrapper layers:
  `PathGeometryCoreRs.h` (`tiny-skia/src/path_geometry.rs`) and
  `PathGeometryPathRs.h` (`tiny-skia/path/src/path_geometry.rs`).
- **WI-18** (2026-03-03): Added explicit wrappers for path module boundaries:
  `PathRectRs.h`, `PathSizeRs.h`, and `PathScalarRs.h`.
- **Verification:** `bazel build //...` and `bazel test //...` passed.

---

## Accepted Language-Level Divergences

Only language-level idiomatic differences are accepted. Module/file topology
divergences are **not** accepted and must be tracked as work items.

| Divergence | Rust | C++ | Rationale |
|------------|------|-----|-----------|
| Lowp tail handling | Dual function arrays | Single path + `tail` param | Simpler in C++, identical results |
| Naming convention | `snake_case` | `lowerCamelCase` | Project convention |
| Error types | `Option<T>` / `Result<T,E>` | `std::optional<T>` | Language idiom |
| Pattern matching | `match` expressions | `if/else` chains | Language idiom |
| Lifetimes | Explicit `'a, 'b` | RAII / raw pointers | Language idiom |
| Trait dispatch | `&mut dyn Trait` | `virtual` base class | Language idiom |
| Enum with data | Rust enum variants | `std::variant<...>` | Language idiom |
| cfg_if! SIMD | Conditional native SIMD | Follow-on implementation in `tiny-skia_cpp_cfg_if_simd.md` | No longer an accepted permanent divergence |

## Testing and Validation

- Every work item must leave `bazel build //...` and `bazel test //...` green.
- Status/doc sync (WI-12) must match real code state and not regress coverage.
- Topology refactors (WI-13 through WI-18) must preserve behavior while
  improving one-to-one Rust module traceability.
- Final parity sign-off requires reading each affected C++ function against its
  Rust counterpart and confirming branch-for-branch, constant-for-constant match.

## Security / Privacy

No change to trust boundaries. All inputs remain repository-local source files.
