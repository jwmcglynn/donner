# Design: CI Escape Prevention — Zero Escapes from Local to CI

**Status:** Phase 1 Implemented
**Author:** Claude Opus 4.6
**Created:** 2026-04-07
**Updated:** 2026-04-08
**Tracking:** Org initiative

## Summary

Developers run `bazel test //...` locally and expect it to catch all issues before pushing.
In practice, 5-6 categories of failures routinely escape to CI, costing multiple round-trips
per PR. This design doc catalogues every observed escape category from recent PRs, proposes
concrete mitigations for each, and charts a path to **zero escapes** — where `bazel test //...`
(plus a lightweight `tools/presubmit.sh`) catches everything CI would catch, with the sole
exception of genuinely platform-dependent issues (Linux-only when developing on macOS).

## Escape Taxonomy

Analysis of the last ~50 PRs and CI runs yields six categories of escapes, ordered by
frequency and developer pain:

### Category 1: CMake Build Failures (Most Frequent)

**Affected PRs:** #459 (8 CI iterations), #444 (linker errors + syntax), #481 (missing include)

**Root cause:** CMake is a completely separate build system maintained by `gen_cmakelists.py`.
Developers add files to BUILD.bazel but the CMake side drifts because:
- `gen_cmakelists.py` has 11 manually-maintained data structures that must stay in sync
- Dependency versions are hardcoded separately from MODULE.bazel
- `select()`-based sources/deps require manual `CONDITIONAL_*` dicts
- No local validation exists — CMake only runs in CI

**Observed failure modes:**
1. New source files added to Bazel but not reflected in CMake generation
2. Missing `#include` that works on macOS/Clang (transitive) but fails on Linux/GCC
3. Hardcoded FetchContent versions drifting from MODULE.bazel versions
4. Platform-specific defines baked at generation time via `sys.platform`

### Category 2: Linux-Only Compilation Failures

**Affected PRs:** #415 (int64_t type width), #469 (HarfBuzz linker)

**Root cause:** macOS Clang and Linux Clang/GCC have different type widths, header
transitivity, and linker behavior. `std::int64_t` is `long long` on macOS but `long`
on Linux, causing template specialization collisions. Headers included transitively on
macOS must be explicitly included on Linux.

**Observed failure modes:**
1. Template specialization redefinition (`AddUnsigned<long>` vs `AddUnsigned<int64_t>`)
2. Missing explicit `#include <optional>`, `#include <cstdint>`, etc.
3. Linker symbol visibility differences

### Category 3: Fuzzer Failures (Linux-Only by Default)

**Affected PRs:** #481 (bezier_utils_fuzzer assertion failure)

**Root cause — investigated:** Fuzzers run on Linux CI but `bazel test //...` on
macOS skips them via `fuzzer_compatible_with()` in `build_defs/rules.bzl`. The
restriction exists because **Apple Clang does not ship `libclang_rt.fuzzer_osx.a`**
(confirmed on Apple Clang 17) — only the header `FuzzedDataProvider.h` is included.

**However:** The vendored **LLVM 21 toolchain** (downloaded via `--config=latest_llvm`)
*does* ship `libclang_rt.fuzzer_osx.a` at
`external/toolchains_llvm++llvm+llvm_toolchain_llvm/lib/clang/21/lib/darwin/`.
Running `bazel test --config=asan-fuzzer //...` on macOS works today and catches
fuzzer failures locally. The gap is purely in the default `bazel test //...`
invocation (which uses Apple Clang for speed).

### Category 4: Cross-Backend Rendering Differences

**Affected PRs:** #444 (SVGClipPath/SVGPattern ASCII test failures on Skia backend)

**Root cause — clarified:** Cross-backend testing **already exists** via
`donner_variant_cc_test` in `build_defs/rules.bzl`, which creates separate
`_skia` and `_tiny_skia` variants with backend transitions. These *are* exercised
by `bazel test //...`. The #444 failure was macOS-specific ASCII rendering
differences, not a missing backend test. No action needed for this category
beyond monitoring the existing variant tests.

### Category 5: Coverage Infrastructure Failures

