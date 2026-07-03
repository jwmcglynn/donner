---
name: donner-cpp-conventions
description: Donner's C++ conventions with failure signatures - naming, destFromSource transform discipline, string types, the lifetimebound view-return footgun, banned patterns and the *_lint tests that enforce them, no-exceptions/no-std::any, Doxygen style, formatting commands, and no-dead-code/refactor-in-place. Use when writing, editing, or reviewing any C++ under donner/, or when fixing a *_lint test failure or a clang-tidy / clang-diagnostic-dangling error.
---

# Donner C++ Conventions

Baseline: Google C++ Style Guide, C++20, 100-char lines. Full reference: `docs/coding_style.md`
(depth) and `AGENTS.md` (summary). This skill is the working checklist plus the failure signatures
that catch violations.

## 1. Naming quick reference

| Thing                                        | Convention                                       | Example                                          |
| -------------------------------------------- | ------------------------------------------------ | ------------------------------------------------ |
| Folders                                      | `lower_snake_case`, one word preferred           | `donner/svg/renderer`                            |
| Files                                        | `UpperCamelCase`, match the principal class      | `SVGPathElement.h`, `Path.cc`                    |
| Test files                                   | `_tests.cc` suffix, in a `tests/` subdir         | `tests/Path_tests.cc`                            |
| Fuzzer files                                 | `_fuzzer.cc` suffix                              | `PathParser_fuzzer.cc`                           |
| Classes/structs                              | `UpperCamelCase`                                 | `PathSpline`                                     |
| Non-static member functions                  | `lowerCamelCase` (matches SVG DOM standard)      | `pointAt()`, `hasValue()`                        |
| ALL static member functions & free functions | `UpperCamelCase` — factories AND plain utilities | `Create(...)`, `ParseUnit(...)`, `IsBaseOf(...)` |
| Member variables                             | `lowerCamelCaseTrailingUnderscore_`              | `someMember_`                                    |
| Params / locals                              | `lowerCamelCase`                                 | `inputVariable`                                  |
| Constants                                    | `kUpperCamelCase`                                | `kTimeoutMs`                                     |
| Enum class values                            | `UpperCamelCase`, NO `k` prefix                  | `MaskUnits::UserSpaceOnUse`                      |
| Properties                                   | `thing()` / `setThing(...)`                      | `id()` / `setId(...)`                            |

- Values with units carry the unit in the name (`timeoutMs`) or use a strong type (`Duration`).
- Headers start with `#pragma once` then `/// @file` (in that order).
- Includes: Donner headers repo-relative in quotes (`#include "donner/base/Vector2.h"`);
  STL/third-party in angle brackets (`#include <vector>`). Never `#include "Vector2.h"`.
- All code lives in the `donner` namespace (sub-namespaces like `donner::svg`, `donner::css`).

## 2. Transform naming: destFromSource (mandatory)

Every `Transform2d` local, field, parameter, struct member, and return value is named
`destFromSource` — the name states which coordinate space the value maps INTO and FROM.
Rationale: the name IS the documentation; a direction that lives only in a comment gets composed
backwards the first time the value is reused.

- OK: `bitmapEntityFromEntity`, `worldFromPreviousWorld`, `canvasFromDocumentWorldTransform_`
- Banned: `delta`, `xform`, `t`, `transform`, `mat`, `temp` — and wrong-direction words like
  `deviceToLocal` / `worldToEntity`.

