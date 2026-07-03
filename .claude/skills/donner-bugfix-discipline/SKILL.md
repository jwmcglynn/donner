---
name: donner-bugfix-discipline
description: The mandatory red-to-green bug-fix workflow for Donner — write a failing repro test first, capture failure evidence, commit red then fix, and write diagnosable gmock-based tests (including perf repros with donner_perf_cc_test). Use when fixing any reported bug, writing a regression or perf test, preparing a commit that says "Fixes #NNN", or responding to user pushback that a "fixed" bug is still broken.
---

# Donner Bug-Fix Discipline

The single rule everything below serves: **no fixes without repros**. A fix you cannot verify with
an automated red-to-green test is an _attempt_, not a fix. Full policy: `CLAUDE.md`
sections "Debugging Discipline" and "Bug-Fix Commit Discipline".

## 1. The iron rule: red before green

0. **Editor visual/interaction bugs only:** reproduce the bug through the editor-control MCP
   BEFORE writing any test — if the MCP cannot yet drive the needed gesture, viewport state, or
   capture, extend it first (donner-editor-debugging skill). The MCP bookends the loop: it opens
   the investigation here and closes it (MCP re-verification, section 8). Skipping it invites
   fabricated replay sequences the real editor never fires (e.g. an invented prewarm phase),
   which produce tests that prove nothing.
1. **Write the automated test that reproduces the bug BEFORE touching the fix.**
2. **Run it at HEAD and capture the failure output** (diff PNG, pixel count, error message):
   `bazel test //<pkg>:<target> --test_output=all` (target selection, configs, variant lanes:
   donner-build-test skill). Verify the failure matches the user-reported symptom — a test that
   fails for a _different_ reason proves nothing. "Capture" means the red commit's message
   describes the concrete failure symptom, specific enough that a reviewer can tell it is the
   same failure the user reported (see the model message below); pasting raw stdout is optional.
3. **Commit the red test on its own commit** so CI records a red-to-green transition on the branch.
4. Fix the bug in a follow-up commit; the test now passes.
5. `bazel test //...` must be fully green before pushing (see donner-build-test skill).
6. `clang-format -i` every modified C/C++ file (or `git clang-format` for staged changes).

If you cannot make the test fail at HEAD, **the test is wrong — not the bug**. Rework the test;
do not conclude the bug is gone.

Canonical red-to-green pair. (The commits live on an unmerged feature branch and squash-merge
will strip them from `main`, so the load-bearing text is quoted inline instead of a hash; find
live examples with `git log --all --oneline --grep='^attempt:'`.)

- Red: `attempt: failing repro — diff fallback desyncs DOM on similar-sibling insert` adds
  `EditorSyncTest.WholeTextDiffFallbackDoesNotDesyncDomFromSourceOnSimilarSiblingInsert`
  (`donner/editor/tests/EditorSync_tests.cc`) and documents the exact failure in the message:

  > DispatchSourceTextChange (the no-intent whole-text diff fallback) misclassifies inserting
  > `<rect id="c">` before a near-identical `<rect id="b">` as a single-char id="b"->id="c"
  > attribute edit, renaming the existing sibling and dropping the new element from the DOM
  > though the source bytes are correct. querySelector("#b") returns null. Fix follows in the
  > next commit (red->green).

- Green: `Fix diff-fallback DOM/source desync via structural-fingerprint guard` — the fix
  commit, whose message closes the loop: "Fixes EditorSyncTest.WholeTextDiffFallback… (red at
  \<parent-hash\>, green here)".

The model message names the test, states what fails and why in prose, and announces the
red-to-green sequence. Note it is a plain gmock test — no bitmap or perf machinery. For
non-visual bugs (parsers, CSS cascade, DOM/source sync), a diagnosable gtest IS the complete
evidence; the pixel and perf tooling below applies only when the symptom is pixels or latency.

## 2. Commit-message decision tree