**Affected PRs:** #465, #480 (empty coverage.dat from transitioned tests)

**Root cause:** Custom Bazel rules (`donner_transitioned_cc_test`) didn't forward
`InstrumentedFilesInfo`, causing coverage collection to silently produce empty results.
This was only discoverable by checking Codecov reports, not by test failures.

### Category 6: DevContainer / CodeQL Flakiness

**Affected:** Every main branch push (devcontainer), occasional CodeQL timeouts

**Root cause:** Infrastructure issues unrelated to code changes. Not actionable via
local testing.

### Category 7: Transient Network Failures

**Affected:** Intermittent `apt-get`, `bazel fetch`, GitHub clone / checkout failures

**Root cause:** Flaky external services (GitHub / apt mirrors / Bazel Central
Registry / chromium.googlesource.com). These aren't "escapes" in the code-quality
sense but they do show up as red CI runs that waste developer attention. Mitigation:
retry the network-sensitive steps automatically so a single transient failure
doesn't block a PR.

## Implementation Status (Phase 1 Landed)

The first phase of mitigations has landed. Recap:

### gen_cmakelists.py hardening

- **Versions auto-extracted from `MODULE.bazel`** — eliminates drift between
  Bazel and CMake FetchContent (entt was at v3.13.2 vs MODULE's v3.16.0;
  zlib 1.3.1 vs 1.3.2; woff2 commits diverged). Fixed by parsing `MODULE.bazel`
  at generation time.
- **`sys.platform` replaced with CMake conditionals** — the Skia font defines
  (`DONNER_USE_CORETEXT` vs `DONNER_USE_FREETYPE_WITH_FONTCONFIG`) were being
  baked in at generation time based on *the host running gen_cmakelists.py*.
  Now emitted as `if(APPLE)/elseif(UNIX)` so the generated file is correct
  regardless of host.
- **`find_package(Python3 REQUIRED)` added** — `${Python3_EXECUTABLE}` was
  referenced in embed_resources custom commands but never discovered.
- **Unmapped-dep warnings** — the generator used to silently drop external
  deps that weren't in `KNOWN_BAZEL_TO_CMAKE_DEPS`. It now warns, and fails
  hard in `--check` mode. Auto-mapping for `absl::*` patterns added.
- **Ignore list for Bazel-internal deps** — `@bazel_tools//tools/cpp:*`,
  `@re2//:re2` (test-only transitive), `@skia//src/*:*` (internal modules).
- **`--check` mode** — generates CMakeLists.txt to the workspace, statically
  validates the output (every referenced source exists, every linked target
  is defined or external), then restores the workspace. Found 2 real pre-existing
  drift items during bring-up.
- **Unit tests** — `tools/cmake/gen_cmakelists_test.py` with 23 tests covering
  version normalization, MODULE.bazel parsing, auto-mapping, and CMake target
  extraction. Wired into `bazel test //tools/cmake:gen_cmakelists_test`.

### Lint: banned source patterns wired into bazel test

- **`build_defs/check_banned_patterns.py`** — catches `long long`,
  `std::aligned_storage`, `std::aligned_union`, and user-defined literal
  operators. Supports `// NOLINT(banned_patterns: reason)` on the match line
  or ±2 lines (to handle clang-format line wrapping). Preserves line counts
  when stripping comments so reported line numbers match the source.
- **Bazel integration:** `donner_cc_library`, `donner_cc_test`, and
  `donner_cc_binary` in `build_defs/rules.bzl` now emit a `{name}_lint`
  `py_test` per library that runs the checker on just that library's
  `srcs`+`hdrs`. `bazel test //...` auto-runs 161+ lint tests (~0.3s each,
  Bazel-cached). A new `long long` or UDL in any source fails the
  corresponding library's lint test immediately.
- **Handling of `select()`-valued srcs:** those targets skip lint emission
  because `select()` can't be enumerated at load time. This is rare (mostly
  backend-variant sources) and those files are still covered by clang-tidy.
- **Cleared exceptions:** the `AddUnsigned<long long>` primitive template
  specialization in `MathUtils.h` and the 3 `_cv` UDL operators in test files
  are annotated with `// NOLINT(banned_patterns: reason)`.

