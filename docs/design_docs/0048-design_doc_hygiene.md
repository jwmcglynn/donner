# Design: Design Doc Hygiene — 2026-06-30 Audit Follow-Up

**Status:** In Progress — Milestones 1–5 complete; Milestone 6 blocked on `v08/*` merges.
**Author:** Claude Sonnet 5
**Created:** 2026-06-30

## Summary

A full audit of all 64 docs under `docs/design_docs/` (2026-06-30) verified every
implementation-status claim against the current codebase and corrected 29 docs whose
`Status:` line or content had drifted from reality (see git history around this commit
for the diff). The audit also surfaced structural problems in the design-doc corpus
itself — a missing file still referenced by the index, five unresolved doc-number
collisions, and ten docs not yet numbered in the index (three top-level editor-text
docs plus the seven files under `text/`) — that are mechanical cleanup, not
judgment calls, and were deliberately left unfixed by the audit pass pending review.
This doc tracks that cleanup to completion.

## Goals

- Every top-level file under `docs/design_docs/` has exactly one number, and every
  number in `README.md`'s Document Index resolves to exactly one file — where the `-2`
  suffix convention (`NNNN-` + `NNNN-2-`) is a legitimate shared-number *group*, not a
  collision. Files under `text/` may share a parent number namespace rather than each
  taking a top-level number (see Milestone 3 and Open Questions), so this goal is scoped
  to the number being *stable and indexed*, not necessarily encoded in every filename.
- Every doc under `docs/design_docs/` (including `text/` and the three unnumbered
  editor-text docs) appears in the Document Index.
- Docs whose Finalization rewrite was deferred by the audit (because it required a
  scope judgment call, not a status fix) get that rewrite done deliberately.
- Docs carrying dated resvg data snapshots (which drift against the "no history" rule
  in `AGENTS.md`) are re-scoped as living references — a stated "regenerate by running
  the suite" instruction instead of a hardcoded date — until the underlying text/font
  design work is complete, then replaced with a live-CI pointer at finalization.
- The v0.8 feature-branch docs (`0041-2`, `0044-2`, `0046`, `0047`) get a follow-up
  status pass once `v08/*` branches merge to `main`.

## Non-Goals

- Re-litigating the status corrections already applied in the 2026-06-30 audit pass —
  those are done; this doc only covers what that pass explicitly deferred.
- Auditing docs outside `docs/design_docs/` (e.g. `docs/developer_docs.md`,
  `docs/architecture.md`).
- Deciding the v0.8 vs v1.0 release re-sequencing itself (0028 already flags that
  `ProjectRoadmap.md` has pivoted to v0.8 next) — only re-auditing the affected design
  docs once the merge actually happens.
- Building tooling/CI to auto-detect stale docs. Worth a future doc if this class of
  drift recurs, but out of scope here.

## Next Steps

- Milestones 1–5 are complete and validated (collisions, `0015`, unnumbered-doc
  numbering, the four Finalization rewrites, and the resvg snapshot re-scoping).
- Only Milestone 6 remains, and it is blocked: re-audit the v0.8 feature-branch docs
  (`0041-2`, `0044-2`, `0046`, `0047`) once the `v08/*` branches merge to `main`.

## Implementation Plan

- [x] Milestone 1: Resolve doc-number collisions — done. Kept both numbers and gave the
      `-2` suffix to one doc per pair (no fresh renumbering). Assignment policy: the more
      heavily / code-referenced doc keeps the bare number to minimize churn; ties fall
      back to `git log --diff-filter=A` landing order. Final result:
    - `0025-composited_rendering.md` keeps `0025`; `editor_ux` → `0025-2-editor_ux.md`
    - `0026-svg_conformance_testing.md` keeps `0026`; `drag_end_latency` → `0026-2-drag_end_latency.md`
    - `0027-tight_bounded_segments.md` keeps `0027`; `scripting` → `0027-2-scripting.md`
    - `0028-v1_0_release.md` keeps `0028`; `tinyskia_premul_internal` → `0028-2-tinyskia_premul_internal.md`
    - `0029-ui_input_repro.md` keeps `0029`; `ci_runtime` → `0029-2-ci_runtime.md`
  - [x] Renamed the five files with `git mv`.
  - [x] Updated every inbound reference (doc cross-links + code comments in
        `donner/editor/ViewportState.h`, `donner/editor/BUILD.bazel`, and the
        `0026`/`0028`/`0031` design docs).
  - [x] Rewrote the `README.md` Document Index rows: renumbered, dropped the
        collision-flag notes, and reordered each bare-number keeper before its `-2` sibling.