```
Does the commit have a test file + test name that FAILED at the parent commit
and PASSES at this commit?
├─ YES, and the red run is documented (earlier red commit in the PR, or the
│  message cites the failure output)          → may say "Fixes #NNN" / "Closes #NNN"
└─ NO (plausible fix, no documented red run)  → title starts with "attempt:" or
   "hypothesis:", do NOT close the issue. A human decides when evidence suffices.
```

Rationale: "plausible-sounding fix" commits that close issues without a red-to-green record are
how bugs get re-reported three weeks later with the issue already closed.

## 3. User-pushback protocol

When the user reports the bug is still present after a claimed fix, the automatic conclusion is:
**the test that verifies the fix is wrong or missing.** Write a new repro that matches what the
user actually sees.

- Never reply "I don't see why it would still be broken."
- Never ask the user to re-confirm repro steps. Their report _is_ the signal; your test is what
  needs debugging.
- Manual "please run it and tell me what you see" cycles are a last resort, not a workflow.

## 4. Evidence capture

**Pixel evidence** goes through `//donner/editor/tests:bitmap_golden_compare`
(`donner/editor/tests/BitmapGoldenCompare.h`, functions `CompareBitmapToBitmap` and
`CompareBitmapToGolden`) — see the donner-pixel-diff skill for the full API and rules. On
mismatch these write `actual_*.png` / `expected_*.png` / `diff_*.png` to
`$TEST_UNDECLARED_OUTPUTS_DIR`, which lands at `bazel-testlogs/<pkg>/<target>/test.outputs/`.

**TRAP — `UPDATE_GOLDEN_IMAGES_DIR`:** when that env var is set, `CompareBitmapToGolden` _writes_
the current bitmap as the new golden and returns without comparing anything
(`BitmapGoldenCompare.cc`). A "verification" run with it still exported silently passes every
golden test. Unset it before any run you intend to use as evidence.

**Reference tests to clone from** (note: `CLAUDE.md` cites a stale path,
`donner/editor/sandbox/tests/EditorBackendGoldenImage_tests.cc` with a `FilterDisappearRepro7*`
suite — that file does not exist; these are the real ones):

| What it demonstrates                                                     | File / test                                                                                      | Bazel target                                                                                                                             |
| ------------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------ | ---------------------------------------------------------------------------------------------------------------------------------------- |
| Full `.rnr` replay through the live editor pipeline, golden-compared     | `donner/editor/tests/RnrReplay_tests.cc`, `FilterDisappearRepro3MatchesGoldenAfterSecondMouseUp` | `//donner/editor/tests:rnr_replay_tests`                                                                                                 |
| GL-path replay repros (drag hang, post-drag jump, zoom pop)              | `donner/editor/tests/GlRnrReplay_tests.cc`                                                       | `//donner/editor/tests:gl_rnr_replay_tests`                                                                                              |
| Compositor drag correctness + perf gates on the real `donner_splash.svg` | `donner/svg/compositor/CompositorGolden_tests.cc`, `SplashDrag*` tests                           | `//donner/svg/compositor:compositor_golden_tests` (perf-budget tests split into `:compositor_golden_perf_tests`, tagged `manual`+`perf`) |

Seeded `.rnr` repro recordings live in `donner/editor/tests/` (e.g.
`filter_elm_disappear-2.rnr` … `-7.rnr`, `filter_drag_repro.rnr`). How to record and replay them:
`docs/deterministic_replay_testing.md`; visual triage workflow: `docs/editor_visual_debugging.md`
and the donner-editor-debugging skill.

## 5. Banned explanations

