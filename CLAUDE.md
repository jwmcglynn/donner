# Donner Project Instructions

## Pull Requests

- **Always squash-and-merge** when merging PRs. Use `gh pr merge --squash`.
- Never use merge commits or rebase-and-merge on this repository.

## AI Comment Convention

- **Always prefix AI-generated GitHub comments with 🤖.** This applies to all PR comments, review comments, and issue comments posted by any AI agent (Claude, Codex, Copilot, etc.).
- This distinguishes human comments from AI comments, since all AI activity goes through `jwmcglynn`'s GitHub account.

## Always-Green Main

- **`main` is always green.** There is no such thing as a "preexisting test failure" — any red test blocks merge, full stop. If something on `main` breaks, the next PR is fixing it, not routing around it.
- **No preexisting issues, ever. We fix every issue we encounter.** "This test was already red at the base commit / before my change / on another branch" is NOT an exemption — it is the next thing to fix, not a footnote to route around. Discovering a base-red test, a latent crash, a segfault, or any other defect while doing other work means that defect is now in scope: root-cause and fix it (or, if it is genuinely too large for the current change, open a tracking issue, link it, and say so explicitly — never silently leave it red or label it "preexisting, not mine"). A branch is allowed to carry red tests *transiently while actively being driven to green*, but the bar for "done" is always a fully-green `bazel test //...` with real fixes and zero disabled/skipped tests. Agents must not downgrade a red test to "preexisting" to declare success.
- **Run `bazel test //...` before pushing any PR.** This is the single source of truth for local validation. Our goal is that `bazel test //...` catches every regression that CI would — if CI catches something local didn't, that's a gap to fix in the test surface, not a reason to skip the local check.
- **If `main.yml`'s bazel-diff target determinator looks wrong on a PR, add the `ci:full-test` label** to force the workflow back to full `bazel test //...` coverage for that PR.
- **When touching the CMake mirror or `gen_cmakelists.py`, also run `python3 tools/cmake/gen_cmakelists.py --check --build`.** Plain `--check` is intentionally fast and static; `--build` is the opt-in local compile gate that catches real CMake drift before CI does.
- The `tiny`, `text-full`, and `geode` variant lanes now run as `*_tiny` / `*_text_full` / `*_geode` wrappers under default `bazel test //...` (see `donner_cc_test(variants=…)` in `build_defs/rules.bzl`). The transitional `tools/presubmit.sh` wrapper has been retired — `bazel test //...` is the single command that gates a PR.

## Transform Naming

- **Every `Transform2d` local, field, parameter, and struct member must be named in `destFromSource` form** — e.g., `bitmapEntityFromEntity`, `worldFromPreviousWorld`, `canvasFromDocument`. Names like `delta`, `xform`, `t`, `transform`, `mat` are banned: the destFromSource name *is* the documentation, and a value whose direction is encoded only in a comment will be composed wrong the first time it's reused. See `AGENTS.md` §"Transform Naming Convention" for the full rule and rationale.

## Formatting

- **Run `clang-format -i` on every modified C/C++ file before committing.** `git clang-format` covers staged changes. The project `.clang-format` (Google + 100-col, see `.clang-format`) is tuned so clang-format 18 and 19 produce identical output — use whichever is on your `$PATH`.

## Debugging Discipline

When debugging bugs — **especially performance or UI bugs** — write an automated test that reproduces the bug BEFORE attempting a fix. No fixes without repros.

- **A regression test is only valid if it FAILED on the broken code.** Run the test at HEAD *before* applying the fix, capture the failure output (diff PNG, pixel count, error) and verify it matches the user-reported symptom. Commit the test on its own commit first so CI records a red→green transition. If you can't get the test to fail at HEAD, the test is wrong — not the bug. "Plausible-sounding fix without a red→green transition" is an attempt, not a fix — title the commit `attempt:` / `hypothesis:` and do not mark the issue closed.
- **User pushback is automatic evidence the test was wrong.** When the user reports a bug is still present after a claimed fix, the default response is "the test that verifies my fix is wrong or missing — writing a new one." Never reply with "I don't see why it would still be broken" or ask the user to re-confirm steps. The user's repro *is* the signal; your test is what needs debugging.
- **Perf bugs**: the repro must measure the exact latency the user observes (e.g. click-to-first-pixel wall-clock, per-frame time). Put explicit budget assertions in the test (`EXPECT_LT(measured_ms, budget_ms)`) so regressions trip loudly. New perf tests should use `donner_perf_cc_test` so CPU-invariant correctness counters stay on the PR gate while runner-sensitive wall-clock budgets move to nightly `perf` targets. Don't settle for "works on my laptop" — the test itself is the verification.
- **UI bugs**: if the bug only manifests through the full editor event loop (mouse events, ImGui state, worker-thread ping-pong), write an instrumented UI-layer test that drives the live backend path (`EditorBackendCore` + `CompositorController`) with the exact request-posting sequence the editor uses. Faithfully mirror the event flow — do not fabricate a prewarm phase that the real editor doesn't fire.
- **Editor visual bugs**: follow [`docs/editor_visual_debugging.md`](docs/editor_visual_debugging.md) for `.rnr` replay, Geode direct-texture diagnostics, pixel crops, stack-layer boundaries, and failure signatures.
- **Editor path overlays must stay lockstep with presented document pixels.** A drag/zoom frame is
  wrong if the overlay uses a transform different from the shape pixels underneath it; preserve the
  same presented transform for both, or move both together.
