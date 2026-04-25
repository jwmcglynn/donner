End of the day.# Donner Project Instructions

## Pull Requests

- **Always squash-and-merge** when merging PRs. Use `gh pr merge --squash`.
- Never use merge commits or rebase-and-merge on this repository.

## AI Comment Convention

- **Always prefix AI-generated GitHub comments with 🤖.** This applies to all PR comments, review comments, and issue comments posted by any AI agent (Claude, Codex, Copilot, etc.).
- This distinguishes human comments from AI comments, since all AI activity goes through `jwmcglynn`'s GitHub account.

## Always-Green Main

- **`main` is always green.** There is no such thing as a "preexisting test failure" — any red test blocks merge, full stop. If something on `main` breaks, the next PR is fixing it, not routing around it.
- **Run `bazel test //...` before pushing any PR.** This is the single source of truth for local validation. Our goal is that `bazel test //...` catches every regression that CI would — if CI catches something local didn't, that's a gap to fix in the test surface, not a reason to skip the local check.
- **If `main.yml`'s bazel-diff target determinator looks wrong on a PR, add the `ci:full-test` label** to force the workflow back to full `bazel test //...` coverage for that PR.
- **When touching the CMake mirror or `gen_cmakelists.py`, also run `python3 tools/cmake/gen_cmakelists.py --check --build`.** Plain `--check` is intentionally fast and static; `--build` is the opt-in local compile gate that catches real CMake drift before CI does.
- The `tiny`, `text-full`, and `geode` variant lanes now run as `*_tiny` / `*_text_full` / `*_geode` wrappers under default `bazel test //...` (see `donner_cc_test(variants=…)` in `build_defs/rules.bzl`). The transitional `tools/presubmit.sh` wrapper has been retired — `bazel test //...` is the single command that gates a PR.
- **`misc-include-cleaner` runs inside `bazel test //...` for opted-in libraries** via the `include_cleaner = "strict"` attribute on `donner_cc_library`/`_test`/`_binary` (see `build_defs/include_cleaner.bzl`). Default is off because of historical debt (issue [#559](https://github.com/jwmcglynn/donner/issues/559)); `tools/run_misc_include_cleaner_diff.sh` keeps the diff-only CI net for non-opted-in code. When you clean a directory's includes, flip the macro to `"strict"` so future regressions are caught locally, not in CI.

## Debugging Discipline

When debugging bugs — **especially performance or UI bugs** — write an automated test that reproduces the bug BEFORE attempting a fix. No fixes without repros.

- **A regression test is only valid if it FAILED on the broken code.** Run the test at HEAD *before* applying the fix, capture the failure output (diff PNG, pixel count, error) and verify it matches the user-reported symptom. Commit the test on its own commit first so CI records a red→green transition. If you can't get the test to fail at HEAD, the test is wrong — not the bug. "Plausible-sounding fix without a red→green transition" is an attempt, not a fix — title the commit `attempt:` / `hypothesis:` and do not mark the issue closed.
- **User pushback is automatic evidence the test was wrong.** When the user reports a bug is still present after a claimed fix, the default response is "the test that verifies my fix is wrong or missing — writing a new one." Never reply with "I don't see why it would still be broken" or ask the user to re-confirm steps. The user's repro *is* the signal; your test is what needs debugging.
- **Perf bugs**: the repro must measure the exact latency the user observes (e.g. click-to-first-pixel wall-clock, per-frame time). Put explicit budget assertions in the test (`EXPECT_LT(measured_ms, budget_ms)`) so regressions trip loudly. New perf tests should use `donner_perf_cc_test` so CPU-invariant correctness counters stay on the PR gate while runner-sensitive wall-clock budgets move to nightly `perf` targets. Don't settle for "works on my laptop" — the test itself is the verification.
- **UI bugs**: if the bug only manifests through the full editor event loop (mouse events, ImGui state, worker-thread ping-pong), write an instrumented UI-layer test that drives the live backend path (`EditorBackendCore` + `CompositorController`) with the exact request-posting sequence the editor uses. Faithfully mirror the event flow — do not fabricate a prewarm phase that the real editor doesn't fire.
- **Iterating without a repro** wastes everyone's time. A bug you can't reproduce automatically is a bug you can't fix; a fix you can't verify automatically is a fix you can't ship. Manual "please run it and tell me what you see" cycles are a last resort, not a primary workflow.
- Reference tests:
  - `donner/editor/sandbox/tests/EditorBackendGoldenImage_tests.cc`'s `FilterDisappearRepro7*` suite — full thin-client flow (`.rnr` → `EditorBackendCore` → pixelmatch diff vs `svg::Renderer::draw`) with inspectable diff PNGs.
  - `donner/svg/compositor/CompositorGolden_tests.cc`'s `SplashDrag*` tests — compositor-level perf gates on the real `donner_splash.svg` via the `data` dep.

## Pixel-Diff Tests

- **Use `donner/editor/tests:bitmap_golden_compare`** (`CompareBitmapToBitmap` / `CompareBitmapToGolden`) + pixelmatch for every bitmap comparison. Do NOT roll a private `composeOver` / `CompositeOver` / `CountDifferingPixelsInRect` helper in the test file — those "boutique" comparators hid bug #582 behind a 5% threshold for weeks.
- **Do NOT use percentage thresholds.** They mask regressions smaller than the threshold and scale with scene size. Either the diff is zero (identity) or the test writes `actual_*.png` / `expected_*.png` / `diff_*.png` to `$TEST_UNDECLARED_OUTPUTS_DIR` for operator inspection.
- If a new pixel-diff test needs composition the helper library doesn't provide, extend `bitmap_golden_compare` — do not inline a private variant in the test.

## Bug-Fix Commit Discipline

- **Commits claiming `Fixes #NNN` / `closes #NNN` must name a test file + test name that failed at the parent commit and passes at this commit.** If the test was introduced in the same PR, an earlier commit in the series must show it failing (red→green sequence on the branch).
- **"Plausible-sounding fix without a documented red→green transition" is an attempt, not a fix.** Use `attempt:` or `hypothesis:` in the commit subject and do NOT close the issue — a human reviewer decides when the evidence is sufficient.

## No Dead Code, Refactor In-Place

- **Never leave dark or dead code.** Orphaned `.cc`/`.h` files whose only callers are their own tests are dead code per the always-green-main rule. They pass CI (their tests still work), but they actively harm debugging — investigators grep for symbols that no live binary executes and burn hours chasing ghosts. The `EditorShell` / `GlTextureCache` / `RenderPanePresenter` / `ExperimentalDragPresentation` cluster that soaked up weeks of the #582 investigation is the worst-case example.
- **Refactoring must be incremental and in-place.** Modify the existing type/function/module step by step, landing each step on `main`. Do NOT build a parallel new implementation alongside the old one with the intent to "switch over later" — the old path inevitably persists, accumulates "we'll delete this in the migration" TODOs, diverges, and becomes the dark-code trap above. If the change really is too large for an in-place sequence, write a design doc describing the migration strategy and the deletion milestones, and hold each deletion as a blocking gate.
- **A rebase/merge commit that enumerates "deleted files" must actually delete them.** The `Category A (deleted files kept deleted)` pattern (60052563) is banned — either land the deletions in the same commit, or open a tracking issue for each orphaned path and link it so the omission is visible.
