# Roadmap: Tiny-Skia-CPP Completion and Quality Hardening

**Status:** Design
**Author:** Codex
**Created:** 2026-03-03
**Updated:** 2026-03-03

## Summary

This roadmap tracks remaining work to move the C++ port from feature-complete parity toward a
production-grade library with enforceable quality gates, stable release process, and
maintainable documentation. The scope is hardening and delivery readiness, not new rendering
features.

## Goals

- Establish deterministic formatting and style tooling for C++ and Bazel files.
- Support both Bazel and CMake build paths with CI coverage.
- Ship complete project-facing docs, including onboarding and API references.
- Add robust CI quality gates across supported platforms/toolchains.
- Add explicit code coverage collection and quality gates for critical modules.
- Improve readability and maintainability with Google-grade C++ best practices and modern C++20
  patterns where behavior is unchanged.
- Remove intermediate porting artifacts from production code and docs.
- Define release discipline: versioning, compatibility expectations, and publishing workflow.
- Add AI-readiness documentation via `AGENTS.md` for code changes and API usage tasks.

## Non-Goals

- New rendering features or algorithmic behavior changes.
- Changing SIMD strategy, pipeline semantics, or ABI decisions already accepted.
- Rewriting stable modules unless required by quality/readability/security findings.

## Next Steps

1. Lock production-grade acceptance criteria and CI gate definitions.
2. Execute formatting/tooling baseline so subsequent milestones are low-noise.
3. Start doc + API + Doxygen pipeline while CI matrix and sanitizer tracks are built in parallel.

## Requirements and Constraints

- Must keep behavior parity with current validated Rust-to-C++ mapping.
- Must keep existing repository build/test gates green for each milestone.
- Must support at minimum `wasm`, `macOS arm64`, and `x86_64` in CI
  (including scalar/SIMD modes where applicable).
- Documentation and release workflows must be reproducible by maintainers without local tribal
  knowledge.
- Improvements should be staged to keep diffs reviewable and diagnosable.

## Proposed Architecture

The roadmap is organized into eight workstreams:

- `repo_tooling`: deterministic formatting and style tooling.
- `ci_quality`: build/test/sanitizer/coverage/perf gates and matrix execution.
- `project_story`: documentation and contributor onboarding.
- `api_surface`: Doxygen and contract comments for public/core interfaces.
- `readability`: style, modern C++20 usage, and cleanup of intermediate artifacts.
- `security_reliability`: fuzzing, sanitizer depth, and dependency/license hygiene.
- `performance`: benchmark baselining and optimization with regression thresholds.
- `release_engineering`: versioning, compatibility policy, changelog, and release publish flow.
- `ai_readiness`: agent-oriented operating guidance for code changes and API usage.

All workstreams feed a final acceptance milestone with objective pass/fail criteria.

## Security / Privacy

This library processes externally provided geometry, paint parameters, transforms, and raster
inputs. These should be treated as untrusted. Production hardening includes:

- Fuzzing/parser-negative tests for path/rasterization entry points.
- Sanitizer coverage (ASan/UBSan minimum; TSan where practical).
- Clear bounds/overflow invariants in safety-sensitive conversion and indexing paths.
- Dependency/license and vulnerability scanning in CI.

## Implementation Plan

- [x] Milestone 1: Tooling Standardization (Formatting Foundation)
  - [x] Add/confirm `.clang-format` and formatter version policy in docs.
  - [x] Run `clang-format` across C++ sources and headers with tracked file audit.
  - [x] Add/confirm `buildifier` formatting commands and policy for Bazel files.
  - [x] Add and document a CMake build path for the library (and tests where practical).
  - [x] Verify Bazel files are buildifier-formatted and include no follow-up drift.
  - [x] Record completion evidence for reproducible formatter runs.

- [ ] Milestone 2: CI Quality Gates and Platform Matrix
  - [ ] Define CI matrix with minimum targets: `wasm`, `macOS arm64`, and `x86_64`.
  - [ ] Enforce `bazel build //...` and `bazel test //...` as required CI checks.
  - [x] Add CI validation for the CMake build path.
  - [ ] Add dedicated warning-clean builds for both `clang` and `gcc`.
  - [x] Drive compiler warnings to zero (or explicitly documented allowlist) for both
    `clang` and `gcc`.
  - [ ] Gate CI on warning-clean status for both compilers.
  - [ ] Add sanitizer jobs (ASan/UBSan; TSan where stable) with documented scopes.
  - [ ] Add mode coverage for SIMD/scalar transitions and fallback paths.
  - [ ] Add line and branch coverage collection for unit/integration tests.
  - [ ] Publish CI coverage reports with trend visibility for critical modules.
  - [ ] Define and enforce minimum coverage thresholds for production-critical modules.

