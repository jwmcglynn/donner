# AGENT Instructions for Donner Repository

Modern C++20 SVG project. Source lives in `donner/`.

## Coding Style

- [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) with C++20 and SVG naming modifications; 100-char line limit enforced by `.clang-format`.
- Folders: `lower_snake_case`. Files: `UpperCamelCase` (matching main class), `.cc`/`.h`/`_tests.cc`. Headers: `#pragma once`, then `/// @file`.
- Includes: project `#include "donner/path/file.h"` (repo-relative), system/third-party `#include <lib/header.h>`.
- Doxygen on all public APIs: `///` single-line, `/** */` multi-line, `//!<` trailing. `@param` for all params. Use `\ref` for cross-references.
- All code in `donner` namespace (sub-namespaces like `donner::svg`).
- Naming: Classes `UpperCamelCase`, methods `lowerCamelCase`, static/global functions `UpperCamelCase`, members `trailingUnderscore_`, constants `kUpperCamelCase`, enum values `UpperCamelCase`. Include units in names (`timeoutMs`) or use strong types. Properties use `thing()` / `setThing()`.
- Strings: `std::string_view` (non-owning), `RcString` (owning), `RcStringOrRef` (flexible API param). Helpers in `"donner/base/StringUtils.h"`.
- Use `enum class` with `operator<<` for debugging. Prefer `operator<=>` with explicit `operator==` (gtest bug workaround).
- Use `auto` sparingly — only when type is obvious or for standard patterns (iterators, `ParseResult`).
- Assert with `UTILS_RELEASE_ASSERT` / `UTILS_RELEASE_ASSERT_MSG` (release) or `assert(cond && "msg")` (debug).
- **No C++ exceptions** — do not use `throw`, `try`, `catch`, or target-specific
  `-fexceptions`; return explicit error/status values instead.
- **No `std::any`** — use concrete types, `std::variant`, forward declarations, or existing handle/value wrappers.

## Architecture

Donner is a dynamic SVG engine (browser-like, not a static renderer). It builds an in-memory DOM that can be inspected, modified, styled, and re-rendered.

### Public API Boundary

- The ECS is an internal implementation detail. It may be discussed in developer docs, design docs,
  and internal architecture notes, but public-facing docs should describe Donner in terms of the
  SVG DOM, documents, elements, styles, resources, and rendering.
- Do not expose new public APIs whose names or required concepts depend on ECS internals, including
  entities, components, registries, systems, raw entity handles, or EnTT types, unless explicitly
  approved as an advanced escape hatch. Prefer DOM-shaped wrappers and typed value handles.
- Public Doxygen for user-facing APIs must not require users to understand the ECS. If a public
  method touches internal storage for legacy or advanced use, document it as an advanced/internal
  escape hatch and keep it out of general API guides, getting-started docs, examples, and release
  notes.

### Core Components

- **Parser Suite**: Layered parsers — `donner::xml` (XML/document tree), `donner::svg::parser` (SVG structure/attributes), `donner::css::parser` (stylesheets/selectors/values), `donner::parser` (shared types: numbers, lengths).
- **CSS** (`donner::css`): CSS3 toolkit producing `SelectorRule` and `Declaration` objects.
- **Styling** (`Property`, `PropertyRegistry`, `StyleSystem`): Consumes CSS data, implements SVG style model (presentation attributes, cascading, inheritance) → `ComputedStyleComponent`.
- **Document Model (ECS)**: Built on **EnTT**. Entities = SVG elements, Components = data (`TreeComponent`, `StyleComponent`, `PathComponent`), Systems = logic (`LayoutSystem`, `StyleSystem`, `ShapeSystem`).
- **API Frontend** (`donner::svg::SVG*Element`): User-facing wrappers around ECS entities/components.
- **Rendering**: `RendererDriver` traverses the ECS and emits drawing commands via `RendererInterface`. Backends: **TinySkia** (`RendererTinySkia`, default — lightweight software rasterizer from `third_party/tiny-skia-cpp`) and **Geode** (`RendererGeode`, in-development GPU backend via Dawn/WebGPU; gated on `--//donner/svg/renderer/geode:enable_geode=true`). `Renderer` is the public facade. Select the default tiny-skia backend or `--config=geode` in Bazel; CMake uses `DONNER_RENDERER_BACKEND` where supported.
- **Base Library** (`donner::base`): Common utilities (`RcString`, `Vector2`, `Transform`, `Length`).

