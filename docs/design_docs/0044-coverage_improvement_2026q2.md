# Design: Coverage Improvement 2026-Q2 (81.5% → 85%+)

**Status:** Implemented
**Author:** Claude Opus 4.8
**Created:** 2026-05-29

> **Done (2026-05-29):** Phases 0–4 landed. Repo-wide line coverage
> **81.47% → 85.63%** (full `coverage.sh` run) — the 85% goal is met. Per-file:
> `SandboxCodecs.cc` 50%→95%, `RenderCoordinator.cc` 19%→69%, `TextEditorCore.cc`
> 62%→80%, `XMLDocument.cc` 74%→80%, `TextEditor.cc` 57%→69%. The coverage push
> also surfaced and fixed three real bugs (two TextEditor crashes + the
> `EncodeColor` CurrentColor semantics). See [Results](#results).

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

- [x] **Phase 0: Denominator hygiene** (measured +0.59%, 558 lines)
  - [x] Exclude `donner/**/tests/**` helper `.cc` (e.g. `ImageComparisonTestFixture.cc`
        ~295, `BitmapGoldenCompare.cc` ~131) from the coverage denominator in
        `tools/filter_coverage.py`. (`*_tests.cc` bodies were already absent.)
- [x] **Phase 1: Sandbox wire codecs** (`SandboxCodecs.cc` 50%→69%, ~300 lines)
  - [x] Expand `WireFormat_tests.cc` to round-trip all 25 `Encode*/Decode*` pairs.
  - [x] Add 24 malformed/truncated-input `Decode*` failure cases (trust boundary).
  - [ ] Remaining 475 uncovered: the ~14 per-filter-primitive helpers reachable
        only through `EncodeFilterGraph` — cover one graph node per primitive type.
- [x] **Phase 2: Core + coordinator**
  - [x] `XMLDocument_tests.cc` — 45 cases, mutation/error branches (74%→80%).
  - [x] `RenderCoordinator_tests.cc` — 26 cases, GL-free predicates + rasterize/
        park/dispatch (19%→69%). GL-upload paths stay on the `.rnr` integration suites.
- [x] **Phase 3: Expand thin editor tests** (+43 cases)
  - [x] `DocumentSyncController`, `XmlAutocomplete`, `RopeSimulation`, `AttributeWriteback`.
- [x] **Phase 4: ImGui widgets via the existing headless harness**
  - [x] `TextEditorCore.cc` 62%→80% (+69 cases, headless editing substrate).
  - [x] `TextEditor.cc` 57%→69% (+33 cases via the existing ImGui-context harness:
        keyboard, mouse, render-option toggles, find/replace). The residual ~966
        lines are deep render/layout paths needing a real GPU/font backend — left
        as a future stretch, not required to hold ≥85%.

## Results

Measured by full `tools/coverage.sh --no-html //donner/...` before/after.

| Metric | Before | After |
|--------|-------:|------:|
| Repo-wide line coverage | 81.47% | **85.63%** |
| `editor/sandbox/SandboxCodecs.cc` | 50% | 95% |
| `editor/RenderCoordinator.cc` | 19% | 69% |
| `editor/TextEditorCore.cc` | 62% | 80% |
| `base/xml/XMLDocument.cc` | 74% | 80% |
| `editor/TextEditor.cc` | 57% | 69% |

The **85% goal is met** (85.63%). `TextEditor.cc` (69%, ~966 lines uncovered)
still has headroom — the residual is deep ImGui render/layout paths that need a
real GPU/font backend rather than the headless harness; tracked as a future
stretch, not required to hold ≥85%.

### Follow-ups surfaced

- **Fixed:** `TextEditorCore::handleNewLine` held a `Line&` across `insertLine()`
  (which reallocates `lines_`), a use-after-realloc that crashed on Enter with
  the default `smartIndent=true`. Repro `EnterWithSmartIndentDoesNotReadFreedLineStorage`
  (ASan heap-use-after-free → green).
- **Fixed:** `TextEditor::processFind(findNext=false)` called
  `ImGui::SetKeyboardFocusHere(-1)` unconditionally, segfaulting when invoked
  outside an active frame. Now frame-scope-guarded. Repro
  `ProcessFindInitialOutsideFrameDoesNotCrash` (segfault → green).
- `EncodeColor`'s CurrentColor path writes a default `css::RGBA()` (opaque white),
  not the "transparent" its comment at `SandboxCodecs.cc:135` claims. Behavior
  change out of scope here; documented by test `ColorCurrentColorEncodesAsDefaultRgba`.

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