### Include completeness: deferred to clang-tidy

The initial design proposed a regex-based `check_includes.py`. After review,
that approach was dropped in favor of enabling clang-tidy's
`misc-include-cleaner` (follow-up PR). Clang-tidy already runs via the
existing `--config=clang-tidy` aspect, has real C++ semantics, no regex
false positives, and no hand-maintained rule table.

### Workflow integration

- **`tools/presubmit.sh`** — single-command local check. Since banned
  patterns run inside `bazel test //...` via per-library `_lint` py_tests,
  this script's job reduces to: `gen_cmakelists.py --check` (must run
  outside bazel), `bazel test //...`, and `clang-format --dry-run` on
  modified files. Flags: `--fast` (skip bazel), `--no-cmake`, `--no-bazel`,
  `--no-format`.
- **`.github/workflows/lint.yml`** — fast job running `gen_cmakelists_test`
  unit tests + `gen_cmakelists.py --check` in parallel with the main build.
  Retries the `--check` step once to tolerate transient `bazel query`
  failures (e.g. registry rate-limits).
- **`.github/workflows/cmake.yml`** — runs `gen_cmakelists.py --check`
  before the CMake build on both Linux and macOS, so generator-level
  issues fail with a clear error instead of an opaque CMake build failure.
- **`.github/workflows/main.yml`** — network-sensitive steps (`apt-get`,
  `bazelisk fetch`) retried 3× via `nick-fields/retry` to survive transient
  GitHub/BCR/chromium outages. Build and test steps are **not** retried so
  real failures surface immediately. Added a macOS fuzzer test step using
  `--config=asan-fuzzer` (informational initially).
- **`build_defs/rules.bzl`** — `donner_cc_library`, `donner_cc_test`, and
  `donner_cc_binary` now auto-emit a `{name}_lint` py_test per target
  (tagged `lint`, `banned_patterns`). 163 lint tests are generated across
  the project; each runs in ~0.3s and is fully bazel-cached.

## Proposed Mitigations

### Phase 1: CMake Validation (Highest Impact)

#### 1A: Local CMake Smoke Test (`bazel test //tools/cmake:cmake_validation_test`)

Add a Bazel test target that:
1. Runs `gen_cmakelists.py` to regenerate CMakeLists.txt files
2. Diffs against the checked-in versions
3. Fails if they diverge

This catches the most common escape: developers modify BUILD.bazel files but forget to
regenerate CMake. The test is fast (just runs bazel queries + diffing) and integrates
into `bazel test //...`.

```python
# tools/cmake/BUILD.bazel
donner_cc_test(
    name = "cmake_validation_test",
    # Actually a sh_test that runs gen_cmakelists.py and diffs
)
```

Implementation: `sh_test` that runs `gen_cmakelists.py --check` (new flag) which
generates to a temp directory and diffs against the workspace. Exit code 1 on drift.

#### 1B: Make gen_cmakelists.py More Resilient

**Extract dependency versions from MODULE.bazel** instead of hardcoding them:

```python
def extract_versions_from_module_bazel() -> Dict[str, str]:
    """Parse MODULE.bazel to get canonical dependency versions."""
    # Parse bazel_dep() and archive_override() calls
    # Return {dep_name: version_or_commit}
```

This eliminates the version drift between MODULE.bazel and CMake FetchContent
(currently entt, zlib, woff2, and rules_cc are all out of sync).

**Replace `sys.platform` with CMake conditionals** for the Skia font defines:

```python
# Before (fragile):
if sys.platform == "darwin":
    f.write("  target_compile_definitions(... DONNER_USE_CORETEXT)\n")

# After (correct):
f.write("  if(APPLE)\n")
f.write("    target_compile_definitions(... DONNER_USE_CORETEXT)\n")
f.write("  else()\n")
f.write("    target_compile_definitions(... DONNER_USE_FREETYPE_WITH_FONTCONFIG)\n")
f.write("  endif()\n")
```

**Use `bazel cquery` for select-aware queries:**