### Rendering Pipeline

Stages transform components through the ECS:

1. **Parsing** → Initial ECS tree (`TreeComponent`, `StyleComponent`, `PathComponent`, etc.)
2. **System Execution** (sequential, dependency-ordered):
   - `StyleSystem`: `StyleComponent` + CSS rules + hierarchy → `ComputedStyleComponent`
   - `LayoutSystem`: Computes bounding boxes (`ComputedSizedElementComponent`), world transforms (`AbsoluteTransformComponent`), viewport/viewBox handling
   - `ShapeSystem`: `PathComponent`/`RectComponent` + computed styles → `ComputedPathComponent` (with `PathSpline`). Handles SVG2 CSS-defined geometry.
   - `TextSystem`: `TextComponent` → `ComputedTextComponent` (laid-out text)
   - `ShadowTreeSystem`: Instantiates shadow trees for `<use>`, `<pattern>`, `<mask>`, `<marker>` → `ShadowTreeComponent`, `ComputedShadowTreeComponent` (main + offscreen branches)
   - `FilterSystem`: `FilterComponent` → `ComputedFilterComponent`
   - `PaintSystem`: Gradients/patterns → `ComputedGradientComponent`, `ComputedPatternComponent`
3. **Rendering Instantiation** (`RenderingContext`): Traverses computed tree, creates `RenderingInstanceComponent` per visible element with resolved references (paint, clip, mask, marker, filter), offscreen subtrees, layer isolation (opacity < 1, filters, masks), and `drawOrder`.
4. **Backend** (TinySkia or Geode): `RendererDriver` iterates `RenderingInstanceComponent`s in draw order, emitting commands to a `RendererInterface` implementation — sets canvas state, handles layers, draws shapes, configures paint (including offscreen subtree rendering for patterns/markers).

## Pull Request Workflow

When creating a pull request:

1. **Rebase on latest `origin/main`** before pushing — `git fetch origin main && git rebase origin/main`.
2. **Run `bazel test //...`** before opening the PR. This is the single source of truth for local validation — it covers:
   - Unit tests across the default config AND the `tiny` / `text_full` / `geode` variant lanes (auto-emitted as `*_tiny` / `*_text_full` / `*_geode` wrappers by `donner_cc_test(variants=…)`).
   - The per-library banned-patterns lint (`*_lint` py_tests auto-emitted by `donner_cc_library`/`_test`/`_binary`). Catches `long long`, `std::aligned_storage`, user-defined literal operators at test time.
   On Intel Arc Xe hosts the Geode lane needs `--test_env=VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json --test_env=XDG_RUNTIME_DIR=/tmp` to fall back to llvmpipe.
   Also run, separately:
   - `python3 tools/cmake/gen_cmakelists.py --check` (CMake generator + output validator; runs outside bazel because it uses `bazel query`).
   - **`clang-format -i` on every modified C/C++ file** before committing — `git clang-format` covers staged changes. The project `.clang-format` is tuned so clang-format 18 and 19 produce identical output, so any locally-installed clang-format works.