- [x] Milestone 3: Project Documentation and Onboarding
  - [x] Write top-level `README.md` project intro, architecture summary, and quickstart.
  - [x] Add module map linking directories to ownership/responsibility boundaries.
  - [x] Document benchmark and SIMD test workflows for contributors.
  - [x] Include contribution workflow, toolchain versions, and troubleshooting guidance.
  - [x] Add repository `AGENTS.md` with explicit guidance for making codebase changes
    (design-doc-first flow, build/test gates, coding style, and safe edit patterns).
  - [x] Add `AGENTS.md` section for API users (public entry points, expected usage patterns,
    ownership/lifetime expectations, and minimal end-to-end examples).

- [x] Milestone 4: API Documentation and Doxygen Delivery
  - [x] Add Doxygen `@file`, `@brief`, and ownership/contract comments to public headers.
  - [x] Add class/function/group docs for core API and pipeline entry points.
  - [x] Standardize parameter and return contracts for pointer/ownership-sensitive APIs.
  - [x] Set up Doxygen config and local doc generation workflow.
  - [x] Address Doxygen warnings and enforce warning tracking to zero/new-only policy.
  - [x] Add CI workflow to generate docs and publish to GitHub Pages.
  - [x] Publish and maintain a comprehensive API documentation site artifact.

- [ ] Milestone 5: Code Review and C++20 Readability Hardening
  - [x] Execute module-by-module readability pass with deterministic review notes.
  - [x] Normalize style for const-correctness, references/spans, and naming.
  - [ ] Adopt modern C++20 patterns where semantics remain unchanged.
  - [x] Remove TODOs and comments tied to intermediate porting steps.
  - [x] Audit and eliminate temporary references to partial-port decisions.

- [ ] Milestone 6: Security and Reliability Hardening
  - [ ] Add fuzzing targets for input-heavy rendering/path APIs.
  - [ ] Add negative tests for malformed/extreme inputs and overflow boundaries.
  - [ ] Introduce dependency/license/vulnerability checks in CI.
  - [ ] Add SECURITY policy and issue disclosure guidance.

- [x] Milestone 7: Performance Optimization and Regression Guardrails
  - [x] Establish baseline measurements for representative rendering workloads.
  - [x] Identify and benchmark top bottlenecks with reproducible harnesses.
  - [x] Apply low-risk optimizations and track before/after deltas.
  - [x] Define and enforce regression thresholds in CI/perf workflow.
  - [x] Validate semantic parity after each optimization batch.

- [ ] Milestone 8: Release Engineering and Production Delivery
  - [ ] Define versioning strategy (SemVer) and compatibility policy.
  - [ ] Document supported platform/compiler matrix.
  - [ ] Add release checklist: changelog, docs, benchmarks, and test evidence.
  - [ ] Automate release publishing workflow for version tags and release notes.
  - [ ] Document consumer integration paths (Bazel and CMake).

- [ ] Milestone 9: Completion Checklist and Acceptance
  - [ ] Confirm no intermediate-porting artifacts remain in production paths.
  - [ ] Confirm formatting, docs, and CI standards are stable and reproducible.
  - [ ] Confirm `AGENTS.md` guidance is current for both code-change and API-usage workflows.
  - [ ] Confirm coverage thresholds are green for required modules in CI.
  - [ ] Confirm all roadmap checkboxes are complete with linked evidence.
  - [ ] Confirm final status reflected in validation and SIMD roadmap docs.

## Testing and Validation

- Required per milestone:
  - `bazel build //...`
  - `bazel test //...`
- Required hardening tracks:
  - Sanitizer suite execution policy and pass criteria.
  - SIMD/scalar and transition-path coverage in tests.
  - Coverage collection and threshold gates for production-critical modules.
  - Golden/render-regression checks for behavior-sensitive modules.
- Required release checks:
  - Doxygen generation and publish job health.
  - Benchmark comparison output with regression threshold decision.
  - Release checklist artifacts retained with traceable CI links.

## Open Questions

- Which OS/compiler set is the minimum supported matrix at v1.0?
- Should TSan be required or advisory given runtime/flake risk?
- Should docs publish on every merge to default branch or only tagged releases?
