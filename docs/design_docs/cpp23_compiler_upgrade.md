# Design: C++23 Compiler and Library Upgrade

## Summary
- Raise Donner's minimum supported Clang toolchain to the highest common
  platform baseline (Clang 17 on macOS 26 and Ubuntu 24.04) to unlock C++23
  vocabulary types (`std::expected`) and utility helpers
  (`std::to_underlying`, `std::string::contains`).
- Capture the ceiling imposed by system Clang 17: newer library types like
  `std::flat_map` (Clang 20) are deferred until the ecosystem catches up.
- Define migration steps so Bazel and CMake builds use the upgraded system
  toolchains and so code adoption is sequenced and testable.

## Goals
- Establish a clear minimum Clang version requirement and communicate the delta
  from system defaults.
- Catalog which C++23 features we will adopt first and the compiler versions
  that unlock them.
- Provide an actionable migration and validation plan for Bazel and CMake.
- Keep macOS and Ubuntu developers unblocked via documented toolchain setup.

## Non-Goals
- Switching default compilers away from Clang (e.g., to GCC).
- Reworking non-Clang CI images beyond adding newer LLVM toolchains.
- Replacing existing build systems or changing runtime dependencies.

## Next Steps
- Run the compiler feature probes in CI for both Bazel and CMake builds to
  confirm Clang 17 library coverage on macOS 26 and Ubuntu 24.04.
- Use the new `ParseResult`/`std::expected` bridge to migrate parser utilities
  and property handling without changing error propagation semantics.
- Gate third-party or warning-related regressions that surface with the new
  standard library.

## Implementation Plan
- [ ] Baseline compiler matrix
  - [ ] Validate current Apple Clang (macOS 26 / Clang 17) feature coverage and
        record missing C++23 headers (`<expected>`, `<flat_map>`).
  - [ ] Confirm Ubuntu 24.04 LLVM packages (Clang 17+) and remaining gaps.
- [x] Toolchain upgrade path
  - [x] Add docs and scripts to install Clang 17+ on macOS via Homebrew when the
        system toolchain lags (future macOS releases may ship newer by default).
  - [x] Add docs to install Clang 17 or newer from the apt.llvm.org repository
        on Ubuntu when needed and ensure CI can pin the expected version.
- [ ] Build configuration updates
  - [x] Set `-std=c++23` and the new minimum compiler version checks in Bazel
        and CMake toolchain files.
  - [x] Pin CI and developer containers to Ubuntu 24.04 with Clang 17+ and
        install LLVM 17+ on macOS runners to keep builds on the shared
        baseline.
  - [ ] Gate third-party deps or warnings that regress with the newer standard
        library.
- [x] Compiler feature validation
  - [x] Add test coverage to assert Clang 17 provides `std::expected`,
        `std::to_underlying`, and `std::string::contains`.
- [ ] Feature adoption rollout
  - [ ] Replace custom result wrappers with `std::expected` now that Clang 16+
        is required.
  - [ ] Use `std::to_underlying` and `std::string::contains` to simplify enum
        casts and substring checks.
  - [x] Provide a `ParseResult` <-> `std::expected` bridge to stage migrations
        without changing parser call sites yet.
- [ ] Validation
  - [ ] Run Bazel and CMake builds on macOS with Apple Clang 17 and Ubuntu
        24.04 with Clang 17+.
  - [ ] Add CI jobs for the upgraded toolchain and keep a fallback job pinned to
        the highest system Clang (Apple Clang 17) to catch portability issues.

## User Stories
- As a contributor, I want the repository to specify the required Clang version
  so I can set up a matching toolchain quickly.
- As a maintainer, I want CI to validate new C++23 features so we avoid
  regressions when adopting `std::expected` and `std::flat_map`.

## Background
- `std::expected` is available starting in Clang 16; the new system baseline of
  Clang 17 on macOS 26 and Ubuntu 24.04 ships it in `<expected>`.
- `std::flat_map` arrives with Clang 20; adopting it is deferred until the
  minimum supported toolchain ships the feature.
- Utility helpers: `std::to_underlying` is usable from Clang 13, and
  `std::string::contains` from Clang 12, so they do not block the upgrade but
  will be called out explicitly to encourage use.

## Requirements and Constraints
- Support the latest macOS with only system tools plus an optional Homebrew LLVM
  install; do not require Xcode betas.
- Support Ubuntu 24.04 using official LLVM apt packages without custom patches.
- Maintain Bazel as the primary build; CMake must continue to work for
  contributors who prefer it.

## Proposed Architecture
- Toolchains
  - Keep Bazel and CMake `cc_toolchain` configs aligned to the shared system
    baseline of Clang 17 on macOS 26 and Ubuntu 24.04.
- Feature gating
  - Set the project-wide standard to `-std=c++23` after the toolchain bump.
  - Defer `std::flat_map` usage until the baseline ships it; provide feature
    probes to experiment locally without breaking CI.
- Error-handling primitives
  - Transition parsers to `std::expected` once Clang 17+ is baseline; this
    simplifies current optional/sentinel error flows and aligns with the
    standard vocabulary type.

## Error Handling
- Use `std::expected` for parser and property pipelines once the minimum Clang
  supports it; propagate parse errors explicitly rather than via sentinels.
- Preserve existing logging and diagnostic hooks; only the return types and
  control flow change.

## Security / Privacy
- No new external inputs or trust boundaries are introduced. Existing parser
  validation and fuzzing plans remain applicable after the error-handling API
  swap.

## Testing and Validation
- Add CI coverage for Bazel and CMake builds using Clang 17+ on macOS and Ubuntu
  (system toolchains or official packages).
- Keep a canary CI build on Apple Clang 17 to catch regressions when system
  toolchains lag behind the minimum target.
- Run focused parser/property tests after migrating to `std::expected` to ensure
  error propagation semantics match expectations.

## Dependencies
- System Clang toolchain packages (Clang 17+) on macOS and Ubuntu; no new runtime
  dependencies.

## Rollout Plan
- Phase 1: land toolchain setup docs and CI coverage for Clang 17+.
- Phase 2: flip the project standard to `-std=c++23` and enforce the new minimum
  compiler version in build files.
- Phase 3: migrate code to `std::expected`, `std::to_underlying`, and
  `std::string::contains` where appropriate; defer `std::flat_map` until the
  baseline includes it.

## Alternatives Considered
- Pushing directly to LLVM/Clang 20 to unlock `std::flat_map`: deferred to keep
  parity with the macOS 26 and Ubuntu 24.04 system toolchains and avoid raising
  the setup burden.
- Using GCC as the primary compiler: rejected to avoid fragmenting the toolchain
  and CI matrix.

## Open Questions
- Do we need a temporary shim or backport for `std::expected` to ease the
  transition while Apple Clang 17 remains in circulation?
- Should we vendor libc++20 for macOS instead of relying on Homebrew installs?

# Future Work
- [ ] Revisit the minimum toolchain once Apple Clang includes Clang 20+
  features.
- [ ] Remove fallback build targets once the ecosystem converges on Clang 20 or
  newer.
