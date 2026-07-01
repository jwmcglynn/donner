# Design: Design Doc Hygiene — 2026-06-30 Audit Follow-Up

**Status:** Draft
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

- Resolve the five collisions the settled way (Milestone 1): leave both numbers in
  place and give the later-landed doc in each pair a `NNNN-2-` suffix — the same
  convention already used for `0033-2`/`0041-2`/`0044-2`. No fresh top-level renumbering.
- Recreate or retire `0015-skia_filter_conformance.md` (Milestone 2).

## Implementation Plan

- [ ] Milestone 1: Resolve doc-number collisions (decided: keep both numbers, `-2`
      suffix on the later-landed doc — no fresh renumbering)
  - [ ] Establish landing order per pair with `git log --diff-filter=A` so the
        earlier-landed doc keeps the bare `NNNN-` number and the later one takes
        `NNNN-2-`, per `AGENTS.md`. Both are currently on `main`:
    - `0025-composited_rendering.md` vs `0025-editor_ux.md`
    - `0026-drag_end_latency.md` vs `0026-svg_conformance_testing.md`
    - `0027-tight_bounded_segments.md` vs `0027-scripting.md`
    - `0028-v1_0_release.md` vs `0028-tinyskia_premul_internal.md`
    - `0029-ci_runtime.md` vs `0029-ui_input_repro.md`
  - [ ] Rename the later-landed file in each pair to its `NNNN-2-` form.
  - [ ] Grep the repo (`docs/`, code comments, PR-linked commit messages are
        out of reach but doc cross-references aren't) for inbound links to the old
        filename and update them.
  - [ ] Update `README.md`'s Document Index to drop the collision-flag notes added by
        the audit pass and reflect the final numbers.
- [ ] Milestone 2: Fix the `0015` broken reference
  - [ ] Check git history (`git log --all --full-history -- 'docs/design_docs/0015*'`)
        for whether `0015-skia_filter_conformance.md` ever existed and was deleted, or
        whether the README entry was always aspirational.
  - [ ] If content exists in history, restore it as an `Implemented`/`Removed` stub
        per the Finalization workflow (Skia was removed as a backend entirely — this
        doc's whole premise may now be moot). If it never existed, remove the row from
        `README.md`'s Document Index.
- [ ] Milestone 3: Index the unnumbered docs
  - [ ] Assign real `NNNN-` numbers to: `structured_text_editing.md`,
        `text_editor_behavior.md`, `text_editor_refactor.md`, and the seven files
        under `text/` (`architecture.md`, `overview.md`, `rtl_and_complex_scripts.md`,
        `testing.md`, `text_backend_refactor.md`, `text_v1_release.md`,
        `textpath.md`). Follow `AGENTS.md` numbering rules — next free number per
        file, or group them under one number with `-2`/`-3` suffixes if they're
        meant to stay logically grouped (matches the existing `text/` directory
        convention).
  - [ ] For the three top-level docs, rename the files with their new `NNNN-` prefix
        and update inbound references. For the `text/` files, **recommended default:
        keep the `text/` directory paths stable and assign the number only in the index
        row** (grouped under one parent number with `-2…`/`-8` suffixes) — because
        `0010-text_rendering.md` and code link into `text/` by relative path, so
        renaming those files is the highest-churn, most breakage-prone part of this
        plan for the least benefit. If a per-file top-level number is chosen instead
        (see Open Questions), grep every relative link into `text/` and update it.
  - [ ] Replace the unnumbered README rows added by the audit pass with the final
        numbered rows.
- [ ] Milestone 4: Complete deferred Finalization rewrites
  - [ ] `0020-editor.md`: the M3–M6 checklist doesn't match what actually shipped
        (e.g. no `NativeFileDialog`/`fuzz_replay_cli`), and M7–M8 aren't described at
        all. Do the full stub rewrite per `AGENTS.md` §Workflow step 4 — link the
        developer docs this design spawned, not the doc's own now-inaccurate plan.
  - [ ] `structured_text_editing.md`: flagged "Implemented" by the audit but not
        finalized into a short stub, because no standalone developer doc exists yet
        for `XMLSourceStore`/`DocumentSyncController` to link to. Write that developer
        doc (via `developer_template.md`), then finalize this doc down to a summary +
        pointer.
  - [ ] `0043-deterministic_replay_testing.md`: marked Implemented but has no
        standalone developer doc yet either. Same treatment — write the developer doc,
        then finalize.
- [ ] Milestone 5: Re-scope resvg snapshots as living references
  - Decision: these tables stay as living references (refreshed by re-running the
    suite) until the underlying text/font design work is complete — at which point the
    finalization pass replaces them with a pointer to a live CI artifact. Until then,
    each table must (a) say "run `bazel test //donner/svg/renderer/tests:resvg_test_suite_{default_text,max}`
    to regenerate" instead of carrying a hardcoded date, and (b) report enabled-pass /
    disabled counts, not a per-test failure ledger.
  - Regenerated 2026-06-30 against a green suite run (both tiny-skia variants pass;
    "failing" entries in the old tables were stale — the suite has no failures, only
    enabled-passing + `DISABLED_`/commented-out tests). Done in this pass:
    - [x] `text/testing.md` **Current Snapshot** table refreshed and re-scoped as a
          living reference: `e-text-*` **31/31** (was 30/30), `e-tspan-*` **24/24** (was
          23/24; stale "active failure `e-tspan-030`" callout deleted), `e-textPath-*`
          **33/33** enabled via blessed goldens (was "Still disabled"). Date dropped;
          added a "regenerate by running the suite" instruction.
    - [x] `0008-css_fonts.md` "Resvg Test Coverage" table rewritten and re-scoped:
          `a-font-weight-*` **12/12** at default params (old "0/6, 18K threshold" was
          stale — override gone), `a-font-size-*` **20/20** (was 3/8), `e-tspan-024`
          now passes / `e-tspan-028` now disabled (old pixel-count failures were stale).
          No longer contradicts `text/testing.md`.
    - [ ] **Remaining (larger, separate No-History cleanup):** `text/testing.md` is 749
          lines and its body below the Current Snapshot is dated/stale gap-analysis
          ("Historical Failure Analysis (2026-03-30)", "Changes since 2026-03-29/03-21",
          per-property "NOT IMPLEMENTED / PARTIAL" sections that the green suite
          contradicts — e.g. font-weight is 12/12 but §"font-weight PARTIAL (7 tests)"
          remains, and §textpath still says "34 failures / all commented out" against
          the now-enabled 33). This is a full doc rewrite per `AGENTS.md` §"No History",
          not a counts refresh — scope and schedule it on its own. The `e-tspan-006`
          "Still failing" line lives in this body and gets fixed as part of it.
  - [ ] `0009-resvg_test_suite_bugs.md`: the audit flagged the doc's "34 active
        overrides" as possibly off-by-one vs live. The golden dir holds **36**
        `resvg-*.png` (34 active + 2 parked), which still matches the doc; but counting
        active golden-override *call sites* in `resvg_test_suite.cc` is not grep-able
        (filenames contain `=`, e.g. `resvg-orient=auto-on-M-C-C-4.png`, and some paths
        are concatenated). Reconcile by careful source read, correct the count, and note
        in the doc that the count is hand-maintained because it can't be mechanically
        derived.
  - [ ] `0013-coverage_improvement.md`: confirm every phase's checkbox state against
        current code (the audit only spot-checked `TextEngine_tests.cc`) before
        finalizing it as fully superseded by 0044.
  - [ ] `0002-mcp_test_triage_server.md`: re-verify the cited line-number references
        (e.g. `server.py:890-977`) against the current file, which has likely grown
        since the doc was written.
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

- ~~Milestone 1's renumbering scheme~~ — **resolved:** keep both numbers and give the
  later-landed doc a `NNNN-2-` suffix (no fresh top-level renumbering). Bare-number docs
  keep their number so existing inbound links stay stable.
- Milestone 3: should the `text/` subdirectory keep its own numbering namespace
  (e.g. all seven files share one parent number with `-2.../-8` suffixes) or should
  each file get an independent top-level number? The existing `0033-2`/`0041-2`/`0044-2`
  convention suggests grouping under a parent is idiomatic here, but there's no single
  "parent" design doc for the `text/` docs to attach to (0010 is the closest fit).
