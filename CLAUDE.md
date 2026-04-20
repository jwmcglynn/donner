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
- `tools/presubmit.sh` is a transitional wrapper and will be retired once `bazel test //...` covers every variant (`--config=tiny`, `--config=text-full`, `--config=geode`) by default. Prefer `bazel test //...` today; do not add new logic to `presubmit.sh` that couldn't live in Bazel directly.

## Debugging Discipline

When debugging bugs — **especially performance or UI bugs** — write an automated test that reproduces the bug BEFORE attempting a fix. No fixes without repros.

- **Perf bugs**: the repro must measure the exact latency the user observes (e.g. click-to-first-pixel wall-clock, per-frame time). Put explicit budget assertions in the test (`EXPECT_LT(measured_ms, budget_ms)`) so regressions trip loudly. Don't settle for "works on my laptop" — the test itself is the verification.
- **UI bugs**: if the bug only manifests through the full editor event loop (mouse events, ImGui state, worker-thread ping-pong), write an instrumented UI-layer test that drives `AsyncRenderer`/`AsyncSVGDocument`/`RenderCoordinator` with the exact request-posting sequence the editor uses. Faithfully mirror the event flow — do not fabricate a prewarm phase that the real editor doesn't fire.
- **Iterating without a repro** wastes everyone's time. A bug you can't reproduce automatically is a bug you can't fix; a fix you can't verify automatically is a fix you can't ship. Manual "please run it and tell me what you see" cycles are a last resort, not a primary workflow.
- Reference tests:
  - `donner/editor/tests/AsyncRenderer_tests.cc`'s `AsyncRendererE2ETest` suite — examples of the full editor-flow harness (cold render → click-then-drag → steady-state drag frames, with wall-clock budgets).
  - `donner/svg/compositor/CompositorGolden_tests.cc`'s `SplashDrag*` tests — examples of compositor-level perf gates on the real `donner_splash.svg` via the `data` dep.
