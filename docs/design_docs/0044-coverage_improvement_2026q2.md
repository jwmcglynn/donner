# Design: Coverage Improvement 2026-Q2 (81.5% → 85%+)

**Status:** Design
**Author:** Claude Opus 4.8
**Created:** 2026-05-29

## Summary

Local `tools/coverage.sh` measures **81.5%** line coverage on `main`
(43,698 / 53,640 lines; **9,942 uncovered**). The 2026-03 plan
([0007](0007-coverage_improvement_plan.md)) closed the ECS-system and parser
gaps it targeted — those files are now 90%+. The remaining gap has migrated
almost entirely into `donner/editor/`, which holds **6,223 of 9,942 uncovered
lines (63%)** at 68% coverage, while the core libraries are strong: `svg` 90%,
`base` 87%, `css` 91%.

This plan closes the biggest *winnable* gaps — plain-logic files with no or thin
tests — and explicitly defers the ImGui-coupled UI widgets (which need a headless
UI harness) to a separate, lower-ROI workstream. Phases 0–3 reach ~85% without
touching ImGui code.

## Goals

- Raise local `coverage.sh` line coverage from 81.5% to **≥85%**.
- Stop counting test-support code (`/tests/` helpers) in the coverage
  denominator — it is not product code.
- Add durable round-trip / unit tests for the largest untested plain-logic
  files, prioritizing trust-boundary code (sandbox wire codecs).
- Keep `main` green: every new test must pass under `bazel test //...`.

## Non-Goals

- Chasing 100%. ImGui render paths and CLI `main()` entry points are explicitly
  out of scope for Phases 0–3.
- Rewriting or refactoring the code under test. This is a test-coverage effort;
  behavior changes are out of scope.
- A headless ImGui-driving harness for `TextEditor*` — scoped separately as
  Phase 4 (large effort, lowest ROI).
- GPU/Geode coverage on the local box (blocked by the #542 environment).

## Next Steps

- Land Phase 0 (denominator hygiene) — exclude `/tests/` helper `.cc` from
  `filter_coverage.py`.
- In parallel, draft the three independent test phases (1, 2, 3) and verify each
  builds + passes before measuring the coverage delta.

## Implementation Plan

Phases 1–3 are independent (distinct files, mostly distinct BUILD targets) and
are being executed in parallel.

- [ ] **Phase 0: Denominator hygiene** (~+0.8%, ~558 lines)
  - [ ] Exclude `donner/**/tests/**` helper `.cc` (e.g. `ImageComparisonTestFixture.cc`
        ~295, `BitmapGoldenCompare.cc` ~131) from the coverage denominator in
        `tools/filter_coverage.py`.
  - [ ] Confirm `*_tool.cc` / `*_benchmark.cc` / CLI `main()` files are excluded.
- [ ] **Phase 1: Sandbox wire codecs** (~+1.3%, 775 lines, biggest winnable gap)
  - [ ] Expand `WireFormat_tests.cc` to round-trip every `Encode*/Decode*` pair
        in `SandboxCodecs.cc` (Vector2d/2i, Transform2d, Box2d, Rgba, Color, …).
  - [ ] Add malformed/truncated-input `Decode*` failure cases (trust boundary).
- [ ] **Phase 2: Core + coordinator** (~+1.4%, ~756 lines)
  - [ ] `XMLDocument_tests.cc` — mutation/lifetime paths (442 @ 74%, no test).
  - [ ] `RenderCoordinator_tests.cc` via the `EditorBackendCore` harness (314 @ 19%).
- [ ] **Phase 3: Expand thin editor tests** (~+0.7%)
  - [ ] `DocumentSyncController` (146 @ 62%), `XmlAutocomplete` (81 @ 73%),
        `RopeSimulation` (70 @ 85%), `AttributeWriteback` (65 @ 82%).
- [ ] **Phase 4 (deferred): ImGui widgets via UI harness** (stretch, ~2,000 lines)
  - [ ] `TextEditor.cc` (1364 @ 57%), `TextEditorCore.cc` (654 @ 62%) need a
        headless ImGui-driving harness. Scope separately.

## Background

Coverage is measured by `tools/coverage.sh` (`bazel coverage
--config=latest_llvm`) → `filter_coverage.py` → LCOV `filtered_report.dat`.
Ranking is by **absolute uncovered lines** (LF−LH), which is what moves the
denominator, matching how 0007 framed it.

### Coverage gap inventory (main, 2026-05-29)

| File | Uncov | % | Test? | ImGui? | Verdict |
|------|------:|--:|-------|--------|---------|
| `editor/TextEditor.cc` | 1364 | 57% | yes | yes | Phase 4 |
| `editor/sandbox/SandboxCodecs.cc` | 775 | 50% | partial | no | **Phase 1** |
| `editor/TextEditorCore.cc` | 654 | 62% | yes | yes | Phase 4 |
| `base/xml/XMLDocument.cc` | 442 | 74% | none | no | **Phase 2** |
| `editor/RenderCoordinator.cc` | 314 | 19% | none | no | **Phase 2** |
| `svg/tool/DonnerSvgTool.cc` | 311 | 18% | — | — | exclude (CLI) |
| `svg/renderer/tests/ImageComparisonTestFixture.cc` | 295 | — | — | — | **Phase 0** (test infra) |
| `editor/SidebarPresenter.cc` | 245 | 31% | — | yes | Phase 4 |
| `base/Path.cc` | 209 | 88% | yes | no | Phase 3 (edge cases) |
| `editor/DocumentSyncController.cc` | 146 | 62% | yes | no | **Phase 3** |

Uncovered lines by category: LOGIC 6,879 · SANDBOX 1,105 · GUI/ImGui 843 ·
TEST-INFRA 558 · CLI/TOOL 557.

## Proposed Architecture

No production architecture changes. Test additions mirror existing patterns:
round-trip property tests for codecs (`WireFormat_tests.cc`), `EditorBackendCore`
harness for editor-coordinator logic, and gmock matchers per the repo's
diagnosability rules.

## Security / Privacy

Phase 1 hardens a **trust boundary**: `SandboxCodecs` decodes wire data from a
sandboxed renderer process. Malformed/truncated-input `Decode*` tests are a
security win, not just a coverage win — they assert the decoders fail safely
rather than over-read.

## Testing and Validation

- Every new test must pass under `bazel test //...` (always-green-main).
- Coverage delta measured by re-running `tools/coverage.sh --no-html //donner/...`
  and diffing per-file LF/LH before/after.
- **Invariant → CI target:** "the sandbox wire decoders never over-read on
  truncated input" is enforced by the new malformed-input cases in
  `//donner/editor/sandbox/tests:wire_format_tests` (run under ASan in CI).
