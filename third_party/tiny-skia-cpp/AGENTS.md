# AGENTS Instructions for `tiny-skia-cpp`

This repository is a C++20, Bazel-first porting effort for `tiny-skia` (Rust) to C++.

## Scope and Process

- Start every feature by writing a design doc under `docs/design_docs`.
- Keep design docs in the `docs/design_docs` directory and follow the templates there.
- Use `docs/design_docs/AGENTS.md` as the authoritative doc-style guide.

## Coding Style

- Use C++20 and Bazel as the primary build system.
- Keep line length under 100 characters when practical.
- Use clear naming, strong types, and explicit ownership boundaries.
- Use lowerCamelCase for all function names (e.g., `catchOverflow`, `setForceHqPipeline`).
- Prefer deterministic, bit-accurate implementations and explicit comments only when non-obvious.
- Keep edits minimal and consistent with existing style in touched files.
- Run both `bazel build //...` and `bazel test //...` after each implementation step.
- Add/extend C++ tests as each file is ported before running the per-step build/test gate.
- Prefer Google Mock matchers (`EXPECT_THAT`, `ASSERT_THAT`) for container/value assertions
  when they provide better diagnostics than plain equality checks.
- Prioritize diagnosable failures: choose assertion styles that clearly report element-level
  mismatches (for example `ElementsAre`/`Pointwise`) over opaque pass/fail comparisons.
- It is explicitly acceptable to add custom matchers when they improve readability and
  diagnostics (for example `ScreenIntRectEq`).
- Prefer reusable matcher helpers in `src/**/tests/test_utils/` (header-only where practical)
  and share them across tests instead of duplicating ad-hoc matcher logic per file.
- Keep tests colocated with source modules:
  - `src/tiny_skia/Foo.cpp` -> `src/tiny_skia/tests/FooTest.cpp`
  - `src/tiny_skia/subdir/Bar.cpp` -> `src/tiny_skia/subdir/tests/BarTest.cpp`
- Update the design doc immediately when a function or module is marked complete or in progress.
- Keep milestone checkboxes synchronized with code changes (any new/edited/deleted file must
  be reflected by an accurate status change in the design tracker).

## LLM-Specific Guidance

- Use `AGENTS.md` and `docs/design_docs/AGENTS.md` as the source of truth for local instruction.
- Keep design and implementation steps actionable and testable.
- When a user asks to keep a larger-step strategy, plan and execute a cohesive, larger-than-normal
  implementation batch (code + tests + tracker/doc status updates) instead of micro-steps.
- By default, if the active model is in a top capability tier (for example GPT-5.2-Codex or
  Claude Opus 4.5-class reasoning), use the larger-step strategy automatically unless the user
  explicitly requests smaller incremental steps.
- For any code change (new/edited/deleted file), update milestone checkboxes or function
  status entries in the same update batch as implementation.
- During porting, explicitly report good sequence points for incremental commits.
- Explicitly call out when a full Rust source file mapping is complete in C++ so the user can
  decide to commit in pieces.
- Before taking risky actions (large refactors, deletions, destructive git operations), confirm intent.
- No commit is allowed without explicit user approval in this session, reviewed live.
- Explicit user approval is required for **every** commit operation (including after previous
  handoff phrases), regardless of any shorthand wording.
- There is no implied commit permission. Always ask before running `git commit`.
- The user phrase “Commit and next step” is only valid when it appears as an explicit request and
  indicates approval for the current outstanding working-directory diff only.
- “Next step” and similar non-commit phrases are explicitly non-committal and must not trigger any
  commit.
- Never commit partially. If a commit is approved, include all currently outstanding working-directory
  changes in that commit.

## Building

- Favor Bazel targets and keep Bazel files organized by module.
- Keep build configuration explicit and easy to diff for review.
- Avoid adding unnecessary dependencies.

## Docs

- Prefer concise Markdown with the repository-specific section structure.
- Keep markdown paths and links simple and stable.

## Design-Doc-First Flow

1. Create a design doc under `docs/design_docs/` using `design_template.md`.
2. Include Summary, Goals, Non-Goals, Implementation Plan (checklist), and
   Testing/Validation sections.
3. Get user approval before implementing.
4. Implement in batches — each batch must update the design doc's milestone
   checkboxes and function status entries.
5. After implementation is complete, convert to `developer_template.md` format.

## Build and Test Gates

Every implementation step must pass both gates:

```bash
bazel build //...    # All targets compile without errors or warnings
bazel test //...     # All tests pass in both native and scalar SIMD modes
```

"Green" means zero failures and zero new warnings. Do not proceed to the next
step or request a commit if either gate fails.

## Safe Edit Patterns

### Adding a new source file

1. Create `src/tiny_skia/[subdir/]Foo.h` and `Foo.cpp`.
2. Add to the appropriate `BUILD.bazel` (`cc_library` or `filegroup`).
3. Add to `CMakeLists.txt` source list.
4. Create `src/tiny_skia/[subdir/]tests/FooTest.cpp` with a dual-mode test.
5. Run both gates.

### Modifying existing code

- Keep edits minimal and consistent with the touched file's existing style.
- Any change to `wide/` types must produce identical results in native and
  scalar modes — run the full test suite to verify.
- Changes to `pipeline/` or `scan/` may affect rendering output — check
  integration tests and golden images.