3. **For fuzzer-sensitive changes**, run `bazel test --config=asan-fuzzer <fuzzer target>`. macOS needs this config because Apple Clang lacks `libclang_rt.fuzzer_osx.a`; `--config=asan-fuzzer` activates the LLVM 21 toolchain which provides it.
4. **Monitor CI and code review by default for every created PR** — whenever you create or open a PR, automatically keep monitoring CI status, merge conflicts, and review comments every ~7 minutes until the PR is green and reviewed; do not wait for a separate user prompt. For PR monitoring, the policy is to check both comments and CI/status checks on each ~7-minute pass until the PR has stabilized. For draft PRs, keep monitoring CI and mergeability, but do not expect CR/review comments until the PR is published and out of draft. Use `gh pr checks <number>` and `gh api repos/jwmcglynn/donner/pulls/<number>/comments` when `gh` is authenticated, or the GitHub connector equivalents when `gh` is unavailable.
5. **No agent branding in PR titles** — do not prefix pull request titles with `[codex]`, agent names, or other tool branding. Use a plain project-style title that describes the change.
6. **Expect a Codex code review** within the first few minutes after the PR is published and out of draft — address feedback promptly by pushing follow-up commits. If Codex finds no issues it will approve the PR (👍 / APPROVED state). A Codex approval alone is not sufficient to merge — a `jwmcglynn` review is always required.
7. **Transient CI failures** (apt/bazel fetch/chromium rate-limits) are retried automatically. Test, compile, linker, and pixel-diff failures are never transient — investigate the root cause, don't re-run blindly.
8. **Fix CI diagnosability gaps** — if a CI failure cannot be diagnosed because logs, test output,
   screenshots, undeclared outputs, pixel diffs, artifacts, or job summaries are missing or
   inaccessible, treat that as a CI bug and fix the workflow/test harness to expose the missing
   evidence. Do not leave failures opaque or rely on blind reruns when better GitHub Actions
   artifacts or logs would make the next failure actionable.

See `docs/design_docs/0016-ci_escape_prevention.md` for the full rationale behind these checks and the taxonomy of CI escapes they prevent.

## General Practices

- Prefer existing Donner utilities (`Transform2d`, `RcString`, `StringUtils`) before adding dependencies.
- **No private-infra references.** Donner is public: never cite the operator's private repos, their design-doc numbers, or personal notes in code, comments, commits, or PRs. State the motivation in self-contained terms instead. See `CLAUDE.md` §"No Private-Infra References".
- Docs: follow `docs/AGENTS.md`, use templates under `docs/design_docs/`. Run `tools/doxygen.sh` to regenerate.
- **All code changes should include tests.** Use gMock/gTest. Add fuzzers for parser paths when practical.
- Fix root causes, not symptoms; include necessary error handling without asking. Mainline must stay green — investigate failures rather than dismissing them as pre-existing.
- **No dead code, refactor in-place.** Modify existing types/functions/modules step by step — do NOT build a parallel new implementation alongside the old one with the intent to "switch over later." Orphaned `.cc`/`.h` whose only consumers are their own tests are dead code and must be deleted in the same commit that severs their last live caller. See `CLAUDE.md` §"No Dead Code, Refactor In-Place" for the policy that drives this.
- **Prefer replacement over parallel paths.** When adding or switching behavior, such as moving a comparison path to pixelmatch, aggressively remove the old implementation, output fields, docs, and tests instead of keeping multiple code paths unless compatibility or rollback is explicitly required.
- **Pixel-diff tests use `donner/editor/tests:bitmap_golden_compare` (`CompareBitmapToBitmap` / `CompareBitmapToGolden`) + pixelmatch.** No private `composeOver` helpers; no percentage-divergence thresholds — either identity or inspectable `actual_*`/`expected_*`/`diff_*.png` under `$TEST_UNDECLARED_OUTPUTS_DIR`. See `CLAUDE.md` §"Pixel-Diff Tests".
- **Render SVGs with Donner, even from the terminal.** For local SVG previews or PNG
  generation, use `bazel run //donner/svg/tool:donner-svg -- <input.svg> --output <out.png>` or
  Donner renderer test utilities. Do not use external SVG renderers such as `rsvg-convert`,
  ImageMagick, browser screenshots, or resvg unless the task explicitly compares Donner against
  another engine.
- **Regression tests must fail at HEAD before the fix lands.** Commit the failing test on its own commit first so CI records a red→green transition. See `CLAUDE.md` §"Debugging Discipline" and §"Bug-Fix Commit Discipline".
- **Editor visual bugs use the visual debugging playbook.** Start with a live `.rnr`/screenshot repro, then work down the editor stack using [`docs/editor_visual_debugging.md`](docs/editor_visual_debugging.md).
- **Editor path overlays must match the presented shapes below them in the same frame.** During
  pan, zoom, drag, or worker stalls, never show a newer overlay transform over stale document
  pixels; either keep both on the presented transform or move both together.
