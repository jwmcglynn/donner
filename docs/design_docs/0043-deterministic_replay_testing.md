# Design: Deterministic Multi-Thread Replay Testing

**Status:** Implemented — shipped in [#602](https://github.com/jwmcglynn/donner/pull/602),
closing [#601](https://github.com/jwmcglynn/donner/issues/601).

## Summary

The GL replay harness drives recorded `.rnr` sessions through the real
`EditorShell::runFrame()` loop while the editor renders on a background
`AsyncRenderer` worker thread. Because a worker render lands relative to
wall-clock, tests that compared specific replay frames were chronically flaky
(the two `#601`-disabled subtests plus later accumulations). This design added a
deterministic replay framework: replay-only worker scheduling modes
(`DrainEachFrame`, `HoldFramesBehind`) that make render landing a function of
frame index, a content-only capture mode that excludes editor chrome, and a
worker-delay injection hook used to validate determinism. All of it is test-only
and defaults to production behavior. The `#601`-disabled
`gl_rnr_replay_tests` / `editor_layer_stress_tests` subtests were re-enabled (or
retired in favor of narrower coverage) as deterministic tests.

The shipped mechanism — API, scheduling modes, content-only capture, worker-state
hooks, and where the tests live — is documented in
[Deterministic Replay Testing](../deterministic_replay_testing.md).

The original full design (goals, adversarial premortem, per-milestone plan, and
the disabled-test inventory) is recoverable from git history.