- Never change behavior in a readability-only pass.

### File organization and naming

- Headers and sources are colocated: `Foo.h` and `Foo.cpp` in the same directory.
- Tests live in a `tests/` subdirectory: `src/tiny_skia/tests/FooTest.cpp`.
- Use `UpperCamelCase` for file names matching the primary type they define.
- SIMD backend files use the pattern `{Backend}{Type}.h`
  (e.g., `ScalarF32x4T.h`, `X86Avx2FmaF32x8T.h`, `Aarch64NeonI32x4T.h`).

## API User Guide

This section is for agents or developers consuming the library's public API.

### Public Entry Points

The main drawing API consists of free functions in `src/tiny_skia/Painter.h`:

| Function | Purpose |
|----------|---------|
| `fillRect` | Fill an axis-aligned rectangle |
| `fillPath` | Fill a path with a given fill rule |
| `strokePath` | Stroke a path with configurable width, caps, joins, dash |
| `drawPixmap` | Composite a source pixmap onto a destination |
| `applyMask` | Apply a mask to a pixmap |

Supporting types are defined in:
- `Pixmap.h` — pixel buffer (`Pixmap`, `PixmapRef`, `PixmapMut`)
- `PathBuilder.h` — path construction (`PathBuilder`, `Path`)
- `Transform.h` — affine transforms (`Transform`)
- `Color.h` — color types (`Color`, `PremultipliedColor`, `ColorSpace`)
- `Stroke.h` — stroke properties (`Stroke`, `StrokeDash`, `LineCap`, `LineJoin`)
- `Mask.h` — clipping masks (`Mask`)
- `Geom.h` — geometry primitives (`Rect`, `IntRect`, `IntSize`, `Point`)
- `shaders/LinearGradient.h`, `RadialGradient.h`, `SweepGradient.h`, `Pattern.h`

### Usage Pattern

The typical rendering flow:

```
1. Create a Pixmap          →  Pixmap::fromSize(width, height)
2. Build a Path             →  PathBuilder → moveTo/lineTo/cubicTo → finish()
3. Configure a Paint        →  Set shader (color, gradient, pattern), blend mode, AA
4. Call a Painter function  →  fillPath(pixmap.asMut(), path, paint, fillRule, transform)
5. Extract pixel data       →  pixmap.takeDemultiplied()
```

### Ownership and Lifetime Rules

- **`Pixmap`** owns its pixel buffer. Move-only (no copies).
- **`PixmapRef`** / **`PixmapMut`** are non-owning views into a Pixmap. They
  must not outlive the Pixmap they reference.
- **`Paint`** and its `Shader` variant are value types — pass and store by value.
- **`Path`** is a value type. `PathBuilder::finish()` consumes the builder and
  returns an `optional<Path>`. `Path::clear()` consumes the path and returns a
  fresh `PathBuilder` reusing the path's allocations.
- **`Transform`** is a small value type (6 floats) — pass by value.
- **`Mask`** owns its buffer. Non-owning `SubMaskRef` views must not outlive it.
- All factory functions that can fail return `std::optional`.

### Minimal End-to-End Example

```cpp
#include "src/tiny_skia/Painter.h"
#include "src/tiny_skia/Pixmap.h"
#include "src/tiny_skia/Geom.h"
#include "src/tiny_skia/Color.h"

using namespace tiny_skia;

// Create a 100x100 pixmap
auto pixmap = Pixmap::fromSize(100, 100);

// Fill a rectangle with a solid color
Paint paint;
paint.setColorRgba8(0, 128, 255, 255);

auto rect = Rect::fromXywh(10.0f, 10.0f, 80.0f, 80.0f);
auto mut = pixmap->asMut();
fillRect(mut, *rect, paint, Transform::identity());

// Extract the pixel data (RGBA, demultiplied)
auto pixels = pixmap->takeDemultiplied();
```

### SIMD Transparency

The public API is identical regardless of SIMD backend. Choose your link target:

- **Bazel**: `//src:tiny_skia_lib` (native) or `//src:tiny_skia_lib_scalar`
- **CMake**: `tiny_skia` (native) or `tiny_skia_scalar` (portable)

No API changes, no `#ifdef`s, no runtime dispatch — backend selection is purely
a compile-time decision.

## Learned Patterns and Recommendations

- For `std::optional` results in tests, prefer matcher assertions over `has_value()` checks:
  - present: `ASSERT_THAT(value, Optional(_))`
  - absent: `EXPECT_THAT(value, Eq(std::nullopt))`
- When asserting multiple related scalar results, prefer aggregating into arrays/spans and using
  `ElementsAre(...)` (or `UnorderedElementsAre(...)`) to improve mismatch diagnostics.
- Promote repeated domain assertions into reusable matchers under
  `src/**/tests/test_utils/` instead of copy/pasting local helper loops.
- Prefer `Field(...)`, `Property(...)`, and `AllOf(...)` for struct/member diagnostics when
  failures should identify exactly which field mismatched.
- Keep matcher upgrades behavior-preserving: no production logic changes in readability-only
  batches.
- Diminishing returns rule: once remaining work is mostly isolated scalar equality checks with
  clear failure output already, prioritize new porting work over further matcher refactors.
- In readability-only test passes, keep edits scoped and still run the full gate:
  `bazel build //...` and `bazel test //...`.