- [x] Milestone 2: Fix the `0015` broken reference — done. History shows
      `0015-skia_filter_conformance.md` existed (created in #523) and was deleted in
      #546 "Remove full-Skia rendering backend", violating the never-delete rule.
      Restored it as a short `Removed` stub (subject backend is gone; keeps the number
      and inbound links valid, points to git history for the original 300-line doc) and
      converted the README's bare "Missing" placeholder row into a real link.
- [x] Milestone 3: Index the unnumbered docs — done. Decision: number everything.
  - [x] Top-level docs given independent numbers: `structured_text_editing.md` →
        `0049`, `text_editor_behavior.md` → `0050`, `text_editor_refactor.md` → `0051`.
  - [x] The seven `text/` files grouped under one parent number and renamed in place
        (kept in `text/` so `../00NN` parent links stay valid): hub `overview` →
        `text/0052-overview.md`; the rest take `0052-2…-7` (`architecture`,
        `rtl_and_complex_scripts`, `testing`, `text_backend_refactor`, `text_v1_release`,
        `textpath`).
  - [x] Updated inbound references (code comments for `0049`/`0051`; `text/00NN` links
        from `0006`/`0008`/`0010`; the one intra-`text/` sibling link). Also fixed two
        pre-existing broken cross-links in `0049` (`incremental_invalidation.md` /
        `continuous_fuzzing.md` → their `0005-`/`0012-` numbered targets).
  - [x] Replaced the unnumbered README rows with final numbered rows. Validation script
        confirms: 0 collisions, 0 broken index/relative links, every file indexed.
- [x] Milestone 4: Complete deferred Finalization rewrites — done
  - [x] `0020-editor.md`: done. Authored a comprehensive new developer doc
        [`docs/editor_architecture.md`](../editor_architecture.md)
        (`{#EditorArchitecture}`, wired into `developer_docs.md`) from the shipped code
        — frame loop, mutation seam, async rendering/threading, compositor
        presentation, panes/tools, persistence, and trust boundary — then finalized the
        design doc to a summary + pointers + git-history reference. Two stale-comment
        traps were caught and stated accurately in the new doc: the `AsyncSVGDocument.h`
        "single-threaded, no render thread" comment is obsolete (the worker + `ConcurrentDom`
        is live), and the process-isolation sandbox ships as separate binaries and is not
        wired into the interactive editor. Also fixed 6 stale `docs/design_docs/editor.md`
        code-comment references (→ `0020-editor.md`).
  - [x] `0049-structured_text_editing.md`: done. Authored the developer doc
        [`docs/structured_source_editing.md`](../structured_source_editing.md)
        (`{#StructuredSourceEditing}`, wired into `developer_docs.md`) from the shipped
        `XMLSourceStore` / `XMLDocument::applySourceEdit` / `DocumentSyncController`
        API, then finalized the design doc to a summary + pointer + git-history reference.
  - [x] `0043-deterministic_replay_testing.md`: done. Authored the developer doc
        [`docs/deterministic_replay_testing.md`](../deterministic_replay_testing.md)
        (`{#DeterministicReplayTesting}`, wired into `developer_docs.md`) from the
        shipped `GlRnrReplay` / `AsyncRenderer` API, then finalized the design doc to a
        summary + pointer + git-history reference.
- [x] Milestone 5: Re-scope resvg snapshots as living references — done
  - Decision: these tables stay as living references (refreshed by re-running the
    suite) until the underlying text/font design work is complete — at which point the
    finalization pass replaces them with a pointer to a live CI artifact. Until then,
    each table must (a) say "run `bazel test //donner/svg/renderer/tests:resvg_test_suite_{default_text,max}`
    to regenerate" instead of carrying a hardcoded date, and (b) report enabled-pass /
    disabled counts, not a per-test failure ledger.
  - Regenerated 2026-06-30 against a green suite run (both tiny-skia variants pass;
    "failing" entries in the old tables were stale — the suite has no failures, only
    enabled-passing + `DISABLED_`/commented-out tests). Done in this pass:
    - [x] `text/0052-4-testing.md` **Current Snapshot** table refreshed and re-scoped as a
          living reference: `e-text-*` **31/31** (was 30/30), `e-tspan-*` **24/24** (was
          23/24; stale "active failure `e-tspan-030`" callout deleted), `e-textPath-*`
          **33/33** enabled via blessed goldens (was "Still disabled"). Date dropped;
          added a "regenerate by running the suite" instruction.
    - [x] `0008-css_fonts.md` "Resvg Test Coverage" table rewritten and re-scoped:
          `a-font-weight-*` **12/12** at default params (old "0/6, 18K threshold" was
          stale — override gone), `a-font-size-*` **20/20** (was 3/8), `e-tspan-024`
          now passes / `e-tspan-028` now disabled (old pixel-count failures were stale).
          No longer contradicts `text/0052-4-testing.md`.
    - [x] **`text/0052-4-testing.md` body rewritten** for `AGENTS.md` §"No History"
          compliance (749 → 121 lines). Deleted the dated "Historical Failure Analysis
          (2026-03-30)" snapshot, the "Changes since 2026-03-29/03-21" changelogs, the
          "RESOLVED/COMPLETE/Improved from X to Y" gap-analysis fix-plans, and the
          per-property "NOT IMPLEMENTED/PARTIAL" tables that the green suite and 0008's
          updated table contradict. Replaced them with a concise present-tense "Known
          gaps and disabled tests" section grounded in the current `Params::Skip(...)`
          reasons in `resvg_test_suite.cc` (BiDi/RTL, `<tref>`, `textLength`+`lengthAdjust`,
          font shorthand/kerning, SVG 2 text features, plus the catalogued known bugs).
          Kept the intro + refreshed Current Snapshot; no dangling anchors.
  - [x] `0009-resvg_test_suite_bugs.md`: reconciled — **the doc's "34 active overrides"
        is correct; the audit's "off by one (35)" was itself a miscount.** Verified by
        extracting every `resvg-*.png` string fragment (including the `=`-containing and
        concatenated ones a naive grep misses) from `resvg_test_suite.cc`: 34 distinct
        goldens are referenced, and exactly 2 (`resvg-drop-shadow-function-{mm,em}-values.png`)
        are unreferenced on disk — the parked pair. 34 active + 2 parked = the 36
        `resvg-*.png` files present. No change to `0009`.
  - [x] `0013-coverage_improvement.md`: confirmed superseded and finalized into a short
        stub. 7 of 8 proposed test files now exist (`TextEngine_tests.cc`,
        `SVGSVGElement_tests.cc`, `SVGImageElement_tests.cc`, `SVGUseElement_tests.cc`,
        `SVGTextPathElement_tests.cc`, `SVGStopElement_tests.cc`, `SVGLineElement_tests.cc`;
        only the Phase-5 `FontMetadata_tests.cc` didn't land) and 0044 confirms the 85%
        goal is met — so the stale per-phase checklist and dated coverage tables were
        collapsed to a summary + pointer to 0044 (original recoverable from git history).
  - [x] `0002-mcp_test_triage_server.md`: the cited `server.py:NNN-NNN` ranges were all
        stale (file grew 977→1147 lines; `detect_svg_features` moved 157→175, etc.).
        Replaced every line-number citation with a stable reference — the `call_tool`
        handler name per tool, or the named helper function — so they no longer drift.
- [ ] Milestone 6: v0.8 branch reconciliation pass
  - [ ] Once `v08/*` branches (Layers panel, path-authoring UI, showcase export) merge
        to `main`, re-run status checks on `0041-2-path_authoring_and_boolean_operations.md`,
        `0044-2-editor_fluid_canvas_rendering.md`, `0046-editor_group_layers.md`, and
        `0047-v0_8_showcase.md` against the merged code, the same way the 2026-06-30
        audit did for everything else.

## Security / Privacy

N/A — documentation-only changes (renames, index rows, prose). No code, trust
boundaries, or data handling are touched.

## Testing and Validation

- No code changes — this is documentation hygiene. "Passing" means: every top-level
  number resolves to exactly one logical doc (treating `NNNN-` and `NNNN-2-`/`NNNN-3-`
  as one intentional group, **not** a collision — a naive uniqueness check will
  false-positive on `0033-2`, `0041-2`, `0044-2` otherwise), every number in the
  Document Index resolves to a file that exists, and every file under
  `docs/design_docs/` (including subdirectories) has a row in the Document Index. These
  are mechanically checkable; commit the check as a small script and run it as the
  closing gate on Milestones 1–3, rather than eyeballing the table.

## Open Questions

- ~~Milestone 1's renumbering scheme~~ — **resolved:** keep both numbers, `-2` suffix on
  one doc per pair (more heavily/code-referenced doc keeps the bare number; ties fall
  back to landing order). No fresh top-level renumbering.
- ~~Milestone 3's `text/` namespace~~ — **resolved:** the seven `text/` files share one
  parent number (`0052`, hub `overview` bare + `-2…-7`) and stay in the `text/`
  subdirectory so their `../00NN` parent links remain valid.

All open questions are resolved; the remaining work (Milestones 4–6) is tracked in the
Implementation Plan.