| Excuse                                                                          | Why it is banned                                                                                                                                                                                                                                                                                                             |
| ------------------------------------------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| "It's just anti-aliasing / MSAA / sample-count drift"                           | pixelmatch's AA detection already filters anti-aliased edge pixels before the diff count is reported — any flagged diff has AA excluded by construction. Hundreds of pixels, per-glyph drift, or block-shaped diffs are a real bug. If you truly believe it is edge AA, prove it: show a 1px-wide edge band and quantify it. |
| "Glyph outline differences"                                                     | Same lazy-excuse family; hides wrong glyph position / transform / coverage bugs.                                                                                                                                                                                                                                             |
| Percentage thresholds in pixel diffs                                            | They mask regressions smaller than the threshold. A private 5% comparator hid bug #582 for weeks. Diff is zero or the test fails with PNGs for inspection.                                                                                                                                                                   |
| Private "boutique" comparators (`composeOver`, `CountDifferingPixelsInRect`, …) | Use / extend `bitmap_golden_compare` instead — see donner-pixel-diff skill.                                                                                                                                                                                                                                                  |
| "Preexisting failure, not mine"                                                 | There is no such thing on this repo. A red test found while doing other work is now in scope: fix it or open + link a tracking issue explicitly.                                                                                                                                                                             |

## 6. Diagnosable tests (the ToTT standard)

