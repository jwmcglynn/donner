# Donner Project Instructions

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

- **Perf bugs**: the repro must measure the exact latency the user observes (e.g. click-to-first-pixel wall-clock, per-frame time). Put explicit budget assertions in the test (`EXPECT_LT(measured_ms, budget_ms)`) so regressions trip loudly. New perf tests should use `donner_perf_cc_test` so CPU-invariant correctness counters stay on the PR gate while runner-sensitive wall-clock budgets move to nightly `perf` targets. Don't settle for "works on my laptop" — the test itself is the verification.
- **UI bugs**: if the bug only manifests through the full editor event loop (mouse events, ImGui state, worker-thread ping-pong), write an instrumented UI-layer test that drives `AsyncRenderer`/`AsyncSVGDocument`/`RenderCoordinator` with the exact request-posting sequence the editor uses. Faithfully mirror the event flow — do not fabricate a prewarm phase that the real editor doesn't fire.
- **Iterating without a repro** wastes everyone's time. A bug you can't reproduce automatically is a bug you can't fix; a fix you can't verify automatically is a fix you can't ship. Manual "please run it and tell me what you see" cycles are a last resort, not a primary workflow.
- Reference tests:
  - `donner/editor/tests/AsyncRenderer_tests.cc`'s `AsyncRendererE2ETest` suite — examples of the full editor-flow harness (cold render → click-then-drag → steady-state drag frames, with wall-clock budgets).
  - `donner/svg/compositor/CompositorGolden_tests.cc`'s `SplashDrag*` tests — examples of compositor-level perf gates on the real `donner_splash.svg` via the `data` dep.
