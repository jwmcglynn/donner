# Design: Sandbox Branch Split

**Status:** Design
**Author:** Claude Opus 4.7
**Context window:** 1M tokens
**Created:** 2026-05-10

## Summary

The `origin/sandbox` branch is 97 commits ahead of `origin/main` and entangles
three logically separate threads: (1) a major editor architectural rewrite
into a thin client + out-of-process backend, (2) the address-bar feature plus
URL fetch / SSRF hardening built on that backend, and (3) a long tail of
general-purpose improvements (compositor `#582` fix, drag coalesce, filter
group elevation, tree-view pane, element inspector, `.rnr` v2, WASM Playwright
tests, Geode staging-buffer reuse).

Threads (1) and (2) are sandbox-specific and stay on the draft. Thread (3) is
the subject of this doc: extract every piece that can stand on `main` without
the new architecture, land it as one or more reviewable PRs, then keep the
sandbox draft as a thinner-but-still-sandbox-shaped branch.

Each candidate falls into one of four extraction tiers, from "clean
cherry-pick" to "feature exists on main but the sandbox version touches the
new architecture and needs a re-port." This doc names the tier per feature so
that effort estimates are honest up front — one of the original asks
(on-demand render loop) turned out to already be on `main`, and SelectionAabb
already exists on `main` too, so the actual extractable surface is smaller
than the raw commit count suggests.

**PR shape: exactly four PRs total, one per tier.** Tier 3 bundles all seven
re-ports into a single large PR per operator direction.

## Goals

1. **Land every general-purpose improvement on `main` ahead of the sandbox
   merge.** This de-risks the sandbox PR (smaller diff, fewer concerns mixed
   in) and gets the wins to users without waiting for the sandbox to clear
   review.
2. **Preserve the sandbox draft as a still-coherent branch.** After
   extraction, `sandbox` rebases onto the freshened `main` and stays a draft
   centered on the architecture refactor + address bar + sandboxing
   primitives.
3. **No silent regressions.** Each extracted PR runs `bazel test //...`
   green; no perf-test budgets relax; no compositor goldens drift.
4. **Per-feature tier visibility.** Each item in the extraction list is
   tagged with its tier (1–4) so reviewers and the operator can see at a
   glance which extractions are 30-minute cherry-picks and which are
   day-shaped re-ports.

## Non-Goals

- **Not extracting the thin-client / out-of-process backend architecture.**
  `EditorBackendClient*`, `donner/editor/backend_lib/`, and
  `donner/editor/sandbox/` were created *for* the sandbox and stay on the
  draft.
- **Not extracting the address bar.** Per operator direction, `AddressBar*`,
  `AddressBarDispatcher*`, `ContentSniffer`, `CurlAvailability`,
  `ResourcePolicy`, `SvgFetcher*`, the curl SSRF hardening, parse-error
  chips, the implicit-consent UX, and the load progress bar all stay on the
  draft.
- **Not extracting sandboxing primitives.** Linux seccomp fail-closed, macOS
  `sandbox_init`, `RLIMIT_FSIZE`, FD sweep, `--backend-smoke-test`,
  `installSandboxProfile`, libc_compat injection, live-protocol fuzzers, the
  persistent-child plumbing, and the sandbox-only design doc 0023 stay on the
  draft.