The current approach uses `bazel query --output=xml` which returns unconditional
attribute values. `select()` branches are invisible, requiring all the manual
`CONDITIONAL_SOURCES` and `CONDITIONAL_TARGETS` dicts. Using `bazel cquery` with
multiple configurations can discover all variants:

```python
def query_all_select_variants(target: str) -> Dict[str, List[str]]:
    """Run cquery with each config to discover select() branches."""
    configs = {
        "tiny_skia": ["--config=tiny-skia"],
        "skia": ["--config=skia"],
        "text_full": ["--config=text-full"],
    }
    variants = {}
    for name, flags in configs.items():
        variants[name] = cquery_srcs(target, flags)
    return variants
```

This is the most ambitious change but would eliminate 4 of the 11 manual data structures
(`CONDITIONAL_TARGETS`, `OPTIONAL_DEPS`, `CONDITIONAL_SOURCES`, `CONDITIONAL_DEFINES`).

**Warn on unmapped external deps:**

When `query_deps` returns an external label not in `KNOWN_BAZEL_TO_CMAKE_DEPS`, emit a
warning (or error in `--check` mode) instead of silently dropping it.

**Add `find_package(Python3)`:** The generated CMakeLists.txt uses `${Python3_EXECUTABLE}`
for embed_resources but never calls `find_package(Python3 REQUIRED)`.

#### 1C: Add Unit Tests for gen_cmakelists.py

The script has zero tests. Add a `gen_cmakelists_test.py` that validates:
- `cmake_target_name()` conversion logic
- `extract_versions_from_module_bazel()` parsing
- Dep mapping completeness (every external dep in the Bazel graph has a CMake mapping)
- Generated output parses as valid CMake (regex-based smoke check)

```python
# tools/cmake/gen_cmakelists_test.py
def test_cmake_target_name():
    assert cmake_target_name("//donner/svg:renderer") == "donner_svg_renderer"

def test_all_external_deps_mapped():
    """Every external dep discovered by bazel query has a CMake mapping."""
    external_deps = discover_external_deps()
    for dep in external_deps:
        assert dep in KNOWN_BAZEL_TO_CMAKE_DEPS, f"Unmapped: {dep}"

def test_module_bazel_versions_match():
    """FetchContent versions match MODULE.bazel."""
    module_versions = extract_versions_from_module_bazel()
    cmake_versions = extract_fetchcontent_versions()
    for dep in module_versions:
        assert module_versions[dep] == cmake_versions.get(dep), \
            f"{dep}: MODULE.bazel={module_versions[dep]}, CMake={cmake_versions.get(dep)}"
```

### Phase 2: Include Completeness via clang-tidy (follow-up PR)