- **Do not fix editor presentation bugs by clearing broad render caches.** Avoid
  `resetComposited()`, `resetForLoadedDocument()`, full compositor resets, whole presentation-cache
  clears, or forced full reparses for incremental editor mutations such as delete, pathfinder,
  drag, transform, attribute, or source-writeback changes. Those create one-frame checkerboard
  flashes and hide the real invalidation bug. Use targeted entity/tile/region invalidation,
  structural remap, dirty flags, or an explicit render handoff instead. A broad clear is allowed
  only for true document/file replacement or renderer teardown, and the reason must be covered by a
  regression test or design note.

## Building

Bazel is primary. CMake is experimental.

```sh
# Bazel
bazel build //donner/...          # build all
bazel test //donner/base/...      # scope tests to specific dirs (renderer is slow)

# CMake
python3 tools/cmake/gen_cmakelists.py && cmake -S . -B build && cmake --build build -j$(nproc)
# CMake with tests
cmake -S . -B build -DDONNER_BUILD_TESTS=ON && cmake --build build -j$(nproc) && ctest --test-dir build
```

## Transform Naming Convention

Use **destFromSource** naming for every `Transform2d` — locals, fields, parameters, struct members, return values, everything. The destFromSource name *is* the documentation; a value whose direction lives only in a comment will eventually be composed wrong.

- ✅ `entityFromWorldTransform`, `deviceFromPattern`, `canvasFromDocumentWorldTransform_`, `bitmapEntityFromEntity`, `worldFromPreviousWorld`.
- ❌ `delta`, `xform`, `transform`, `mat`, `t`, `temp` — and `deviceToLocal` / `worldToEntity` (wrong direction word).

Composition reads right-to-left under donner's post-multiply convention: `A_from_B * B_from_C` produces `A_from_C` (rightmost applied first). If your composition doesn't spell out a valid `dest_from_source` chain when you read the names, the math is wrong.

Inverses get the swapped name: `bitmapEntityFromWorld.inverse()` is `worldFromBitmapEntity`. Don't keep the original name on a local that holds the inverse.

## Feature Flags & Build Configurations

Features are controlled by Bazel flags under `--//donner/svg/renderer:`. Use `--config=` shortcuts for common combos.

### Renderer Backend

Flag: `--//donner/svg/renderer:renderer_backend` (default: `tiny_skia`)

| Config | Backend | Notes |
|--------|---------|-------|
| (default) | TinySkia (`RendererTinySkia`) | Lightweight software rasterizer, no external deps |
| `--config=geode` | Geode (`RendererGeode`) | Experimental WebGPU + Slug backend |

### Text Rendering

Text is **off by default**. Two tiers enabled via flags `--//donner/svg/renderer:text` and `--//donner/svg/renderer:text_full`:

| Config | Layout Engine | Description |
|--------|--------------|-------------|
| `--config=text` | stb_truetype (`TextLayout`) | Basic kern-table kerning, glyph outlines |
| `--config=text-full` | FreeType + HarfBuzz (`TextShaper`) + WOFF2 | Full OpenType shaping (GSUB/GPOS), web fonts |

`text-full` implicitly enables `text`. When making text changes, test all applicable tiers.

### Filters

Flag: `--//donner/svg/renderer:filters` (default: `true` — **enabled**)

SVG `<filter>` support (filter graph executor + filter primitives). Disable with `--config=no-filters`. Currently supported in TinySkia backend only.

### Examples

```sh
bazel test --config=text-full //donner/svg/renderer/tests:resvg_test_suite
# Update goldens:
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run //donner/svg/renderer/tests:renderer_tests
```

## Test Diagnosability (gmock + ToTT)

- **Prioritize gmock (`EXPECT_THAT` + matchers) over `EXPECT_TRUE(a == b)`-style asserts.** A failure must localize the bug without a rerun — match the whole value and print expected-vs-actual, not a bare boolean (the "Testing on the Toilet" standard). Promote repeated assertion shapes into a named matcher with a descriptive failure message (e.g. the `Rgba`/`RgbaNear`/`Alpha` pixel matchers in `RendererGeode_tests.cc`). See CLAUDE.md §"Test Diagnosability".