A failing test must localize the bug **without a rerun** — the failure message is the diagnostic.
("ToTT" = Google's "Testing on the Toilet" testing-best-practices series.)

- Use `EXPECT_THAT(value, Matcher)` over `EXPECT_TRUE(a == b)`. `EXPECT_TRUE` prints "false";
  a matcher prints expected vs. actual and names the offending part.
- Promote repeated assertion shapes into a named `MATCHER_P` / `MATCHER_P4` with a
  `DescribeMatcher`-built description and a `result_listener` message naming the failing sub-part.
- Copyable templates in `donner/svg/renderer/tests/RendererGeode_tests.cc` (grep `MATCHER`):
  - `Rgba(rM, gM, bM, aM)` — per-channel sub-matchers; failure prints all four channels and
    which ones failed. Compose tolerances per channel:
    `EXPECT_THAT(center, Rgba(Near(187, 4), Eq(0), Near(188, 4), Near(255, 1)))`.
  - `RgbaEq(r, g, b, a)`, `Near(expected, tol)`, `Alpha(m)`, `IsTransparent()`.
  - A simpler `MATCHER_P5(RgbaNear, r, g, b, a, tol, ...)` lives in
    `donner/editor/tests/RenderElementToBitmap_tests.cc` if uniform tolerance is enough.
- gtest quirk: a type declaring `operator<=>` must also declare an explicit `operator==`, or
  gtest equality assertions break (`AGENTS.md`, Coding Style section).
- Mechanics: tests live next to their subject in `tests/` with an `_tests.cc` suffix; pass
  `variants = ["tiny", "text_full", "geode"]` to `donner_cc_test` for tier-sensitive tests —
  any test whose expected output depends on the renderer backend or text-shaping tier
  (emits `{name}_{variant}` wrappers so plain `bazel test //...` covers the lanes; BUILD.bazel
  authoring details: donner-build-test skill). Renderer tests are quiet under `LLM=1`
  (default in the in-repo agent settings); set
  `DONNER_RENDERER_TEST_VERBOSE=1` to re-enable pixel dumps — both env vars pass through
  bazel via `test --test_env=` lines in `.bazelrc`.

## 7. Perf bugs

The repro must measure **the exact latency the user observes** (click-to-first-pixel, per-frame
time), with an explicit budget assertion: `EXPECT_LT(measured_ms, budget_ms)`. "Works on my
laptop" is not verification — the test is.

Use `donner_perf_cc_test` (`build_defs/rules.bzl`, search `def donner_perf_cc_test`). It splits
one logical suite into two targets so runner-speed flakiness never gates PRs:

| Parameter          | Emitted target       | Tags             | When it runs                                                                                    |
| ------------------ | -------------------- | ---------------- | ----------------------------------------------------------------------------------------------- |
| `correctness_srcs` | `{name}_correctness` | normal           | PR gate — CPU-invariant counters (rebuild counts, compose counts), always on `bazel test //...` |
| `wallclock_srcs`   | `{name}_wallclock`   | `manual`, `perf` | nightly `.github/workflows/perf.yml` via `--test_tag_filters=perf`                              |

Shared setup goes in plain `srcs` (compiled into both). Copyable examples in
`donner/editor/tests/BUILD.bazel`: `filter_drag_repro_tests` (shared
`FilterDragReproTestUtils.{h,cc}`, correctness in `FilterDragReproCorrectness_tests.cc`,
wallclock in `FilterDragReproWallclock_tests.cc`) and `async_renderer_filter_group_perf_tests`.

Run a wallclock target locally by naming it explicitly (the `manual` tag only hides it from
wildcards): `bazel test //donner/editor/tests:filter_drag_repro_tests_wallclock`

Decision tree for a new perf assertion:

```
Is the metric CPU-speed-invariant (a counter: rebuilds, composes, cache hits)?
├─ YES → correctness_srcs — it belongs on the PR gate
└─ NO (wall-clock ms budget) → wallclock_srcs — nightly only
```

In practice most perf bugs need **both** halves: a wallclock test that reproduces the latency
the user observes, plus a correctness-side counter tied to the same root cause (rebuild/compose
count) so the PR gate catches a regression without waiting for the nightly lane —
`filter_drag_repro_tests` above is exactly this pairing.

Picking a budget that will not flake: measure first, then budget the threshold the user
perceives — not measurement-plus-epsilon. `SplashDragLatencyBudgetsOnRealRenderer` records its
measured baselines in a comment ("steady ≈ 0.2 ms" on the reference machine) yet asserts
`steadyAvgMs < 20.0`, because 20 ms is where a 60 Hz drag stream shows perceptible lag; budgets
that hug the measurement flake on slower CI runners. Record the measured numbers and date in a
comment (self-contained, per repo policy) so future tightening has a baseline.

**TRAP:** bazel's `manual` tag does NOT exclude explicitly-listed targets. CI's target
determinator lists wallclock tests by name when their deps change, so a slow runner would trip
budgets on unrelated PRs — `test:ci --test_tag_filters=-perf` in `.bazelrc` (search `test:ci`)
is the guard that filters them even when requested. Never "fix" a perf flake by loosening a
budget or re-tagging; move the assertion to the right side of the split.

An alternative split pattern (same idea, gtest-filter based) is
`donner/svg/compositor/BUILD.bazel`'s `COMPOSITOR_GOLDEN_PERF_TESTS` list: one source file,
`compositor_golden_tests` excludes the budget tests via `--gtest_filter`, and
`compositor_golden_perf_tests` (tagged `manual`+`perf`) runs only them. Exemplar budget tests
there: `SplashDragLatencyBudgetsOnRealRenderer`, `RealSplashDragLatencyOnTinySkia` — note their
`[PERF]` stderr lines separating measured numbers from the asserted budgets. Perf design depth:
`docs/design_docs/0030-geode_performance.md`.

## 8. Done-bar checklist

Before declaring a bug fixed — everything in section 1 (documented red run, fully green
`bazel test //...` with zero disabled/skipped/"preexisting-red" tests, commit message naming the
test file + test name or titled `attempt:`/`hypothesis:`, clang-format run), plus:

- [ ] No dead code left behind by the investigation — scaffolding, orphaned helpers, or files
      whose only callers are their own tests must be deleted (the dark-code cluster around
      bug #582 burned weeks of investigation on code no live binary executed).
- [ ] For editor visual/interaction bugs: the closing half of step 0's MCP pairing —
      re-verify the actual editor behavior through the MCP repro that opened the investigation,
      not just a green lower-level test (see donner-editor-debugging skill).

## 9. Related skills and docs

- **donner-build-test** — running `bazel test //...`, variants, CI configs.
- **donner-pixel-diff** — full `bitmap_golden_compare` API, pixelmatch rules, golden updates.
- **donner-editor-debugging** — `.rnr` replay, MCP-first QA workflow, screenshot generation.
- **donner-pr-ci** — target determinator, `ci:full-test` label, PR conventions.
- Depth docs: `docs/editor_visual_debugging.md`, `docs/deterministic_replay_testing.md`,
  `docs/design_docs/0030-geode_performance.md`, `.bazelrc`'s `test:ci` comment block.