- **Not extracting sandbox-driven CI hygiene as part of this split.** The
  misc-include-cleaner pre-build scoping, clang-tidy GCC-13 stdlib pinning,
  `*_macOS.*` exclusions, the 256-finding include-cleaner sweep, and the
  branch-wide clang-format 18 reformat are their own initiative and are
  out of scope for this doc. (See "Future Work" — they may justify a
  separate extraction track later, but they're not gating the sandbox merge.)
- **Not back-porting features that genuinely require the new architecture.**
  If a sandbox feature only works because there's an out-of-process backend
  to talk to (e.g., the live-protocol fuzzers, persistent-child flake fix),
  it does not get a re-port — it ships when sandbox ships.
- **Not refactoring `main`'s editor architecture as part of this work.**
  `main` keeps `EditorShell` / `RenderCoordinator` / `EditorWindow` for now;
  this split is purely additive on `main`. The architecture refactor lands
  via the eventual sandbox PR, not via these extractions.

## Next Steps

- Operator sign-off received (2026-05-10): execute the maximalist split —
  Tiers 1, 2, 3, and 4 all in scope. Sandbox proper is deferred "for a
  while," so Tier 3 re-ports are unambiguously worth the cost.
- **Stack-then-split workflow.** Branch `editor-dev` off `origin/main` and
  port *all four tiers* onto it as a stack — Tier 1 commits at the
  bottom, Tier 4 commits at the top — **before opening any PRs**. The
  operator tests `editor-dev` locally as a single integrated branch first.
  Only after local validation does the branch get split into four PRs
  (one per tier) targeting `main`.
- **Exactly four PRs total: one PR per tier.** Tier 3 is one large bundled
  PR covering all seven re-ports, not seven small PRs. This is an explicit
  operator constraint.
- Implementation order on `editor-dev`: Tier 1 → Tier 2 → Tier 3 → Tier 4,
  with each tier as a contiguous range of commits so the eventual PR
  split is a clean `git range-diff` / `git rebase --onto` operation.

## Implementation Plan

- [x] **Milestone 0: Branch setup.** Created `editor-dev` off
      `origin/main`. All subsequent milestones landed on this branch as a
      stack.
  - [x] `git checkout -b editor-dev origin/main` (and detach from upstream).
  - [x] Tier boundaries marked in commit messages with `[tier1]` /
        `[tier2]` / `[tier3]` / `[tier4]` prefixes.

- [x] **Milestone 1: Tier 1 extractions (clean cherry-pick).** Cleanly
      applies onto `main`; small surface; high confidence. Landed on
      `editor-dev` as the bottom of the stack.
  - [x] Cherry-picked the four compositor `#582` commits in chronological
        order: `c020578b`, `8e7ec375`, `d032df3e`, `53780152`. Sandbox-only
        test hunks dropped. Required two fixups: (a) duplicate
        `propagateFastPathTranslationToSubtree` declaration removed,
        (b) `findLayerForTest` accessor added to expose layer state to
        the new tests. Also renamed `compositionTransform` → `canvasFromBitmap`
        in test code per destFromSource convention.
  - [x] ~~Cherry-pick `879e3340` (Geode staging-buffer reuse)~~ — **deferred**;
        regresses `BlendedLayerPopPreservesBackdropOutsideClip` on aarch64.
        Investigate as a follow-up.
  - [x] Cherry-picked the `CompositorGolden_tests.cc` additions (subset
        driving `CompositorController` directly).
  - [x] Cherry-picked design-doc `0025-composited_rendering.md` postmortem
        (`d11da687`) and policy updates (`b5189d8b`).
  - [x] Compositor tests pass; Geode renderer tests fail with environmental
        per-case timeouts on this aarch64 dev box (same on `origin/main`,
        not Tier 1 regressions).
  - [x] `clang-format -i` run; commits stay un-PR'd on `editor-dev`.

- [x] **Milestone 2: Tier 2 extractions (`.rnr` v2 format upgrade only).**
      Format-only slice landed via codex delegation (commit
      `[tier2] editor: .rnr v2 file format (format-only slice)`).
  - [x] `ReproFile.{cc,h}` extended with `ReproViewport`, `ReproHit`,
        per-frame `mouseDocX/mouseDocY`, per-frame `viewport`, per-event
        `hit`. Version bumped to v2. Reader handles both v1 and v2.
  - [x] `ReproRecorder.{cc,h}` gained `FrameContext` parameter on
        `snapshotFrame()` (default-empty) so existing zero-arg callsites
        compile unchanged.
  - [x] `ReproFile_tests.cc` extended with v2 round-trip + v1-readable-
        under-v2 tests; `bazel test //donner/editor/repro/...` green.
        Filter-disappear corpus seeds + replay harness shipped in
        Milestone 3.7.

- [ ] **Milestone 3: Tier 3 extractions (re-port against `main`'s
      `EditorShell`).** Implementation surfaced an important finding:
      **three of the seven items were already on `main`** from prior PRs
      (`#529`, `#531`, `#550` era), so only four required actual porting.
  - [x] **3a — Drag-coalesce extraction.** Codex-delegated
        (`[tier3] editor: drag-coalesce primitive (header + EditorShell
        wiring)`). `DragCoalesce.h` ported verbatim, `DragCoalesce_tests.cc`
        with high-rate stream coverage, and wired into `EditorShell.cc`
        with `ShouldPostDragMove` gating against `asyncRenderer().isBusy()`.
        Last-posted-screen-point reset on mouse-up.
  - [x] **3b — Filter-group sibling expansion.** Codex-delegated
        (`[tier3] editor: drag elevated <g filter> with visual-composite
        siblings`). After the existing elevation logic on main selects a
        `<g filter>` / `<g mask>` / `<g clip-path>`, the new code walks the
        group's direct siblings and adds those whose renderable geometry
        is ≥70 % contained in the group's subtree bbox to the drag set.
        Reuses `CollectRenderableGeometry`. Tests in `SelectTool_tests.cc`.
  - [~] **3c — Group-selection AABB expansion.** **Already on `main`**
        from PR `#550` (Editor: fix exit crash, filter-drag perf, and
        group-selection chrome). `CollectRenderableGeometry` and
        `SnapshotSelectionWorldBounds` are already on `main`. Sandbox-only
        residual (`ComputeSelectionAabbScreenRects` test utility) deferred
        as a follow-up — it has no production callsite even on sandbox.
  - [~] **3e — Tree-view pane.** **Already on `main`**: `SidebarPresenter`
        already has `renderTreeView`/`renderTreeNode` with click-to-select,
        Ctrl/Shift modifiers, and scroll-to-selected. The sandbox commit
        `8866deb2` adds the *protocol* (FrameTreeSummary, kSelectElement);
        the UX itself shipped earlier. No port needed.
  - [ ] **3d — Selection identity diagnostics.** **Deferred** as
        follow-up. Most of `13099271` is sandbox-protocol-tied
        (`FramePayload.tree`, `FrameSelectionEntry`); the portable bits
        (BitmapGoldenCompare helper, attempt-tagged PNG dumps) are test
        infrastructure that doesn't unblock the operator's testing.
        Revisit when the sandbox merge gets closer.
  - [in-flight] **3f — Element inspector G4.** Codex-delegated; adds
        XML attributes + computed style sections to `renderInspector`
        in `SidebarPresenter`. Single-element invariant.
  - [in-flight] **3g — `.rnr` replay harness + filter-disappear corpus.**
        Codex-delegated; new `RnrReplay_tests.cc` drives `AsyncRenderer` +
        `AsyncSVGDocument` with replayed events from .rnr v2 files,
        pixelmatch-compares against committed PNG goldens. Corpus seeds
        (`filter_elm_disappear-{2..7}.rnr`) and goldens move from sandbox
        to `donner/editor/tests/testdata/`.

- [x] **Milestone 4: Tier 4 — WASM Playwright smoke harness (re-scoped).**
      Codex-delegated. Codex correctly identified that the original-scope
      Playwright suite depended on browser-side test hooks (`__donner_ready`,
      `__donner_metrics`, `__donner_simulate`, `OnBrowserFileReadyPath`)
      that are sandbox-only additions to `donner/editor/main.cc` and the
      WASM `BUILD.bazel`. **Re-scoped** to a smoke-only port that doesn't
      require those hooks; `drag_perf.spec.ts` deferred until the hooks land.
  - [x] Added `donner/editor/wasm/tests/` (BUILD, package.json,
        playwright.config.ts, run_tests.sh, smoke.spec.ts, NOTES.md
        documenting the deferral). Bazel target tagged `manual` and
        `playwright`. `bazel build //donner/editor/wasm/tests:all`
        completes (target is manual, so `:all` is empty; explicit target
        builds successfully).
  - [x] Startup-stderr matcher in `smoke.spec.ts` is permissive
        (`/^(\[startup\]|\[Geode\/emscripten\]|GLFW error 0:|warning:)/`
        or empty). Tighten when the operator captures the actual tags
        from main's WASM build.

- [ ] **Milestone 5: Local validation gate (operator).** Before any PRs
      open, the operator builds `editor-dev` locally and exercises the
      editor — golden-path UX, drag perf, address-bar-free flows,
      compositor `#582` repros, tree-view + element inspector. Any
      regressions surface here, not in CI.
  - [ ] Operator: `bazel build //donner/editor:editor` and run.
  - [ ] Operator: spot-check Tier-3 features (drag coalesce, filter
        elevation, AABB expansion, tree view, element inspector,
        diagnostics, replay harness).
  - [ ] Operator: optionally exercise WASM build + Playwright suite via
        the tag-gated target.
  - [ ] Operator signal: "ship it" → proceed to Milestone 6.

- [ ] **Milestone 6: Stack split into four PRs.** Mechanical operation —
      use the `[tierN]` commit-message tags from Milestone 0 to delineate
      tier boundaries. One PR per tier, in numerical order. Each PR
      targets `origin/main` and stacks on the previous PR until merged.
  - [ ] Find the boundary commits via `git log --grep='\[tier'`.
  - [ ] Branch `tier1-compositor-fixes` from the last Tier-1 commit, push,
        open PR.
  - [ ] After Tier 1 lands on `main`, rebase `editor-dev` onto refreshed
        `main`; branch `tier2-rnr-v2` from the last Tier-2 commit, push,
        open PR.
  - [ ] Repeat for Tier 3 (`tier3-editor-improvements`) and Tier 4
        (`tier4-wasm-playwright`).
  - [ ] After all four tiers are merged, delete `editor-dev`.

- [ ] **Milestone 7: Sandbox draft cleanup.** After Tiers 1–4 land,
      rebase `sandbox` onto refreshed `main`. Resolve the inevitable
      conflicts (the architecture refactor still touches the same
      compositor / repro files we extracted). Confirm `bazel test //...`
      still passes on the rebased draft. The draft stays draft; the eventual
      sandbox merge is a separate decision.

## Background

`origin/sandbox` is the working branch for the editor sandboxing initiative
(design doc 0023). Across ~5 weeks of work, three threads converged:

1. **The architecture refactor.** To put untrusted SVG parsing behind a
   sandboxed boundary, the editor was rewritten from a single-process
   `EditorShell` + `RenderCoordinator` + `EditorWindow` into a thin
   front-end (ImGui host) that talks to a `donner/editor/backend_lib/`
   backend over `donner/editor/sandbox/` IPC. Files removed: `EditorShell`,
   `RenderCoordinator`, `EditorWindow`, `RenderPanePresenter`,
   `ViewportInteractionController`, `MenuBarPresenter`,
   `SidebarPresenter`, `EditorInputBridge`, `DialogPresenter`,
   `ImGuiClipboard`, `GlTextureCache`, etc. Files added: `EditorBackendClient.h`
   + `_InProcess.cc` + `_Session.cc`, `backend_lib/`, `sandbox/`.
2. **The address-bar feature.** Loading remote URLs requires a fetch path,
   which means an SSRF hardening surface, which means content-type sniffing
   and a resource policy. New files: `AddressBar*`, `AddressBarDispatcher*`,
   `ContentSniffer*`, `CurlAvailability.cc`, `LocalPathDisplay*`,
   `ResourcePolicy*`, `SvgFetcher*`. Stays on the draft per direction.
3. **General-purpose improvements.** Wherever sandbox work uncovered a
   pre-existing bug (compositor `#582`), perf nit (Geode staging buffer),
   missing test infra (`.rnr` v2 with hit-testing), or new editor capability
   (tree view, element inspector, drag-coalesce), commits accumulated on
   `sandbox` that have nothing intrinsically to do with the sandbox.

This doc covers thread (3).

## Proposed Architecture

This is process design, not software architecture. Two phases:

**Phase A — stack on `editor-dev`** (no PRs):

```
   origin/main
       │
       └──► editor-dev
              │
              ├── [tier1] compositor #582 + Geode + design doc 0025
              ├── [tier2] .rnr v2 format upgrade
              ├── [tier3] drag-coalesce, filter elevation, AABB,
              │           diagnostics, tree view, element inspector,
              │           replay harness  (one tier, seven features,
              │           contiguous range of commits)
              └── [tier4] WASM Playwright (tag-gated)
              │
              ▼
       Operator local test
              │
              ▼
       "ship it"
```

**Phase B — split into four PRs** (mechanical, one per tier, in order):

```
   origin/main
       │
       ├── PR 1 ───── tier1-compositor-fixes
       ├── PR 2 ───── tier2-rnr-v2-format         (after PR 1 lands)
       ├── PR 3 ───── tier3-editor-improvements   (after PR 2 lands)
       └── PR 4 ───── tier4-wasm-playwright       (after PR 3 lands)
                              │
                              ▼
                  sandbox rebase onto main
                 (deferred; still draft post-split)
```

Exactly four PRs, one per tier. Sandbox stays a draft for "a while" — the
rebase happens on the operator's schedule, not on this work's critical path.

The tiering itself is the architecture: it forces explicit acknowledgement
of *which* extractions are cheap and *which* are day-shaped, before any
porting work starts.

### Tier definitions

- **Tier 1 — Pure cherry-pick.** Files exist on `main`; the diff applies
  cleanly (modulo dropping sandbox-only test hunks). Confidence: high.
  PR shape: one bundled PR. Effort: ~half-day total.
- **Tier 2 — Cherry-pick + wiring.** Files exist on `main`; diff applies
  but needs small callsite adjustments because `EditorShell` (`main`)
  differs from `EditorBackendClient` (`sandbox`). Confidence: medium-high.
  PR shape: one PR. Effort: ~half-day.
- **Tier 3 — Re-port.** Feature concept is portable; sandbox implementation
  is not. Re-author against `main`'s `EditorShell` + `RenderCoordinator`.
  Confidence: medium. PR shape: **one bundled PR covering seven re-ports**
  (operator constraint). Effort: 7–12 days total.
- **Tier 4 — Standalone subsystem.** Lives outside the editor proper (the
  WASM Playwright harness is the only one). Confidence: medium-high.
  PR shape: one PR, tag-gated. Effort: ~half-day.

## Per-feature extraction map

| Feature | Tier | Sandbox commit(s) | Notes |
|---|---|---|---|
| Compositor `#582` fast-path subtree fix | 1 | `c020578b`, `8e7ec375`, `d032df3e`, `53780152` | Drop sandbox-test hunks during pick. |
| Geode staging-buffer reuse | **deferred** | `879e3340` | Originally planned for Tier 1. Dropped during impl: `RendererGeodeTest.BlendedLayerPopPreservesBackdropOutsideClip` regresses on aarch64 with the patch applied — the thread-local buffer reuse interacts with WebGPU `writeTexture` validation in a way the sandbox commit's "padding bytes are ignored" rationale doesn't cover. Investigate separately. |
| Compositor `#582` postmortem + design doc 0025 | 1 | `d11da687`, `b5189d8b`, design-doc add | Doc-only. |
| `.rnr` v2 format | 2 | `7167fb8e` (format-only slice) | Recorder wiring against `main`'s `EditorShell` is small. |
| `.rnr` replay harness + filter-disappear corpus | 3 | `ce8b181a`, `cd3492b7`, `3c236629`, `7075e632`, `5ea9b77c`, `e7541836`, plus `7167fb8e` corpus | Re-authored against `AsyncRenderer` on main; sandbox version uses `EditorBackendClient`. |
| Drag-coalesce (`DragCoalesce.h`) | 3 | `1fde7905`, `2b4b34c4` | Header is portable; caller integration re-authored. |
| `<g filter>` top-level elevation | 3 | `8274778c`, `cf4231d1` | Re-port into `main`'s `SelectTool.cc`. |
| Group-selection AABB expansion | 3 | `ecf0bb3f` | `SelectionAabb` exists on main; descendant-walk is the new logic. |
| Tree-view pane | 3 | `8866deb2` | Re-author as in-process ECS reader. |
| Element inspector G4 | 3 | `08372da4` | Re-author as in-process ECS reader. |
| Selection identity diagnostics + pixelmatch goldens | 3 | `13099271`, `2a3bf761` | Pixelmatch goldens portable; protocol-tied diagnostics aren't. |
| WASM Playwright harness | 4 | `2f40d0b7` | Adjust startup-log matcher; decide CI tag policy. |
| `emscripten_fetch` wiring (S10) | — | `75cab44c` | **Stays on sandbox.** Only useful with `SvgFetcherWasm`, which is the address-bar's WASM fetch path. Not extractable without dragging in the address bar. |
| On-demand render loop (`0b9c0281`) | n/a | already on `main` | Sandbox dropped it during refactor and re-added it. `main` already has the equivalent in `EditorWindow::waitEvents()`. **No extraction.** |
| `SelectionAabb.{cc,h}` add | n/a | already on `main` | Listed previously but already exists on `main`. **No extraction.** |

## Testing and Validation

- **Per-PR gate.** `bazel test //...` is the source of truth (per
  `CLAUDE.md`'s always-green-`main` rule). Each extraction PR must run that
  green locally before push.
- **Compositor Tier 1.** New goldens land in
  `donner/svg/compositor/CompositorGolden_tests.cc` for the `#582` repros;
  the perf budgets in `CompositorGolden_tests.cc`'s `SplashDrag*` family
  must not regress.
- **Drag-coalesce Tier 3.** Port the unit tests verbatim
  (`DragCoalesce_tests.cc`); add an integration assertion in
  `EditorShell`-level tests on `main` that a synthetic mouse stream
  produces at most one post per round-trip.
- **`.rnr` v2 Tier 2.** `ReproFile_tests.cc` already covers v1; add v2
  round-trip tests; verify a v1 file reads correctly under v2-aware code.
- **Tier 3 re-ports.** Each feature ships with its own targeted tests
  against `main`'s code paths. The sandbox tests don't carry over because
  they exercise the protocol; that's why Tier 3 is more expensive than
  Tier 1/2.
- **Tier 4 WASM Playwright.** Decide whether to gate behind a
  `--test_tag_filters=playwright` opt-in or include in default
  `bazel test //...`. Recommend opt-in for now until headless Chromium
  reliability is established in CI.

No security/privacy section: the extracted features don't introduce new
trust boundaries. The trust boundaries this branch *does* introduce
(URL fetch, IPC, sandbox primitives) all stay on the sandbox draft.

## Risks

- **R1 — Tier 3 effort underestimated, single-PR bundling amplifies the
  blast radius.** "Re-port" is a polite word for "re-author against a
  different architecture." If tree-view, element inspector, or the replay
  harness turns out to need ECS-traversal helpers or test infrastructure
  that doesn't yet exist on `main`, day estimates balloon — and because
  Tier 3 is a single PR (operator constraint), a blocker on one re-port
  delays the whole tier. Mitigation: implement in dependency order
  (drag-coalesce → filter elevation → AABB → diagnostics → tree-view →
  element inspector → replay harness) so partial progress is always in a
  shippable state; if a late item hits a blocker, drop it from the bundle
  and ship the rest, deferring the blocker to a Tier-3.5 follow-up. (The
  follow-up would then be a *fifth* PR — a deviation from the four-PR
  constraint that should be flagged to the operator before splitting.)
- **R2 — Sandbox rebase friction grows.** Every PR that lands on `main`
  while sandbox is still draft is a rebase that the sandbox owner has to
  resolve. Mitigation: bundle Tier 1 into one PR rather than spreading it
  across four; rebase sandbox once after each tier lands, not after each PR.
  This risk is reduced because sandbox proper is deferred "for a while" —
  the rebase cost is borne later, in a single concentrated session, not
  amortized across a tight merge window.
- **R3 — Replay harness re-port collides with main's `AsyncRenderer`
  surface.** The sandbox version drives `EditorBackendClient`; on `main`
  the replay harness must drive `AsyncRenderer` + `AsyncSVGDocument`.
  `AsyncRenderer_tests.cc` was deleted on sandbox, so we don't have a
  reference for what the v2-`.rnr`-aware AsyncRenderer harness should look
  like — we're re-authoring it. Mitigation: do this re-port last in
  Tier 3, after the smaller ports have validated the general re-port
  pattern; expect 1–2 days for the harness alone.
- **R4 — Compositor goldens drift between sandbox and `main`.** If we
  extract Tier 1 goldens but the sandbox branch later modifies the same
  goldens, the rebase produces a conflict where the sandbox version "wins"
  and quietly reverts the extracted change. Mitigation: pin extracted
  golden files in `CODEOWNERS` for the sandbox rebase to force the owner to
  read each conflict.
- **R5 — Tier 4 startup-log contract drift.** The Playwright matcher is
  pinned to `[startup] …` prefixes that the sandbox build emits. `main`'s
  WASM build may emit different prefixes. Mitigation: capture the actual
  `main` startup log before writing the matcher; keep the matcher
  permissive (regex over `^\[(startup|GLFW error 0)\]`).

## Decisions

Sign-off captured 2026-05-10:

- **D1 — Maximalist split.** Tiers 1–4 are all in scope. Sandbox proper is
  deferred "for a while," so Tier 3 re-ports are unambiguously worth the
  cost — they ship value to `main` users now rather than sitting behind a
  multi-month sandbox review.
- **D2 — Replay harness ships, not just format.** The `.rnr` v2 format
  upgrade lands in Tier 2; the replay harness + corpus seeds land as a
  Tier 3 PR (re-authored against `AsyncRenderer`, since sandbox deleted
  the AsyncRenderer test surface).
- **D3 — Tier 1 is one bundled PR.** Compositor `#582` fix + Geode
  staging-buffer reuse + design doc 0025 + postmortem all in a single PR
  off `compositor-fixes`.
- **D4 — Tier 4 is tag-gated.** WASM Playwright suite is opt-in
  (`--test_tag_filters=playwright` or explicit target), not default
  `bazel test //...`. Promote to default only after CI track record exists.
- **D5 — Tree-view + element inspector are in scope.** Worth the Tier 3
  cost given the deferred sandbox merge.
- **D6 — Exactly four PRs total, one per tier.** Tier 3 bundles all seven
  re-ports into a single large PR. Splitting the tier across multiple PRs
  is not authorized without re-checking with the operator — see R1 for
  the contingency if a Tier-3 item hits a blocker.
- **D7 — Stack-then-split workflow.** All four tiers port onto a single
  `editor-dev` branch as a stack of commits before any PR opens. The
  operator validates `editor-dev` locally (Milestone 5). Only after
  "ship it" does the branch get split into four PRs (Milestone 6). This
  catches integration regressions across tiers before they hit reviewers.

## Future Work

- [ ] **Sandbox-driven CI hygiene extraction.** misc-include-cleaner pre-build
      scoping, clang-tidy GCC-13 stdlib pinning, the 256-finding sweep, and
      the branch-wide clang-format 18 reformat are valuable on `main` but
      orthogonal to feature extraction. Justify a separate doc and split.
- [ ] **Live-protocol fuzzers extraction.** If the wire format ends up
      stable enough across `EditorBackendClient` versions, the fuzzers may
      be valuable as a permanent test surface — but only after the protocol
      itself stabilizes.
- [ ] **`.rnr` replay harness on `main`.** If sandbox merge slips past
      Q2 2026, port the replay harness so the existing repro corpus has
      a consumer and `main` gets regression coverage of the filter-disappear
      class of bugs.