## Pixel Diff & Threshold Philosophy

- **Root-cause pixel diffs, always** — even in vendored libraries like tiny-skia-cpp. Don't bump thresholds or inflate max-diff pixels to mask failures; investigate *why* pixels differ. Threshold changes are a last resort requiring explicit human approval.
- **Red herrings**: "glyph outline differences" for resvg failures, "only N pixels off" for any failure. Treat as red herrings without strong evidence — even 200 pixels off can hide real bugs.

### Resvg Test Threshold Conventions

- Pixel diffs <100: **omit the entry** — default `Params()` applies via `getTestsWithPrefix`.
- Non-default thresholds require human review.
- Prefer adjusting the float threshold first (for minor AA/shading diffs).
- Only add `Params::WithThreshold(threshold, N)` after root-cause investigation.
- Don't add `{"test.svg", Params()}` entries — omit entirely.
- `Params::Skip()` for tests that can't pass yet.
- Skipped resvg comparisons are emitted as `DISABLED_...` gtest names. To triage one without
  editing skip tables or adding custom env flags, run with a narrow `--gtest_filter` plus
  `--gtest_also_run_disabled_tests`.
- UB-labeled tests: always `Params::Skip()` — goldens have "UB" text overlay.

## Development Notes

- Format: `clang-format -i` (`git clang-format` for pending changes) for C++, `dprint` for TS/JSON/Markdown (line width 100, indent 2), `buildifier` for Bazel files. Don't format `third_party/` or `external/`. Doc-only changes skip formatting and builds.
- Generated docs: `tools/doxygen.sh` → `generated-doxygen/html/`. Coverage: `tools/coverage.sh`.
- Coverage CI intentionally runs only on `main` pushes and `workflow_dispatch`; PRs get Codecov patch coverage without rerunning the full coverage workflow.
- IDE false positives (`entt.hpp` not found, unknown `Registry`) are from missing Bazel context — verify with `bazel build`.
- **LLM quiet mode**: `LLM=1` suppresses verbose renderer test output (pixel dumps, terminal previews, SVG echoes). Set in `.bazelrc`. Re-enable with `DONNER_RENDERER_TEST_VERBOSE=1`. In-repo Claude/Codex settings set `LLM=1` by default.

## Working with Resvg Test Suite

The resvg test suite (`//donner/svg/renderer/tests:resvg_test_suite`) provides comprehensive SVG validation.

### Test-Driven Development

Identify relevant resvg tests before implementing a feature, use them as acceptance criteria, and triage failures per [README_resvg_test_suite.md](donner/svg/renderer/tests/README_resvg_test_suite.md#triaging-test-failures).

### Triage Quick Reference

```sh
bazel run //donner/svg/renderer/tests:resvg_test_suite -c dbg -- '--gtest_filter=*e_text_*'
bazel run //donner/svg/renderer/tests:resvg_test_suite -c dbg -- \
  '--gtest_filter=*e_text_023*' --gtest_also_run_disabled_tests
# Examine failing SVG, then fix root cause or add skip:
{"e-text-023.svg", Params::Skip()},  // Not impl: `letter-spacing`
```

**Skip comment conventions**: `Not impl: <feature>`, `UB: <reason>`, `Bug: <description>`, `Larger threshold due to <reason>`.

### MCP Test Triage Server

The `resvg-test-triage` MCP server provides 8 tools for automated test analysis:

| Tool | Purpose |
|------|---------|
| `analyze_test_failure` | Analyze single failure with feature detection |
| `batch_triage_tests` | Process multiple failures, group by feature |
| `detect_svg_features` | Parse SVG to identify tested features |
| `suggest_skip_comment` | Generate formatted skip comments |
| `suggest_implementation_approach` | Suggest files to modify + implementation hints |
| `find_related_tests` | Find tests failing for same feature (batch opportunities) |
| `generate_feature_report` | Progress reports by category |
| `analyze_visual_diff` | Categorize failure types from diff images |

Setup & docs: [tools/mcp-servers/resvg-test-triage/README.md](tools/mcp-servers/resvg-test-triage/README.md)