- **Iterating without a repro** wastes everyone's time. A bug you can't reproduce automatically is a bug you can't fix; a fix you can't verify automatically is a fix you can't ship. Manual "please run it and tell me what you see" cycles are a last resort, not a primary workflow.
- Reference tests:
  - `donner/editor/sandbox/tests/EditorBackendGoldenImage_tests.cc`'s `FilterDisappearRepro7*` suite — full thin-client flow (`.rnr` → `EditorBackendCore` → pixelmatch diff vs `svg::Renderer::draw`) with inspectable diff PNGs.
  - `donner/svg/compositor/CompositorGolden_tests.cc`'s `SplashDrag*` tests — compositor-level perf gates on the real `donner_splash.svg` via the `data` dep.

## Pixel-Diff Tests

- **Use `donner/editor/tests:bitmap_golden_compare`** (`CompareBitmapToBitmap` / `CompareBitmapToGolden`) + pixelmatch for every bitmap comparison. Do NOT roll a private `composeOver` / `CompositeOver` / `CountDifferingPixelsInRect` helper in the test file — those "boutique" comparators hid bug #582 behind a 5% threshold for weeks.
- **Do NOT use percentage thresholds.** They mask regressions smaller than the threshold and scale with scene size. Either the diff is zero (identity) or the test writes `actual_*.png` / `expected_*.png` / `diff_*.png` to `$TEST_UNDECLARED_OUTPUTS_DIR` for operator inspection.
- If a new pixel-diff test needs composition the helper library doesn't provide, extend `bitmap_golden_compare` — do not inline a private variant in the test.

## Test Diagnosability: gmock + ToTT-style failures

- **Prioritize gmock matchers (`EXPECT_THAT` + matchers) over hand-rolled `EXPECT_TRUE(a == b)`-style assertions.** A failing test must localize the bug *without a rerun* — `EXPECT_THAT(pixel, RgbaNear(187, 0, 188, 255, 4))` prints the full expected-vs-actual RGBA and names the offending channel; `EXPECT_TRUE(pixel[0] == 187)` prints "false". This is the "Testing on the Toilet" (ToTT) standard: the failure message *is* the diagnostic.
- **Promote repeated assertion shapes into a named gmock matcher** with a good `DescribeTo` / `result_listener` message (e.g. the `Rgba(...)` / `RgbaNear(...)` / `Alpha(...)` pixel matchers in `RendererGeode_tests.cc`). A repeated `EXPECT_NEAR` per channel is a smell — make one matcher that reports all channels.
- Don't churn assertions that are already diagnosable; apply this to new/edited tests and anywhere a failure currently prints a bare boolean.

## Anti-Aliasing Is Never the Root Cause

- **Never attribute a pixel difference to anti-aliasing.** "AA quality", "MSAA drift", "sample-count difference", "4× vs 16× AA", "AA fringe", "supersampling difference" are **banned explanations** — in code comments, design docs, commit messages, test reasons, and chat. They are the universal lazy excuse that hides the actual bug: wrong glyph position, wrong transform/coordinate space, wrong coverage geometry, wrong color space, wrong premultiplication, wrong layer compositing. This is the same trap as "don't blame glyph outline differences."
- **Our pixelmatch comparison already excludes AA pixels.** pixelmatch's anti-aliasing detection runs by default, so anti-aliased edge pixels are *already filtered out* before a diff count is reported. Any diff our harness flags has, by construction, had AA removed — so "it's just AA" doesn't merely lack proof, it contradicts the tool. This rule exists because past AA hand-waving repeatedly masked genuine bugs that the diff was correctly catching.
- **Magnitude is the tell.** Genuine anti-aliasing differences are a thin sub-pixel fringe along edges — single-digit to low-tens of pixels, hugging boundaries. Hundreds of pixels, per-glyph drift, whole-shape offsets, or block-shaped diffs are a *real bug*, full stop. If a diff is large enough to gate a test off, AA is definitionally not the cause — find the real one.
- **Stating a sample count is fine; blaming it is not.** "Geode renders at 4× MSAA" is a true config fact. "The text suite fails because of the 4× MSAA AA gap" is a banned root-cause claim. Describe the *symptom* (e.g. "~700px/glyph positional drift vs tiny-skia, root cause not yet identified") and open the investigation — do not close it with "AA".
- If you genuinely believe a diff is edge-AA, prove it: show the diff is a 1px-wide edge band and quantify it. Absent that proof, treat "it's just AA" as an unfinished investigation.

## Bug-Fix Commit Discipline

- **Commits claiming `Fixes #NNN` / `closes #NNN` must name a test file + test name that failed at the parent commit and passes at this commit.** If the test was introduced in the same PR, an earlier commit in the series must show it failing (red→green sequence on the branch).
- **"Plausible-sounding fix without a documented red→green transition" is an attempt, not a fix.** Use `attempt:` or `hypothesis:` in the commit subject and do NOT close the issue — a human reviewer decides when the evidence is sufficient.

