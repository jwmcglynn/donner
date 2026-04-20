# Design: CI Escape Prevention Refresh (April 2026)

**Status:** Design
**Author:** Claude Opus 4.7 (MiscBot)
**Created:** 2026-04-20
**Tracking:** Follow-up to [0016](0016-ci_escape_prevention.md); companion to [0029](0029-ci_runtime.md)

## Summary

Since [doc 0016](0016-ci_escape_prevention.md) landed Phase 1 (CMake `--check`, banned-pattern lint, retry-on-network) on 2026-04-07, new escape classes have emerged and one Phase 2 item (`misc-include-cleaner`) is 13 days overdue. 11 of the last 25 `main.yml` runs failed, dominated by `geode-dev` + `mac-re`. Issue [#552](https://github.com/jwmcglynn/donner/issues/552) — a macOS-Metal heap UAF in `GeodeDevice::counters_` plus iterator invalidation in `RendererDriver::traverse` — took multiple days to triage because ASan is not on the PR gate. This doc catalogues the post-0016 escapes, adds **Category 8: Sanitizer-only bugs** to the 0016 taxonomy, and commits to a prioritized punch list.

## Goals

- Catch the #552 class of bug (heap UAF / iterator invalidation) at PR time, not after merge.
- Eliminate `misc-include-cleaner`-detectable header-graph escapes (#520 macOS hotfix chain).
- Stop relying on post-merge hotfixes for perf-test wall-clock flakes (5 hotfixes in the last 48h of that episode).
- Close CMake-mirror drift gaps that `--check` missed (`geode_counters`, `renderer_bench`, `tracy_wrapper`, missing `<vector>`).

## Non-Goals

- Rewriting the perf-test framework. (Separate doc if needed.)
- Adding a full GPU farm. Driver/Metal flakes on shared CI runners stay out of scope — `target_compatible_with` gating is the answer.
- Making `bazel test //...` with all sanitizers the default on every PR (too slow — see 0029).

## Next Steps

1. Land `misc-include-cleaner` in `.clang-tidy` + required `lint.yml` job (0016 Phase 2, overdue).
2. Gate `//donner/svg/renderer/geode/...` under `--config=asan` on PRs that touch that path (`paths:` filter in `main.yml`).
3. File tracking issues for the remaining punch-list items (see Implementation Plan).

## Implementation Plan

- [ ] **Milestone 1 — Quick wins (S effort, close immediate gaps)**
  - [ ] Enable `misc-include-cleaner` in `.clang-tidy`; wire `--config=clang-tidy` into `.github/workflows/lint.yml` as a required check. Fix the existing violations surfaced.
  - [ ] Add `asan-geode` job to `main.yml` scoped via `paths:` to `donner/svg/renderer/geode/**` and `donner/svg/renderer/RendererDriver.*`, running `bazel test --config=asan --config=geode //donner/svg/renderer/...:resvg_test_suite_geode`.
  - [ ] Extend `tools/presubmit.sh` with `--variants` flag that runs `tiny` / `text-full` / `geode` tiers; document in `CONTRIBUTING.md`.
  - [x] Update 0016 with Category 8 (wall-clock perf) + Category 9 (sanitizer-only); link #552 comment thread with the repro command (`ASAN_OPTIONS=abort_on_error=1:halt_on_error=1`). _(Landed in this branch, commit 1dd1b49d.)_
- [ ] **Milestone 2 — Generator + perf hardening (M effort)**
  - [ ] Extend `tools/cmake/gen_cmakelists.py --check` to invoke `cmake --build` in a sandbox for Linux + macOS tiers (catches drift the static validator misses).
  - [ ] Introduce `donner_perf_cc_test` macro in `build_defs/rules.bzl` splitting correctness counters (PR-gate) from wall-clock thresholds (nightly, tagged `perf`).
  - [ ] Add nightly `sanitizers.yml` running ASan+UBSan on `//donner/...`, required for `main` merges (not PR-blocking). **Gate the run on "commits on `main` since the last successful nightly"**: first job calls `git log origin/main --since="yesterday"` (or compares SHAs against the last `workflow_run`) and exits 0 if empty, short-circuiting the expensive jobs. Avoids burning CI minutes on idle days.
- [ ] **Milestone 3 — Process (S effort, do last)**
  - [ ] Quarantine lint: any PR that adds `flaky = True` must reference a tracking issue; enforce via GH Action.
  - [ ] ReadabilityBot/GeodeBot rule: raw pointers into objects owned by a different `shared_ptr` chain → review flag. (Narrow but prevents #552 twin.)

## Background

See [0016](0016-ci_escape_prevention.md) for Categories 1–6. This refresh adds:

**Category 7: Wall-clock-gated perf tests on shared CI runners.** `AsyncRendererTest`, `FilterDragBench`, `CompositorBench` were written with ~2 ms thresholds on dev hardware; shared GHA macOS runners measure ~40 ms. Five threshold-widening hotfixes (8043ad7b, 1f147f2f, 43f42cf7, ab68092b, 8cd89ef7) in 48 h.

**Category 8: Sanitizer-only heap/UB bugs.** `bazel test //...` uses default config. Heap UAFs, iterator invalidation, ODR violations surface only when a developer hand-runs `--config=asan` or `--config=ubsan`. **#552 is the canonical example**: UAF in `GeodeDevice::counters_` was invisible under the default build and manifested as random `SIGSEGV` at `tiny_free_list_remove_ptr` / `lvp_CreateBuffer` → `calloc`. Under ASan the exact free/use pair is identified in <1 s.

## Escape Evidence Since 2026-04-07

| Commit / PR | Escape | 0016 Category |
|---|---|---|
| 80c33ef8 → 13fcd20d | macOS build broken for 3 hotfix attempts after #520 EnTT modular headers (missing `<ostream>`) | 2 (hardened by Phase 2) |
| 64ab81f0 | `text_engine` conditional dep broke `tiny` variant after #517 | New: variant-mode misconfig |
| 5f66ea4f, 469bf576 | Linux-only sandbox tests ran on macOS (missing `target_compatible_with`) | 2 |
| 19d41df8, 398d312b, 89448ad7, a7d682fe | CMake mirror drift (`renderer_bench`, `geode_counters`, `tracy_wrapper`, missing `<vector>`) that `--check` did not catch | 1 (Milestone 2 expands `--check`) |
| feed4260, f306a846, 26c5f683 | #514 transform-origin shipped with broken pixel diffs; hotfixes disabled tests post-merge | 4 |
| 8043ad7b, 1f147f2f, 43f42cf7, ab68092b, 8cd89ef7 | Perf-test flakes on shared macOS runners | **7 (new)** |
| 79b819ac, 8059ac55, 1b8c85f7 | Geode resvg variants disabled due to Mesa llvmpipe / Metal watchdog | 5 |
| #552 (branch `geode-dev@beadf3893`) | feImage heap UAF + iterator invalidation | **8 (new)** |
| dfc7ccad | `PersistentChild` flake due to Tracy linkage in sandbox backend | 5 |

## Proposed Architecture

The PR gate stays tight and fast; the nightly gate catches the expensive classes.

```
           PR push                                         Nightly on main
              │                                                  │
              ▼                                                  ▼
  ┌────────────────────────┐                   ┌────────────────────────────┐
  │ lint.yml (required)    │                   │ sanitizers.yml (required   │
  │  + misc-include-cleaner│                   │   for main merges)         │
  │  + clang-format        │                   │  --config=asan //donner/...│
  ├────────────────────────┤                   │  --config=ubsan //donner/..│
  │ main.yml (required)    │                   ├────────────────────────────┤
  │  default config //...  │                   │ perf.yml (tagged tests)    │
  │  +paths-scoped asan-   │                   │  wall-clock thresholds     │
  │    geode job for Geode │                   │  tracked via ci_timing_rep │
  │    path changes        │                   │  ort.py                    │
  │  cmake.yml (build, not │                   └────────────────────────────┘
  │    just --check)       │
  └────────────────────────┘
```

The scoped `asan-geode` job is the only new PR-required job; it only runs when Geode renderer files change, keeping runtime cost bounded while catching #552-class bugs before they reach `main`.

## Testing and Validation

- **Milestone 1 validation:** reproduce the #552 crash in CI by reverting beadf3893 on a dry-run PR and confirming `asan-geode` fails. Verify `misc-include-cleaner` flags the known `<ostream>` omission from #520 on a revert.
- **Milestone 2 validation:** `gen_cmakelists.py --check` must flag the pre-fix state of 19d41df8 / 398d312b. Nightly `sanitizers.yml` must pass a clean `main` within one week of landing.
- **Metric:** track `main` red-run rate; target <20% over 30 days (baseline 11/25 = 44%).

## Dependencies

- `.clang-tidy` already exists; `misc-include-cleaner` is built into clang-tidy 19+.
- Bazel `--config=asan` / `--config=geode` / `--config=ubsan` all exist.
- No new external deps.

## Open Questions

- **Perf-test tagging:** should the nightly perf job fail `main` or just file an issue? Start with issue-only, escalate to blocking after stabilization.
- **Skip-idle heuristic:** "no commits today" is simple but misses cases where dependencies (Bazel registry, Dawn) auto-update. Initial heuristic: `git log` only; revisit if we miss real regressions.
- **ASan on macOS Metal:** Metal validation layers sometimes false-positive under ASan — may need a specific `bazel test --config=asan` allowlist for Geode targets.
- **Scope of `gen_cmakelists.py --build`:** full CMake build is slow; a compile-only (`cmake --build -- -k`) pass may suffice.

## Future Work

- [ ] ThreadSanitizer nightly once any threaded code lands.
- [ ] Per-backend pixel-diff threshold auto-calibration (Category 4 tightening).
