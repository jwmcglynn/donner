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
- **No `std::any`** — use concrete types, `std::variant`, forward declarations, or existing handle/value wrappers.

## Architecture

Donner is a dynamic SVG engine (browser-like, not a static renderer). It builds an in-memory DOM that can be inspected, modified, styled, and re-rendered.

### Core Components

- **Parser Suite**: Layered parsers — `donner::xml` (XML/document tree), `donner::svg::parser` (SVG structure/attributes), `donner::css::parser` (stylesheets/selectors/values), `donner::parser` (shared types: numbers, lengths).
- **CSS** (`donner::css`): CSS3 toolkit producing `SelectorRule` and `Declaration` objects.
- **Styling** (`Property`, `PropertyRegistry`, `StyleSystem`): Consumes CSS data, implements SVG style model (presentation attributes, cascading, inheritance) → `ComputedStyleComponent`.
- **Document Model (ECS)**: Built on **EnTT**. Entities = SVG elements, Components = data (`TreeComponent`, `StyleComponent`, `PathComponent`), Systems = logic (`LayoutSystem`, `StyleSystem`, `ShapeSystem`).
- **API Frontend** (`donner::svg::SVG*Element`): User-facing wrappers around ECS entities/components.
- **Rendering**: `RendererDriver` traverses the ECS and emits drawing commands via `RendererInterface`. Backends: **TinySkia** (`RendererTinySkia`, default — lightweight software rasterizer from `third_party/tiny-skia-cpp`), **Skia** (`RendererSkia`, full-featured), and **Geode** (`RendererGeode`, in-development GPU backend via Dawn/WebGPU; gated on `--//donner/svg/renderer/geode:enable_dawn=true`). `Renderer` is the public facade. Select with `--config=skia` or `--config=tiny-skia` (Bazel) / `DONNER_RENDERER_BACKEND` (CMake).
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
4. **Backend** (TinySkia, Skia, or Geode): `RendererDriver` iterates `RenderingInstanceComponent`s in draw order, emitting commands to a `RendererInterface` implementation — sets canvas state, handles layers, draws shapes, configures paint (including offscreen subtree rendering for patterns/markers).

## Pull Request Workflow

When creating a pull request:

1. **Rebase on latest `origin/main`** before pushing — `git fetch origin main && git rebase origin/main`.
2. **Run `tools/presubmit.sh`** before opening the PR. It runs everything CI runs:
   - `bazel test //...` — covers unit tests AND the per-library banned-patterns lint (`*_lint` py_tests auto-emitted by `donner_cc_library`/`_test`/`_binary`). Catches `long long`, `std::aligned_storage`, user-defined literal operators directly at test time.
   - `tools/cmake/gen_cmakelists.py --check` (CMake generator + output validator; runs outside bazel because it uses `bazel query`).
   - `clang-format --dry-run` on modified files.
   Fast iteration: `tools/presubmit.sh --fast` skips `bazel test`.
3. **For fuzzer-sensitive changes**, run `bazel test --config=asan-fuzzer <fuzzer target>`. macOS needs this config because Apple Clang lacks `libclang_rt.fuzzer_osx.a`; `--config=asan-fuzzer` activates the LLVM 21 toolchain which provides it.
4. **Monitor CI and code review** — after opening, check CI status, merge conflicts, and review comments every ~7 minutes until the PR is green and reviewed. Use `gh pr checks <number>` and `gh api repos/jwmcglynn/donner/pulls/<number>/comments`.
5. **Expect a Codex code review** within the first few minutes — address feedback promptly by pushing follow-up commits. If Codex finds no issues it will approve the PR (👍 / APPROVED state). A Codex approval alone is not sufficient to merge — a `jwmcglynn` review is always required.
6. **Transient CI failures** (apt/bazel fetch/chromium rate-limits) are retried automatically. Test, compile, linker, and pixel-diff failures are never transient — investigate the root cause, don't re-run blindly.

See `docs/design_docs/0016-ci_escape_prevention.md` for the full rationale behind these checks and the taxonomy of CI escapes they prevent.