## No Rendering Vector Graphics With ImGui

- **Donner renders all vector graphics — ImGui never does.** Donner is a fully-integrated SVG stack; its entire premise is that it owns rasterization of paths, shapes, fills, strokes, gradients, and text. Synthesizing vector graphics with ImGui's draw-list primitives (`ImDrawList::AddConvexPolyFilled`, `AddPolyline`, `AddBezierCubic`, `AddCircleFilled`, `PathArcTo`, `PathFillConvex`, hand-rolled polygon/curve tessellation, etc.) to depict *document content* — layer thumbnails, shape previews, glyph outlines, icon silhouettes derived from SVG geometry — is **banned**. It bypasses the engine we exist to build, drifts from real render output, and hides bugs the actual renderer would expose. The Layers-panel thumbnail silhouette built from `AddConvexPolyFilled`/`AddPolyline` is the canonical violation this rule exists to kill.
- **The right pattern:** render the element/subtree through Donner proper (the SVG renderer / compositor) to a bitmap, then *display* that bitmap via an ImGui texture. Blitting a Donner-produced raster through `ImGui::Image` is fine — that is presentation, not vector rendering. If the renderer lacks an entrypoint you need (e.g. "rasterize a single element's subtree to a pixmap for a thumbnail"), **add the API to the renderer** rather than reaching for ImGui primitives.
- **Allowed ImGui drawing:** plain UI chrome that is *not* a depiction of SVG document geometry — panel backgrounds, separators, selection-row highlights, text labels, checkerboard transparency backdrops, resize handles, the editor's own widget furniture. The line is: if it represents the user's vector artwork, Donner draws it; if it's UI furniture, ImGui may.

## No Dead Code, Refactor In-Place

- **Never leave dark or dead code.** Orphaned `.cc`/`.h` files whose only callers are their own tests are dead code per the always-green-main rule. They pass CI (their tests still work), but they actively harm debugging — investigators grep for symbols that no live binary executes and burn hours chasing ghosts. The `EditorShell` / `GlTextureCache` / `RenderPanePresenter` / `ExperimentalDragPresentation` cluster that soaked up weeks of the #582 investigation is the worst-case example.
- **Refactoring must be incremental and in-place.** Modify the existing type/function/module step by step, landing each step on `main`. Do NOT build a parallel new implementation alongside the old one with the intent to "switch over later" — the old path inevitably persists, accumulates "we'll delete this in the migration" TODOs, diverges, and becomes the dark-code trap above. If the change really is too large for an in-place sequence, write a design doc describing the migration strategy and the deletion milestones, and hold each deletion as a blocking gate.
- **A rebase/merge commit that enumerates "deleted files" must actually delete them.** The `Category A (deleted files kept deleted)` pattern (60052563) is banned — either land the deletions in the same commit, or open a tracking issue for each orphaned path and link it so the omission is visible.

## DOM-Level Editing Only — No Source-String Manipulation

- **Every editor operation mutates the DOM (or higher), never the source text directly.** Edits go DOM-first; the structured-editing infrastructure reflects the DOM change back into the source text. The source is a *projection* of the DOM, not the thing edits operate on. Use the DOM mutation APIs (`SVGDocument::insertElement` / `removeElement` / attribute setters, etc.) — they return `ApplySourceEditResult` source deltas that the reflection layer applies for you.
- **Source-string surgery is banned for editor operations.** Computing new source bytes by string manipulation (extracting/moving/deleting/splicing source spans, or hand-building a new source string) and then reparsing is NOT allowed for structural edits (reorder, rename, insert/delete/move element, change attribute, group/ungroup, z-order, etc.). Reorder is `removeElement` + `insertElement` at the DOM level, not a text move. Rename is a DOM attribute change (with reference updates done on the DOM), not a find/replace over source.
- **Even user-generated text editing is DOM-aware.** The source/text-editor pane is the one surface where the user authors raw characters, but typing is NOT a blunt full-document replace-and-reparse. The goal is **incremental reparsing that updates the live DOM tree in place** — reparse the touched region and apply it to the existing tree, preserving entity identity wherever the edit allows, so the DOM stays the source of truth and downstream state (selection, compositor caches, references) survives the keystroke. The text pane is a DOM-editing surface like any other; "the user typed" never licenses a destructive source-replace, and it never licenses structural operations (reorder, rename, etc.) to skip the DOM and splice text.
- **Actions invoked from the text view must still be DOM-aware.** Dragging an element by a handle in the source pane to reorder it is a *DOM reorder* whose result the reflection layer writes back into the text — it is NOT a move of source-text spans. The text view is just another surface for issuing DOM operations.
- **Why:** the DOM is the single source of truth; structured editing keeps source and DOM coherent. Source-surgery shortcuts silently diverge from the DOM, break references/cascade, and bypass undo/validation. They are the structural-editing equivalent of the dead-code and ImGui-vector traps above.