Missing includes were the second most common escape (PR #481). The initial
plan was a regex-based `check_includes.py`, but that was dropped in review:
clang-tidy's `misc-include-cleaner` does the same job with real C++ semantics,
no hand-maintained rule tables, and already runs via the existing
`--config=clang-tidy` aspect.

**Follow-up PR plan:**

1. Enable `misc-include-cleaner` in `.clang-tidy` with an `IgnoreHeaders`
   list covering vendored/third-party roots.
2. Triage findings: real missing-include bugs → fix in-tree; false positives
   → annotate with `NOLINTBEGIN(misc-include-cleaner)/NOLINTEND`.
3. Add `bazel build --config=clang-tidy //donner/...` to a new CI job
   (probably in `lint.yml` since it's slow but cacheable).
4. Once clean, make `--config=clang-tidy` a required status check on PRs.

### Phase 3: Platform Type Safety

#### 3A: Static Assert for Type Assumptions

For the #415-style escape where `int64_t` and `long` are the same type on Linux:

```cpp
// donner/base/MathUtils.h
// Guard against platform-specific type aliasing
static_assert(!std::is_same_v<long, std::int64_t> ||
              /* ensure no duplicate specialization */);
```

More generally: use `if constexpr` or SFINAE to avoid specializing templates for types
that might alias on some platforms. Add a coding style rule and grep-based lint.

#### 3B: Type Alias Lint

Add a Bazel test that greps for dangerous patterns:

```python
# Patterns that cause Linux/macOS divergence:
# - Template specialization on both `long` and `int64_t`
# - Template specialization on both `long long` and `int64_t`
DANGEROUS_PATTERNS = [
    r'template.*<long>.*template.*<.*int64_t>',
    r'template.*<.*int64_t>.*template.*<long>',
]
```

### Phase 4: Fuzzer Accessibility

#### 4A: Enable macOS fuzzer runs (no corpus replay needed)

**Correction from initial design:** libFuzzer *does* work on macOS via the
vendored LLVM 21 toolchain. The `--config=asan-fuzzer` config chain activates
`--config=latest_llvm` which downloads clang 21 and brings
`libclang_rt.fuzzer_osx.a` with it. The `donner_cc_fuzzer` rule already has
macOS-specific rpaths and runtime data deps.

The gap is purely that `bazel test //...` (default) uses Apple Clang for speed
and skips fuzz targets via `fuzzer_compatible_with()`. The right fix is:

1. **CI**: Add a macOS fuzzer step to `.github/workflows/main.yml` that runs
   `bazelisk test --config=asan-fuzzer --test_tag_filters=fuzz_target //...`.
   This is landed (initially as informational to avoid blocking during rollout).
2. **Local**: Document `--config=asan-fuzzer` in AGENTS.md so developers can
   reproduce fuzzer failures on macOS.
3. **Longer term**: Investigate whether `--config=asan-fuzzer` can be included
   in the default test matrix without adding unacceptable latency.

No corpus-replay workaround needed.

### Phase 5: Cross-Backend Testing (already works)

**Correction from initial design:** Cross-backend testing is **already
implemented** via the `donner_variant_cc_test` macro in `build_defs/rules.bzl`,
which creates `_skia` and `_tiny_skia` variants with backend transitions.
These run as part of `bazel test //...` — no further action required for this
category. PR #444's ASCII test diffs were due to platform-specific rendering
differences on macOS CI, not a missing backend variant.

### Phase 6: Presubmit Script

#### 6A: `tools/presubmit.sh`

A single script that runs everything CI would catch:

```bash
#!/bin/bash
set -euo pipefail

echo "=== Step 1: Bazel build + test ==="
bazel test //...

echo "=== Step 2: CMake generation check ==="
python3 tools/cmake/gen_cmakelists.py --check

echo "=== Step 3: clang-format check ==="
# Check modified files only (fast)
git diff --name-only HEAD | grep -E '\.(cc|h)$' | xargs clang-format --dry-run -Werror

echo "=== Step 4: buildifier check ==="
# Check BUILD files
git diff --name-only HEAD | grep -E '(BUILD|\.bzl)$' | xargs buildifier --lint=warn --mode=check

echo "All checks passed!"
```

Steps 2-4 are fast (<30s each). Step 1 is the main time cost. The script provides
a single command that mirrors CI.

#### 6B: Integrate into Bazel

Where possible, make presubmit checks Bazel test targets so they're part of
`bazel test //...`:

| Check | Bazel integration | Status |
|-------|------------------|--------|
| Banned source patterns | `{name}_lint` py_test per `donner_cc_library/test/binary` | **Landed (Phase 1)** |
| gen_cmakelists.py unit tests | `//tools/cmake:gen_cmakelists_test` py_test | **Landed (Phase 1)** |
| gen_cmakelists.py --check | Not yet (reentrant bazel issue) | Deferred to aspect PR |
| Include completeness | clang-tidy `misc-include-cleaner` aspect | Follow-up PR |

`gen_cmakelists.py --check` can't run from inside `bazel test` today because
it calls `bazel query` and would deadlock on the outer command lock. The
aspect refactor (Phase 3) replaces `bazel query` with aspect-provided JSON
manifests computed during the outer `bazel build`, at which point the check
becomes a normal `sh_test` with no reentrancy.

Formatting checks (clang-format, buildifier) are best left in `presubmit.sh`
since they require external tools and are fast enough to run separately.

## gen_cmakelists.py Improvement Roadmap

The CMake generator is the single largest source of escapes. Here is a prioritized
improvement plan:

### Current Fragility: 11 Manual Data Structures

| # | Data Structure | Lines | Risk | Mitigation |
|---|---------------|-------|------|------------|
| 1 | `KNOWN_BAZEL_TO_CMAKE_DEPS` | 47-67 | External dep rename | Warn on unmapped deps |
| 2 | `SKIPPED_PACKAGES` | 71-77 | New package needs skip | Low risk, rarely changes |
| 3 | `SKIPPED_TARGETS` | 80-84 | New target needs skip | Low risk |
| 4 | `CONDITIONAL_TARGETS` | 93-122 | New select()-gated target | **Eliminate via cquery** |
| 5 | `OPTIONAL_DEPS` | 127-143 | Must track CONDITIONAL_TARGETS | **Eliminate via cquery** |
| 6 | `CONDITIONAL_SOURCES` | 148-157 | New select()-gated source | **Eliminate via cquery** |
| 7 | `CONDITIONAL_DEFINES` | 161-169 | New conditional define | **Eliminate via cquery** |
| 8 | `EXTRA_LINK_DEPS` | 173-176 | New system library dep | Keep (system deps aren't in Bazel graph) |
| 9 | `EXTRA_INCLUDE_DIRS` | 179-182 | New system include dir | Keep (same reason) |
| 10 | FetchContent versions | 469-476 | MODULE.bazel version bump | **Auto-extract from MODULE.bazel** |
| 11 | Platform-specific defines | 967-989 | Platform mismatch | **Replace with CMake conditionals** |

### Improvement Tiers

**Tier 1 (Low effort, high impact):**
- Add `--check` mode for staleness detection
- Extract versions from MODULE.bazel
- Replace `sys.platform` with CMake conditionals
- Add `find_package(Python3)`
- Warn on unmapped external deps

**Tier 2 (Medium effort, high impact):**
- Use `bazel cquery` to discover `select()` branches automatically
- Eliminate `CONDITIONAL_TARGETS`, `OPTIONAL_DEPS`, `CONDITIONAL_SOURCES`,
  `CONDITIONAL_DEFINES` — derive them from cquery output

**Tier 3 (High effort, transformative):**
- Write a Bazel aspect that emits a JSON manifest of all targets with their
  full attribute sets (including `select()` branches)
- Rewrite gen_cmakelists.py to consume the JSON manifest instead of running
  N+1 Bazel queries
- This eliminates the depth-2 dep query heuristic and makes generation O(1)
  Bazel invocations instead of O(N)

### Known Bugs to Fix

1. **`DONNER_FILTERS` default mismatch:** Script generates `ON`, checked-in file has `OFF`
2. **Missing `find_package(Python3)`:** `${Python3_EXECUTABLE}` undefined in generated output
3. **Version drift:** entt (v3.16.0 vs v3.13.2), zlib (1.3.2 vs 1.3.1), woff2 and rules_cc
   also diverged

## Escape Coverage Matrix

After all mitigations, here is the expected escape coverage:

| Escape Category | Before | Phase 1 (landed) | Phase 2 (clang-tidy) | Phase 3 (aspect) |
|----------------|--------|------------------|----------------------|------------------|
| CMake generator bugs (missing srcs, undefined targets) | Escapes | Caught locally (`--check`) | Caught locally | Caught in `bazel test //...` |
| CMake version drift (MODULE vs FetchContent) | Escapes | Caught locally | Caught locally | Caught locally |
| `long long` / UDL / aligned_storage | Escapes | **Caught in `bazel test //...`** | Caught locally | Caught locally |
| Missing `#include <optional>` / Linux-only includes | Escapes | Escapes | Caught locally (clang-tidy) | Caught locally |
| Fuzzer regressions (parser bugs) | Linux-only | Caught on macOS via `--config=asan-fuzzer` | Same | Same |
| Cross-backend rendering diffs | Already caught | Already caught | Already caught | Already caught |
| Transient GitHub/apt/bazel-fetch outages | Red CI | **Auto-retried 3× in CI** | Same | Same |
| DevContainer/CodeQL flakiness | Infra-only | Infra-only (tracked separately) | Same | Same |

**Acceptable remaining escapes:**
- New fuzzer discoveries (requires Linux libFuzzer; corpus regressions are caught)
- DevContainer infrastructure failures (not code-related)
- CodeQL timeouts (resource-dependent)
- Genuinely platform-specific bugs that can't be detected on the dev's OS

## Implementation Plan

### Phase 1: CMake Validation + Banned Patterns Lint (landed)

All items below landed in a single PR (ci-escape-prevention branch):

| Task | Impact |
|------|--------|
| `gen_cmakelists.py --check` + static validator | Blocks generator bugs before CI |
| Extract versions from MODULE.bazel | Eliminates entt/zlib/woff2 drift |
| Replace sys.platform with CMake conditionals | Cross-host-correct generation |
| Add find_package(Python3) | Fixes embed_resources custom commands |
| Warn/fail on unmapped external deps | Catches new dep additions |
| `gen_cmakelists_test.py` (23 unit tests in bazel) | Regression prevention |
| `check_banned_patterns.py` with per-library lint test in `donner_cc_library` | New `long long`/UDL/aligned_storage fails bazel test automatically |
| `presubmit.sh` + CI workflow retry for transient outages | Single-command local check; flaky networks don't break CI |
| macOS fuzzer step via `--config=asan-fuzzer` | Closes default macOS fuzzer gap |

### Phase 2: clang-tidy misc-include-cleaner (follow-up PR)

| Task | Impact |
|------|--------|
| Enable `misc-include-cleaner` in `.clang-tidy` | Catches PR #481-style missing-include escapes |
| Triage findings, NOLINT justified exceptions | Baseline the first run |
| Add `bazel build --config=clang-tidy //donner/...` to CI lint job | Continuous enforcement |

### Phase 3: Bazel Aspect → JSON Manifest for gen_cmakelists.py (follow-up PR)

| Task | Impact |
|------|--------|
| Write aspect that emits per-target JSON with srcs/hdrs/deps/defines/selects | Foundation for eliminating manual dicts |
| Refactor `gen_cmakelists.py` to consume the JSON manifest | Eliminates N+1 `bazel query` calls |
| Delete `CONDITIONAL_TARGETS`/`OPTIONAL_DEPS`/`CONDITIONAL_SOURCES`/`CONDITIONAL_DEFINES` | 4 of the 11 manual dicts removed |
| Move `gen_cmakelists.py --check` into `bazel test //...` | No more reentrant bazel issue |

### Phase 4 (stretch): Fuzzer corpus acceleration, buildifier, format enforcement

Smaller follow-ups once Phase 1-3 are stable:
- Add buildifier lint as a sh_test or pre-commit step.
- Flip the macOS fuzzer CI step from informational to required.
- Audit `_force_opt_transition` for remaining sharp edges.

## PR Workflow Guidance

This complements the "Pull Request Workflow" section in AGENTS.md (landed in
PR #483) with the new presubmit tooling and known CI behaviors.

### Before opening a PR

1. **Rebase on latest `origin/main`**: `git fetch origin main && git rebase origin/main`.
2. **Run `tools/presubmit.sh`**: the single local command that mirrors CI:
   - `bazel test //...` — runs all unit tests and the auto-generated
     per-library banned-patterns lint tests (`*_lint` py_tests).
   - `gen_cmakelists.py --check` (generator + validator; runs outside bazel
     because it uses `bazel query`).
   - `clang-format` on modified files.
   For a quick iteration loop use `tools/presubmit.sh --fast` (skips bazel test)
   or `--no-cmake` (skips CMake validation, which takes ~10s for bazel queries).
3. **CMake-sensitive changes**: If you added/removed source files or deps in
   BUILD.bazel, always run `gen_cmakelists.py --check` before pushing.
4. **Fuzzer-sensitive changes**: If you touched parser code, run
   `bazel test --config=asan-fuzzer //path/to:your_fuzzer` locally before pushing.

### After opening a PR

1. **Monitor CI and code review every ~7 minutes** using `gh pr checks <number>`
   and `gh api repos/jwmcglynn/donner/pulls/<number>/comments`.
2. **Expect a Codex code review** within the first few minutes. Address feedback
   promptly via follow-up commits.
3. **Distinguish transient from real failures**:
   - **Transient** (retry automatically): `apt-get` failures, `bazel fetch` 429s
     from GitHub/BCR/chromium.googlesource.com, `git clone` timeouts.
     These are now retried 3× in `main.yml`. If they still fail after retries,
     it's a sustained outage — re-run the job manually after a few minutes.
   - **Real** (debug):
     - Lint workflow failures → check the specific rule output
     - CMake workflow failures → run `python3 tools/cmake/gen_cmakelists.py --check`
       locally; it should reproduce
     - Linux-only build failures → usually a missing explicit `#include` or
       `long long`/`int64_t` template collision (PR #415 / #481 patterns)
     - Fuzzer failures → reproduce with `bazel test --config=asan-fuzzer <target>`
     - Test pixel-diff regressions → reproduce with the specific test target;
       do NOT adjust thresholds without root-causing

### Known-flaky things NOT to retry

- **Test failures** — a test that fails once is a real bug until proven otherwise
- **Compile errors** — never transient
- **Linker errors** — never transient, usually indicate missing deps
- **Pixel-diff threshold failures** — investigate root cause, don't re-run

## Success Criteria

1. **Zero CMake escapes:** `bazel test //...` fails if CMake generation is stale
2. **Zero include escapes:** Missing includes caught by lint before CI
3. **Zero type-width escapes:** Template specialization collisions caught at compile time
4. **Fuzzer corpus regression coverage:** Known crash inputs exercised on all platforms
5. **Cross-backend coverage:** Both TinySkia and Skia variants tested locally
6. **Single presubmit command:** `tools/presubmit.sh` mirrors CI completely

## Metrics

Track escape rate over time:
- **Escape rate:** Number of PRs where CI catches a failure not caught by local
  `bazel test //...`, divided by total PRs merged per month
- **Target:** <5% within 1 month of Phase 1, <1% after all phases
- **Current estimate:** ~30-40% of feature PRs have at least one CI-only failure

## Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| CMake validation test slows `bazel test //...` | gen_cmakelists.py runs bazel queries (~10-15s); tag test `size = "large"` so it runs but doesn't block fast iteration |
| cquery approach may not capture all select() semantics | Keep manual overrides as fallback; remove only when cquery output is validated against known-good CMake |
| IWYU may be too noisy / have false positives | Start with explicit-include lint (simpler, fewer false positives) before committing to IWYU |
| Presubmit script adds friction | Keep it optional; the Bazel tests catch the critical escapes automatically |
| Fuzzer corpus tests may be flaky | Corpus inputs are deterministic; failures are genuine bugs |

## Appendix A: Current gen_cmakelists.py Architecture

The script operates in two phases:

1. **`generate_root()`** — Creates root CMakeLists.txt with project setup, FetchContent
   declarations (7 deps with hardcoded URLs/tags), STB library targets, and conditional
   backend blocks.

2. **`generate_all_packages()`** — For each Bazel package, runs `bazel query` to discover
   targets, then per-target XML queries for srcs/hdrs/deps. Dependencies are queried at
   **depth 2** (a heuristic that misses deeper transitive deps). Results are filtered
   through the 11 manual data structures and emitted as CMake.

Key fragility: the depth-2 dep query, lack of `select()` awareness, and 11 manual dicts
that must be maintained by hand whenever BUILD.bazel files change.

## Appendix B: CI Workflow Coverage

| Workflow | Platforms | What It Checks | Local Equivalent |
|----------|-----------|---------------|-----------------|
| main.yml | Linux + macOS | `bazel build //...` + `bazel test //...` | `bazel test //...` |
| cmake.yml | Linux + macOS | `gen_cmakelists.py` + CMake build (no tests) | **None** → Phase 1 adds |
| coverage.yml | Linux only | LCOV coverage + Codecov upload | `tools/coverage.sh` (manual) |
| codeql.yml | Linux | Static analysis | `--config=clang-tidy` (partial) |
| release.yml | Linux + macOS | `-c opt` release build | Manual |
| deploy_docs.yaml | Linux | Doxygen generation | `tools/doxygen.sh` (manual) |
| badges.yaml | Linux | Code metrics | `tools/cloc.sh` (manual) |