## General Practices

- Prefer existing Donner utilities (`Transform2d`, `RcString`, `StringUtils`) before adding dependencies.
- Docs: follow `docs/AGENTS.md`, use templates under `docs/design_docs/`. Run `tools/doxygen.sh` to regenerate.
- **All code changes should include tests.** Use gMock/gTest. Add fuzzers for parser paths when practical.
- Fix root causes, not symptoms; include necessary error handling without asking. Mainline must stay green — investigate failures rather than dismissing them as pre-existing.

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

Use **destFromSource** naming: `entityFromWorldTransform`, `deviceFromPattern`, `canvasFromDocumentWorldTransform_`. Always `destFromSource` form (e.g., `localFromDevice`, not `deviceToLocal`).

## Feature Flags & Build Configurations

Features are controlled by Bazel flags under `--//donner/svg/renderer:`. Use `--config=` shortcuts for common combos.

### Renderer Backend

Flag: `--//donner/svg/renderer:renderer_backend` (default: `tiny_skia`)

| Config | Backend | Notes |
|--------|---------|-------|
| (default) | TinySkia (`RendererTinySkia`) | Lightweight software rasterizer, no external deps |
| `--config=skia` | Skia (`RendererSkia`) | Full-featured, platform fontmgr, pathops |

### Text Rendering

Text is **off by default**. Two tiers enabled via flags `--//donner/svg/renderer:text` and `--//donner/svg/renderer:text_full`:

| Config | Layout Engine | Description |
|--------|--------------|-------------|
| `--config=text` | stb_truetype (`TextLayout`) | Basic kern-table kerning, glyph outlines |
| `--config=text-full` | FreeType + HarfBuzz (`TextShaper`) + WOFF2 | Full OpenType shaping (GSUB/GPOS), web fonts |
| `--config=skia` | Skia internal | Skia's own text rendering |

`text-full` implicitly enables `text`. When making text changes, test all applicable tiers.

### Filters

Flag: `--//donner/svg/renderer:filters` (default: `true` — **enabled**)

SVG `<filter>` support (filter graph executor + filter primitives). Disable with `--config=no-filters`. Currently supported in TinySkia backend only.

### Examples

```sh
bazel test --config=text-full //donner/svg/renderer/tests:resvg_test_suite
bazel test --config=skia //donner/svg/renderer/tests:resvg_test_suite
# Update goldens:
UPDATE_GOLDEN_IMAGES_DIR=$(bazel info workspace) bazel run //donner/svg/renderer/tests:renderer_tests
```

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
- UB-labeled tests: always `Params::Skip()` — goldens have "UB" text overlay.

## Development Notes

- Format: `clang-format -i` (`git clang-format` for pending changes) for C++, `dprint` for TS/JSON/Markdown (line width 100, indent 2), `buildifier` for Bazel files. Don't format `third_party/` or `external/`. Doc-only changes skip formatting and builds.
- Generated docs: `tools/doxygen.sh` → `generated-doxygen/html/`. Coverage: `tools/coverage.sh`.
- IDE false positives (`entt.hpp` not found, unknown `Registry`) are from missing Bazel context — verify with `bazel build`.
- **LLM quiet mode**: `LLM=1` suppresses verbose renderer test output (pixel dumps, terminal previews, SVG echoes). Set in `.bazelrc`. Re-enable with `DONNER_RENDERER_TEST_VERBOSE=1`. In-repo Claude/Codex settings set `LLM=1` by default.

## Working with Resvg Test Suite

The resvg test suite (`//donner/svg/renderer/tests:resvg_test_suite`) provides comprehensive SVG validation.

### Test-Driven Development

Identify relevant resvg tests before implementing a feature, use them as acceptance criteria, and triage failures per [README_resvg_test_suite.md](donner/svg/renderer/tests/README_resvg_test_suite.md#triaging-test-failures).

### Triage Quick Reference

```sh
bazel run //donner/svg/renderer/tests:resvg_test_suite -c dbg -- '--gtest_filter=*e_text_*'
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