**Composition check** — `Transform2d::operator*` is LEFT-first: `A * B` means "apply A, then B".
So `B_from_A * C_from_B` yields `C_from_A`. Read the chain left-to-right — the DEST of the left
factor must equal the SOURCE of the right factor; if the inner spaces don't match, the math is
wrong. Canonical example (`donner/svg/renderer/RendererDriver.h`):
`worldFromEntityTransform * surfaceFromCanvasTransform_` — entity→canvas, then canvas→surface.
Swapping the factors silently mis-renders rotated content (translations commute, rotations
don't). Mechanically apply this check to every multiplication you write or review; do not trust
prose over the chain rule. If a doc claims the opposite ("`A_from_B * B_from_C` produces
`A_from_C`, rightmost applied first"), the doc is wrong — the code (`donner/base/Transform.h`
`operator*`, `RendererDriver.h`, `Transform_tests.cc`) is left-first.

**Inverses rename the variable.** `bitmapEntityFromWorld.inverse()` must be stored as
`worldFromBitmapEntity`. Keeping the old name on an inverted value is the canonical silent
breakage.

**Reuse existing space names.** Before naming a new coordinate space, grep the target directory
(e.g. `grep -rn 'From[A-Z]' donner/svg/renderer/`) and reuse the established vocabulary
(`device`, `world`, `canvas`, `viewport`, `entity`, `local`, `userSpace`, ...) instead of
inventing a synonym for a space that already has a name.

Full rule: `AGENTS.md` section "Transform Naming Convention".

## 3. String types decision tree

| Situation                                                 | Type                                 |
| --------------------------------------------------------- | ------------------------------------ |
| Non-owning IN-parameter                                   | `std::string_view` (by value)        |
| Owning storage / stored member / return                   | `RcString` (ref-counted, cheap copy) |
| Public-API param that may extend an `RcString`'s lifetime | `const RcStringOrRef&`               |

Helpers in `donner/base/StringUtils.h`: `StringUtils::EqualsLowercase(lhs, lowercaseRhs)`,
`StartsWith`, `EndsWith`, `Contains`, `Find`, `Split(str, ch)` (returns
`std::vector<std::string_view>`). Case-insensitive versions take a template arg:
`StringUtils::StartsWith<StringComparison::IgnoreCase>("Hello", "he")`.

## 4. View-return footgun (issue #603 — read before returning any view type)

**Failure signature:** AddressSanitizer `stack-use-after-scope`, often pointing at a
`std::string_view` read. Root cause pattern: caching a view member of a _returned temporary_, e.g.

```cpp
std::string_view sv = element.tagName().name;  // BUG: tagName() returns XMLQualifiedNameRef
// ... sv now points into the destroyed temporary. RcString's small-string optimization
// stores short strings inline in the temporary, so sv reads freed stack memory.
```

Rules (from `AGENTS.md`, added by commit `ada7c645`, fix for #603):

- `*Ref` view types (`XMLQualifiedNameRef`, `RcStringOrRef`), `std::string_view`, `std::span`,
  and `const T&`-to-temporary are for **in-parameters**, not return types or data members, unless
  the lifetime contract is documented and enforced.
- Never cache `view.member` of a returned temporary `*Ref` in a local view. Copy into an owning
  `RcString` or `std::string` instead.
- When a view return genuinely aliases stable storage, annotate the accessor with
  `UTILS_LIFETIME_BOUND` (from `donner/base/Utils.h`; expands to `[[clang::lifetimebound]]` on
  Clang, `[[msvc::lifetimebound]]` on MSVC, no-op on GCC). Annotated examples:
  `donner/base/RcString.h`, `donner/base/RcStringOrRef.h` (`operator std::string_view`, `data()`,
  iterators).
- Enforcement: `.clang-tidy` sets `WarningsAsErrors: "*"`, so `clang-diagnostic-dangling` is a
  hard error under `bazel build --config=clang-tidy //...` (see `docs/devtools.md`). While
  iterating, scope to just the target you're editing —
  `bazel build --config=clang-tidy //donner/svg:my_target` — and save full `//...` for the final
  pre-push check.

## 5. Banned patterns and the `*_lint` tests

`build_defs/check_banned_patterns.py` runs as an auto-generated `py_test` named `{target}_lint`
for every `donner_cc_library` / `donner_cc_test` / `donner_cc_binary` (see
`build_defs/rules.bzl`). Violations surface as **test failures** under `bazel test //...`, not
compile errors. Lint tests are tagged `lint` and `banned_patterns`.

| Banned                                                                                                                  | Use instead                                                 | Why                                                                                                                                                            |
| ----------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `long long`                                                                                                             | `std::int64_t` / `std::uint64_t`                            | `int64_t` is `long long` on macOS but `long` on 64-bit Linux — `long long` + `std::int64_t` template specializations collide on exactly one platform (PR #415) |
| `std::aligned_storage` / `std::aligned_union`                                                                           | `alignas(T)` on a byte buffer                               | Deprecated in C++23                                                                                                                                            |
| User-defined literal operators (`operator"" _foo`)                                                                      | Named helpers, e.g. `RgbHex(0xFF0000)`                      | Style rule                                                                                                                                                     |
| imgui / GLFW / Tracy includes outside `donner/editor/**`                                                                | Editor-owned abstraction                                    | Exempt: `examples/svg_viewer`, `examples/geode_embed`                                                                                                          |
| `donner/svg/components/**` or `donner/base/xml/components/**` includes outside `donner/svg` / `donner/base`             | Public `SVGElement` APIs; add an accessor if one is missing | ECS internals are not a public surface                                                                                                                         |
| ImGui `AddImageQuad(`                                                                                                   | Direct framebuffer composition                              | Document pixels and overlays must share one presentation space                                                                                                 |
| `.createRenderPipeline` / `.createComputePipeline` outside the Geode pipeline files                                     | `GeodeDevice`-owned pipelines                               | wgpu-native never frees pipelines; per-frame creation leaks until the driver dies (issue #575). See donner-geode-backend                                       |
| Direct `TreeComponent` structural mutation (`appendChild`/`removeChild`/... with a registry arg) outside `TreeMutation` | `donner::svg::components::TreeMutation`                     | Keeps dirty flags, detached-node lifetime, and mutation revisions coherent                                                                                     |

Do not freeze the exemption lists in your head — read the current `exempt_path_prefixes` tuples in
`build_defs/check_banned_patterns.py`.

- **Manual run:** `python3 build_defs/check_banned_patterns.py FILE...`
  (no args = checks all of `donner/` and `examples/`).
- **Escape hatch:** `// NOLINT(banned_patterns: reason)` on the offending line (the checker also
  looks up to 2 lines after the match, for clang-format-wrapped lines). A justification is
  expected — fix the source, never neuter the lint.
- **A `*_lint` failure prints the file:line, the rule, and the remediation** — apply the
  remediation; do not add the file to an exempt list without a real architectural reason.

## 6. Language restrictions

- **No exceptions.** `.bazelrc` sets `build --copt=-fno-exceptions` globally. Never `throw` /
  `try` / `catch`, and never add per-target `-fexceptions`. Return error values instead —
  the `ParseResult<T>` pattern is ubiquitous. `#if UTILS_EXCEPTIONS_ENABLED()` guards the rare
  code that must adapt (from `donner/base/Utils.h`).
- **No `std::any`.** Use concrete types, `std::variant`, forward declarations, or handle/value
  wrappers.
- **`enum class` always**, with an `operator<<` that switch-covers every value and ends in
  `UTILS_UNREACHABLE()` — this makes gtest failures print names instead of integers.
- **Prefer `operator<=>`, but ALWAYS also supply explicit `operator==`** — a gtest bug means
  `<=>` alone breaks `EXPECT_EQ`.
- **`auto` only** when the type appears on the same line, or for iterators, `if (auto maybe = ...)`
  optional patterns, and `ParseResult` locals.
- **Asserts:** `assert(cond && "message")` is debug-only. For checks that must hold in release,
  use `UTILS_RELEASE_ASSERT(cond)` / `UTILS_RELEASE_ASSERT_MSG(cond, "message")` from
  `donner/base/Utils.h`.
- Single-argument constructors are `explicit` by default; mark intentional implicit ones with a
  `/* implicit */` comment. NOT machine-enforced (`google-explicit-constructor` is disabled in
  `.clang-tidy`) — check it yourself; only review catches it. Use `struct` for data, `class` for logic; `const` everywhere possible;
  provide `operator<<` on data classes for debugging.

## 7. Doxygen checklist (all public APIs)

- Header top: `#pragma once`, then `/// @file` (optionally with a brief description).
- Multi-line docs use `/** ... */` **without** `@brief` — the first line is the brief
  automatically. Single-line docs use `///`. Trailing member docs use `//!<`.
- `@param` for every parameter. `@return` only when it adds information.
- Cross-reference with `\ref OtherType`.
- Public members and enum values MUST be documented; private/protected recommended.
- Public-facing docs must not require understanding the ECS (entities/components/registries) —
  describe behavior in DOM/SVG terms (`AGENTS.md` section "Public API Boundary").
- Regenerate locally: `tools/doxygen.sh` (runs `doxygen Doxyfile`) →
  `generated-doxygen/html/index.html`. Doc-comment errors do NOT fail the build
  (`WARN_AS_ERROR = NO` in `Doxyfile`; the deploy workflow runs post-merge with no warning
  gate) — scan doxygen's stderr for warnings YOUR change introduced (main carries a large
  preexisting warning baseline), especially unresolved `\ref` targets.

## 8. Formatting before commit

- **C++:** `clang-format -i <files>` on every modified file, or `git clang-format` for staged
  changes. Config is `.clang-format` (Google-based, `ColumnLimit: 100`); it is tuned so
  clang-format 18 and 19 agree — use either. Newer major versions are unvalidated: if your local
  clang-format reformats lines you didn't touch, restrict the diff to your edits (or pin 18/19)
  rather than committing whole-file churn.
- **TS / JSON / Markdown / TOML:** `dprint fmt` (config `dprint.json`: lineWidth 100, indent 2).
  dprint may not be installed locally — the config file is authoritative; match it manually if
  the binary is missing.
- **Bazel files:** `buildifier <file>`.
- **Never format `third_party/` or `external/`.** Doc-only changes skip formatting and builds.

## 9. No dead code; refactor in-place

- **Never leave orphaned `.cc`/`.h` whose only callers are their own tests.** They stay green in
  CI while investigators grep symbols no live binary executes. (The
  `EditorShell`/`GlTextureCache`/`RenderPanePresenter` dead-code cluster burned weeks of the #582
  investigation.)
- **Refactor incrementally in-place**, landing each step on `main`. Do NOT build a parallel
  implementation to "switch over later" — the old path always survives and diverges. If the
  change is too big for in-place steps, write a design doc with explicit deletion milestones.
- Sever a file's last live caller → delete the file in the same commit. A rebase/merge commit
  that lists "deleted files" must actually delete them, or open and link a tracking issue per
  orphaned path.

## Related skills

- **donner-build-test** — running `bazel test //...`, variant lanes, sanitizer configs, BUILD
  authoring with the `donner_*` macros.
- **donner-bugfix-discipline** — red-to-green repro workflow and gmock-matcher test
  diagnosability standards.
- **donner-editor-editing-rules** — the ImGui-never-renders-vector-graphics rule and DOM-level
  editing boundaries (the editor-side counterparts of the banned-pattern table).
- **donner-geode-backend** — the wgpu pipeline-ownership rule in depth.
- **donner-pr-ci** — pre-push gate and CI triage once your change is committed.
