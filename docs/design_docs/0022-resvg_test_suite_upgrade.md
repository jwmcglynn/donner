# resvg-test-suite Upgrade: The Great Rename and Beyond

Design doc for upgrading Donner's vendored `resvg-test-suite` from the 2022-02-12
snapshot we currently pin to the current upstream HEAD (late 2024), handling the
2023-05-05 restructure, the 2023-05-06 "Great Rename", the repo move from
`RazrFalcon` to `linebender`, and ~2.5 years of accumulated test additions and
golden updates — without losing the ~220 hand-triaged override entries that
encode everything we know about feature gaps, bugs, and spec-vs-resvg
disagreements.

## Motivation

Our pinned commit is **2.5 years old** and missing hundreds of new tests covering
features we care about (filter primitives, text, marker, mask, paint servers).
Upstream has also fixed golden bugs that we currently work around with custom
goldens. Staying pinned means:

- New SVG features ship without regression coverage
- Renovate PRs accumulate in a "too hard to land" backlog
  (`renovate/resvg-test-suite-digest` has been floating for a while)
- Our golden overrides may be masking bugs upstream has since fixed
- Every new contributor asks "why don't our tests match upstream's naming?"

The upgrade is large but it is **strictly better** than a slow rebase: we get
everything in one PR, one triage pass, one set of goldens to re-bless.

## Current State

### Vendoring

- **Repo**: `https://github.com/RazrFalcon/resvg-test-suite.git` (now redirects
  to `linebender/resvg-test-suite`)
- **Pinned commit**: `682a9c8da8c580ad59cba0ef8cb8a8fd5534022f` (2022-02-12)
- **Fetch mechanism**: `new_git_repository` at
  [`third_party/bazel/non_bcr_deps.bzl:113`](../../third_party/bazel/non_bcr_deps.bzl)
- **Overlay BUILD**:
  [`third_party/BUILD.resvg-test-suite`](../../third_party/BUILD.resvg-test-suite)
  — declares four filegroups from the **flat pre-restructure layout**:
  - `svg = glob(["svg/*.svg"])`
  - `png = glob(["png/*.png"])`
  - `images = glob(["images/*.*"])`
  - `fonts = glob(["fonts/*.{ttf,otf}"])`

### Test integration

- **Entry point**:
  [`donner/svg/renderer/tests/resvg_test_suite.cc`](../../donner/svg/renderer/tests/resvg_test_suite.cc)
- **Discovery**: `getTestsWithPrefix(const char* prefix, ...)` at
  `resvg_test_suite.cc:14` — iterates `@resvg-test-suite//svg` (non-recursive)
  and matches by filename prefix (`a-fill`, `e-textPath`, etc.)
- **Golden pairing** at `resvg_test_suite.cc:48` — `<svg stem>.png` in
  `@resvg-test-suite//png/`, unless overridden by `Params::overrideGoldenFilename`
- **Test suite instantiation**: ~74 `INSTANTIATE_TEST_SUITE_P` blocks across
  ~28 prefix groups
- **Canvas size**: forced to 500×500 for all tests via
  `params.setCanvasSize(500, 500)` at `resvg_test_suite.cc:34`

### Override inventory (current)

| Override | Count | Meaning |
|---|---:|---|
| `Params::Skip()` | ~77 | Feature not implemented, or known bug we haven't rooted out |
| `Params::RenderOnly()` | ~51 | Renders fine, pixel compare not meaningful (includes the 49 UB tests from jwmcglynn/donner#496) |
| `Params::WithThreshold(...)` | ~89 | AA artifacts, minor rendering diffs |
| `.onlyTextFull()` | ~15 | Needs HarfBuzz backend (`--config=text-full`) |
| `Params::WithGoldenOverride(...)` | ~40 | resvg's golden is wrong per spec; stored in `donner/svg/renderer/testdata/golden/resvg-*.png` |

**Total test entries**: roughly 1087 lines in `resvg_test_suite.cc`, with ~220
of them carrying non-default params. Each one represents a judgment call that
took real time and must survive the migration.

### Custom goldens

- Location: `donner/svg/renderer/testdata/golden/resvg-&lt;test-name&gt;.png`
- Documented in
  [`0009-resvg_test_suite_bugs.md`](./0009-resvg_test_suite_bugs.md) with per-bug
  Symptom → Root Cause → Ruled Out → Resolution format
- ~40 files; named using the **old** `a-*` / `e-*` convention

### Triage tooling

`tools/mcp-servers/resvg-test-triage/` — MCP server with
`analyze_test_failure`, `batch_triage_tests`, `detect_svg_features`,
`suggest_skip_comment`, and `generate_feature_report`. **All of its pattern
matching currently assumes the old `a-*` / `e-*` naming convention** and will
need to be updated in lockstep.

## Upstream State

### Repo move

- New canonical URL: `https://github.com/linebender/resvg-test-suite`
- Default branch renamed `master` → `main` in
  `3c2309ffde` (2024-10-29)
- Old URL still redirects, but the Bazel `new_git_repository` pin should point
  at the new URL to avoid Renovate confusion

### Key commits to migrate across

| Commit | Date | What it does |
|---|---|---|
| `7dea0ef591` | 2023-05-05 | **Restructure tests.** Flat `svg/` + `png/` → hierarchical `tests/<category>/<feature>/<NNN>.svg` with paired `.png` in the same leaf directory. `a-`/`e-` prefix dropped — the directory tree now encodes that distinction. `images/` → `resources/`. |
| `dd23ec1319` | 2023-05-06 | **The great rename.** Numeric `NNN.svg` → descriptive kebab-case (e.g. `001.svg` → `accumulate-with-new.svg`). Goldens renamed alongside. No mapping document — only git's rename detection tells you old → new. |
| `58104c8752` | 2023-05-06 | **Rename more.** Cleanup of the great rename. |
| `9b6f7d9f39` | 2023-05-06 | **Remove dummy tests.** Some of our entries will resolve to "deleted upstream". |
| 2023-05-15..2023-05-21 | | New filter/mask/text/clipPath/stroke-dasharray tests added |
| 2023-10-01 | | More marker/fill/svg tests; reference image updates |
| `d20d47b514` | 2024-08-20 | **Sync tests with resvg.** Large sync pulling in upstream changes |
| `3c2309ffde` | 2024-10-29 | master → main rename |
| `d8e064337f` | 2024-10-29 | **HEAD as of this design doc.** 1679 `.svg` files under `tests/`, 7 top-level categories (`filters/`, `masking/`, `paint-servers/`, `painting/`, `shapes/`, `structure/`, `text/`). |

### New layout sample

Before (what our code assumes):
```
svg/
  a-fill-001.svg
  a-fill-010.svg
  e-filter-013.svg
png/
  a-fill-001.png
  a-fill-010.png
  e-filter-013.png
images/
  ...
```

After (current upstream):
```
tests/
  painting/
    fill/
      #RGB-color.svg       # was a-fill-001.svg
      #RGB-color.png
      rgb(int int int).svg # was a-fill-010.svg (the UB test)
      rgb(int int int).png
      ...
  filters/
    enable-background/
      new.svg              # was filters/enable-background/009.svg
      accumulate-with-new.svg
      with-clip-path.svg
      ...
resources/  # was images/
fonts/
```

Note the descriptive filenames include characters (`#`, `(`, `)`, spaces) that
can be awkward in C++ test names — we'll need to sanitize them (GoogleTest
already does this for `TestNameFromFilename`; verify it still produces stable
identifiers).

## Goals

1. **Update to current upstream HEAD** (pinning to the commit, not a branch)
2. **Map every current override** from old name to new name, preserving intent
3. **Surface orphans**: entries whose tests were deleted upstream
4. **Surface new tests**: file list of .svg's not previously covered, for
   triage in a follow-up PR
5. **Keep the `a-`/`e-` prefix-based test grouping conceptually**, translating
   it to directory walks — contributors already think in those buckets
6. **Verify custom goldens still apply** — some may now be unnecessary because
   upstream fixed its goldens; others need their filenames updated
7. **Update the triage MCP server** so future failures get the right feature
   labels

## Non-goals

- **Triage new tests in the landing PR**. A few hundred new .svg files will
  have no entries. They'll use default params and most will fail. The landing
  PR gets a conservative Skip/RenderOnly sweep just to turn CI green; the
  real triage is tracked as a **fast-follow milestone** (see Work Breakdown
  below), not something that drifts into the backlog.
- **Switch to linebender's result tracking** (`results.csv`, `stats.py`,
  `site/svg-support-table.html`). Interesting, but scope creep.
- **Adopt upstream `check.py` pre-commit hook**. Ours are different.
- **Fix underlying rendering bugs** exposed by the upgrade. Each bug becomes
  its own issue, triaged alongside the new-test enabling work.

## Migration Strategy

### Phase 0: Build the rename mapping (one-time, offline)

The hardest part. No upstream migration document exists; the only source of
truth is git's rename detection across the two restructuring commits.

**Approach**: clone `linebender/resvg-test-suite` locally, run a script that:

1. Walks every `.svg` in the **old** pinned commit (`682a9c8`) listed in our
   `resvg_test_suite.cc` overrides
2. For each, runs `git log --follow --name-status --diff-filter=R
   682a9c8..d8e064337f -- <old-path>` to collect all rename events
3. Produces `tools/resvg_test_suite_upgrade/rename_map.json` mapping
   `old_filename → new_relative_path` (or `null` for deletions)

**Why `--follow` rather than diffing the tree commits directly**: the restructure
and great rename are two separate commits, plus `58104c8752 "Rename more."` and
some ad-hoc moves afterwards. `--follow` chains all of them.

**Caveats**:
- `--follow` only tracks one path at a time — the script has to loop.
- Git rename detection is similarity-based; files that were also edited in the
  same commit may show as delete+add instead of rename, requiring
  `--find-renames=50%` tuning.
- Some tests will have been deleted (`9b6f7d9f39 Remove dummy tests.`) and
  show as `null`. That's expected; the script should report them as
  "orphaned".

**Validation**: spot-check 20 entries manually against the GitHub commit
history, especially ones from `filters/enable-background/` where we saw the
`001 → new-with-region` style reshuffling (not order-preserving).

### Phase 1: Update vendoring

1. Update `third_party/bazel/non_bcr_deps.bzl:113` to pin `d8e064337f` (or
   whatever is HEAD at PR time) and change the remote to
   `https://github.com/linebender/resvg-test-suite.git`
2. Rewrite `third_party/BUILD.resvg-test-suite` to match the new layout:
   ```starlark
   filegroup(
       name = "tests",
       srcs = glob(["tests/**/*.svg", "tests/**/*.png"]),
       visibility = ["//visibility:public"],
   )
   filegroup(
       name = "resources",
       srcs = glob(["resources/*"], allow_empty = True),
       visibility = ["//visibility:public"],
   )
   filegroup(
       name = "fonts",
       srcs = glob(["fonts/*.{ttf,otf}"], allow_empty = True),
       visibility = ["//visibility:public"],
   )
   ```
3. Update `BUILD.bazel` test targets at
   `donner/svg/renderer/tests/BUILD.bazel:295-301` to reference the new
   filegroups (`tests` replaces both `svg` and `png`).
4. Delete the `:svg`, `:png`, `:images` filegroup names — with the flat layout
   gone, they no longer make sense. Prefer a single `:tests` filegroup.

### Phase 2: Rewrite discovery

`getTestsWithPrefix(prefix)` at `resvg_test_suite.cc:14` currently does a
non-recursive scan and prefix match. Two options for the new layout:

**Option A — Walk by directory (preferred)**

Replace the prefix API with a directory-scoped API:

```cpp
std::vector<ImageComparisonTestcase> getTestsInCategory(
    std::string_view category,        // "painting/fill"
    std::map<std::string, Params> overrides = {},
    Params defaultParams = {});
```

Callers become:

```cpp
INSTANTIATE_TEST_SUITE_P(Fill, ImageComparisonTestFixture,
    ValuesIn(getTestsInCategory("painting/fill", { ... })),
    TestNameFromFilename);
```

**Pros**: mirrors the new directory tree 1:1; no prefix ambiguity; cheap
`recursive_directory_iterator` on exactly one subtree.

**Cons**: `a-fill` and `a-fill-opacity` used to be separate prefixes; now
they're `painting/fill/` and `painting/fill-opacity/` — same outcome, so this
is actually cleaner.

**Option B — Keep prefix API but compute synthetic prefix** from the new path
(`painting/fill/foo.svg` → synthetic `a-fill-foo`). Lets the rename mapping
be used to keep override keys stable.

**Decision**: Option A. The synthetic-prefix bridge is clever but leaks the
old layout into the new code forever. Better to bite the bullet once.

Override-map keys become the new filename (e.g.
`"rgb(int int int).svg"` — yes, with parens). The rename map JSON from
Phase 0 feeds a codemod that rewrites every `{"a-fill-010.svg", ...}` entry
to its new form.

### Phase 3: Codemod the test entries

Write `tools/resvg_test_suite_upgrade/rewrite_test_entries.py` that:

1. Parses `resvg_test_suite.cc` with a line-oriented regex that matches
   the `{"a-foo.svg", Params...},  // comment` entries — the file is
   mechanically structured, so tree-sitter is overkill
2. Looks up each old filename in the rename map
3. Emits new entries with the new filename and preserved comment
4. Outputs a report:
   - **Migrated**: N entries
   - **Orphaned** (deleted upstream): list, each needs a decision
   - **Ambiguous** (rename map has multiple candidates): list
5. Groups the new entries by category directory and regenerates the
   `INSTANTIATE_TEST_SUITE_P` blocks in the order of the new tree

**Hand review of orphaned entries**: each one used to encode a reason
(Skip/threshold/bug comment). For each, either:
- Verify the test really is gone upstream and drop the entry, OR
- Find the new test that replaced it and re-target the override

### Phase 4: Handle new tests

Once the codemod lands, `bazel test` will enumerate all ~1679 tests. The ones
with no override will use default params, and many will fail. Strategy:

1. Run the suite once, collect failures into `new_test_failures.txt`
2. Use the resvg-test-triage MCP server (updated in Phase 6) to bulk-categorize
   them by feature
3. Add override entries in a **single bulk commit per category**, using
   conservative defaults:
   - Obvious "not impl" → `Params::Skip()` with `// Not impl: <feature>`
   - Passes with small diff → `Params::WithThreshold(0.1f)`
   - Large diff but renders → leave failing, add to triage backlog (**do not
     auto-skip**; skip/render-only needs human approval per project policy)
4. **Triage is a follow-up effort**, not part of the landing PR. The landing
   PR should end with a clean `bazel test //...` but may have many more
   entries marked Skip than before.

### Phase 5: Custom goldens

The 40 files in `donner/svg/renderer/testdata/golden/resvg-<old-name>.png`
need:

1. **Renaming** to match the new test names (via the rename map)
2. **Re-validation**: for each, diff against the new upstream golden. If
   upstream has since fixed the bug (e.g. marker direction, filter blur
   halo), **delete the custom golden** and trust upstream.
3. Update
   [`0009-resvg_test_suite_bugs.md`](./0009-resvg_test_suite_bugs.md) entries to reference
   the new names and note any that were retired.
4. Update `Params::WithGoldenOverride(...)` paths in `resvg_test_suite.cc` to
   the new filenames.

### Phase 6: Update triage tooling

`tools/mcp-servers/resvg-test-triage/` assumes `a-*` / `e-*` naming. Update:

1. Pattern matching in `detect_svg_features` — switch from filename prefix to
   directory-based category detection
2. Feature report generation in `generate_feature_report` — group by new
   category tree
3. Skip comment templates in `suggest_skip_comment` — no longer needs to emit
   `a-*` style paths

### Phase 7: Land

Final PR contents:
- `third_party/bazel/non_bcr_deps.bzl` — new commit + URL
- `third_party/BUILD.resvg-test-suite` — new layout
- `donner/svg/renderer/tests/BUILD.bazel` — updated data deps
- `donner/svg/renderer/tests/resvg_test_suite.cc` — fully rewritten with new
  entries, grouped by new categories
- `donner/svg/renderer/testdata/golden/` — renamed custom goldens (or
  deletions where upstream fixed things)
- `docs/design_docs/0009-resvg_test_suite_bugs.md` — updated
- `tools/mcp-servers/resvg-test-triage/` — updated
- `tools/resvg_test_suite_upgrade/` — keep the rename map JSON + scripts as
  historical artifacts (useful if we ever need to re-run)
- `MEMORY.md` note updates: test filter syntax, resvg test target location

## Risks

### R1: Rename map is incomplete or wrong

**Impact**: Entries silently drop their override; tests that used to Skip now
fail.

**Mitigation**: the codemod's "migrated count" must equal "old override count".
Any discrepancy fails the script. Manual spot-check of at least 20 entries.
Keep the rename map checked in so it's reviewable.

### R2: Upstream fixed something we have a golden override for, and our override now masks a Donner regression

**Impact**: silent loss of coverage.

**Mitigation**: in Phase 5, diff each custom golden against both Donner's
current output **and** upstream's new golden. If upstream matches Donner,
delete the custom golden. If upstream matches our custom golden but differs
from current Donner output, that's a Donner regression we need to investigate
before landing.

### R3: New tests expose a large crop of bugs that can't all be triaged in one PR

**Impact**: PR either lingers unmerged, or lands with hundreds of fresh skips.

**Mitigation**: explicit non-goal — triage is follow-up work. Landing criterion
is "bazel test //... green", not "every test passing". The post-landing state
has more skips; that's the honest representation.

### R4: File names with special characters (`#`, `()`, spaces) break tooling

**Impact**: GoogleTest name sanitization produces unstable IDs, or Bazel
globs miss files, or shell commands in scripts mis-parse.

**Mitigation**: verify `TestNameFromFilename` produces stable output for
`rgb(int int int).svg`. Bazel globs should be fine (they pass filenames
verbatim). Scripts must quote paths properly — write in Python, not bash.

### R5: Renovate PR conflicts mid-migration

**Impact**: Renovate opens a PR pointing at an even newer HEAD while our
branch is in flight.

**Mitigation**: close the renovate PR explicitly before starting; rebase to
the latest HEAD just before merging.

### R6: Lost triage context

**Impact**: Comments on entries (`// Bug: Kerning on textPath`) are the only
record of why something is Skip'd. If a comment doesn't survive the codemod,
we lose institutional memory.

**Mitigation**: codemod preserves the trailing `// ...` comment verbatim.
Validated by a round-trip test (parse → rewrite with identity mapping → diff
should be empty).

### R7: The upgrade exposes that several `Params::Skip()` entries were masking actual bugs

**Impact**: can't land the PR without also fixing bugs.

**Mitigation**: per project policy, we never skip tests to make CI green.
Any newly-exposed bug becomes a blocker issue and gets its own fix PR before
the upgrade lands. This is the correct outcome, even if it delays things.

## Open Questions

1. **Do we want to pin to a tag or to HEAD?** Upstream has no tags since 2018.
   Pinning to a specific commit is the only option, and Renovate will keep
   opening PRs. Fine.

2. **Should we keep the `svg/` + `png/` flat layout in our overlay BUILD file
   by symlinking, to minimize C++ changes?** Tempting but wrong — it hides the
   upgrade and makes future debugging harder. Do the real migration.

3. **Are the `resources/` files referenced by `xlink:href` inside SVGs?** If
   an SVG does `<image href="../../../resources/foo.png"/>`, the runfiles path
   has to be right. Need to check whether any test SVG uses relative asset
   refs and verify the Bazel filegroup layout supports them. Likely need
   `resources` as a sibling of `tests/` in runfiles.

4. **What to do about `images/` (old) assets that referenced tests may still
   point to?** If our pinned commit's SVGs reference `images/foo.png` and the
   new tree has those as `resources/foo.png`, pre-migration tests would break
   — but we're not doing an intermediate step, we're going straight to new
   HEAD, so this only matters if any individual SVG internally hardcodes the
   path. Audit needed.

5. **Goldens for the new Skia backend**: do we need to regenerate `-skia`
   suffixed goldens too, or are they under the same naming scheme? Check
   the legacy full-Skia golden handling.

6. **Should the design include upgrading `resvg-test-triage` to a language
   model-assisted triage flow** (e.g., feed failures to Claude with the SVG
   and diff image)? Out of scope for this doc but worth noting as a natural
   follow-up.

## Work Breakdown

Two milestones. **Milestone 1** is the landing PR — large but scoped to the
mechanical migration. **Milestone 2** is the fast-follow that actually enables
the new tests we just pulled in. Both are required for the upgrade to be
"done"; skipping M2 means we paid the migration cost without getting the
coverage benefit.

### Milestone 1 — Mechanical migration (landing PR)

Rough sequencing; each bullet is a self-contained unit of work within one PR.

1. **Prep**: clone upstream locally, verify `git log --follow` works across
   the restructure commits on a single file as a smoke test
2. **Phase 0**: write & run rename-map script; check in
   `tools/resvg_test_suite_upgrade/rename_map.json` + script
3. **Phase 1**: vendoring update + BUILD file rewrite (file compiles, tests
   still enumerate old entries — they'll fail, that's fine locally)
4. **Phase 3**: codemod the C++ test file (driven by rename map); validate
   orphan list manually
5. **Phase 2**: rewrite `getTestsWithPrefix` → `getTestsInCategory`
6. **Phase 5**: custom golden re-validation + rename
7. **First green test run**: `bazel test //...` — many new failures expected
8. **Conservative sweep for new tests**: enough Skip/threshold entries to
   get CI green, nothing more. No feature-level triage yet.
9. **Phase 6**: triage MCP server updates
10. **Phase 7**: land the PR (expect heavy review; it's a big diff)

**Exit criterion**: `bazel test //...` green on main. Probably hundreds more
Skip entries than before — this is acknowledged and the fix is Milestone 2.

### Milestone 2 — Enable new tests (fast-follow)

Kicks off the day M1 lands. Goal: work through every newly-added test and
either enable it (with a threshold if needed), flag it as a real bug, or
confirm it's a feature we don't implement yet. **Each category is its own
PR** — small, reviewable, and safe to ship independently.

11. **Generate the new-test inventory**: diff the post-migration test set
    against the pre-migration set; group by category directory. Produces a
    tracking issue with a checklist per category.
12. **Run the triage MCP server** across the new-tests inventory to
    auto-categorize failures (not impl / threshold / bug / font difference)
13. **Per-category enablement PRs**, in roughly this priority order (pick
    whatever's highest-value first):
    - `painting/fill`, `painting/stroke-*`, `painting/fill-rule` — core
      paint features, likely the fewest surprises
    - `shapes/*` — basic shape tests, should mostly "just work"
    - `structure/*` — `<svg>`, `<g>`, `<use>`, `<symbol>` edge cases
    - `paint-servers/*` — linear/radial gradient, pattern
    - `masking/*` — clip-path and mask
    - `filters/*` — largest and most likely to expose bugs; probably
      multiple PRs per filter primitive
    - `text/*` — most complex, run across both text simple and text-full
      backends per project convention
14. **For each enabled test that exposes a real bug**: open a tracking issue,
    leave the test failing or skipped with a link to the issue, and keep
    moving. The goal of M2 is coverage, not bug fixing — bug fixing happens
    in dedicated PRs afterwards.
15. **Milestone complete when**: every new test has an explicit status
    (passing / threshold / bug-linked skip / not-impl skip) and the project
    Skip count is back down to "known gaps only".

### Milestone 3 — Ongoing (not scoped here)

- Incremental bug fixes against the tracking issues created in M2
- Renovate auto-bumps for future upstream updates (should be painless once
  we're on the new layout)

## Extra Credit — Publishing Donner to the upstream support table

Once M2 is done and we're sitting on a real pass rate, we should publish
Donner as a column on [linebender.org/resvg-test-suite/svg-support-table.html](https://linebender.org/resvg-test-suite/svg-support-table.html),
the same table that shows resvg, Chrome, Firefox, Safari, Batik, Inkscape,
librsvg, svgnet, and QtSvg. That's the canonical place the SVG rendering
community looks for "how good is engine X at SVG?" — we want Donner on it.

### How the upstream table actually works (it's not what you'd expect)

The support table is **human-triaged**, not automated. The workflow is:

1. A Qt5/6 GUI tool at `tools/vdiff/` in `linebender/resvg-test-suite` shells
   out to each renderer binary per test SVG, collects the rendered PNG, and
   shows a side-by-side "reference vs all backends" view.
2. A human clicks **Passed / Failed / Crashed / Unknown** per test per backend.
3. Scores land as integers (`0`=unknown/UB, `1`=passed, `2`=failed, `3`=crashed)
   in `results.csv`, one column per backend.
4. `site/gen-html-tables.py` + `stats.py` regenerate the published HTML table
   and chart.svg from that CSV.

There is **no automated submission API**, **no CI integration**, **no
pixel-diff equivalence check**. It's a shipping spreadsheet.

### Ladybird's precedent: PR #55, still draft after 17 months

The best prior art for what we'd be doing is
[linebender/resvg-test-suite#55](https://github.com/linebender/resvg-test-suite/pull/55)
— *"Test Ladybird"*, by shlyakpavel (not a Ladybird maintainer). It's a
draft PR against the `vdiff` tool that:

- Adds a `Ladybird` entry to the `Backend` enum in `tools/vdiff/src/tests.h`
- Implements `renderViaLadybird` in `tools/vdiff/src/render.cpp` (~30 LoC;
  shells out to `headless-browser --screenshot=1 --width=500 --height=500 foo.svg`
  and reads `output.png`)
- Adds a `ladybirdPath` setting + file picker in `settings.{h,cpp}` and the
  settings dialog `.ui` forms
- Appends a `ladybird` column to `results.csv` and runs the triage manually

Maintainer RazrFalcon responded: *"That's the easy 25%. Not worth adding to
the main test suite yet. Let's wait for at least 50–60%."* The PR stalled
at 27.5% passing. No updates since November 2024. Lesson: **don't bother
opening the PR until we're ≥60%**.

Ladybird also has a long-open tracking issue
[LadybirdBrowser/ladybird#2306](https://github.com/LadybirdBrowser/ladybird/issues/2306)
for the integration. The only Ladybird-side change that shipped to support
the effort was
[LadybirdBrowser/ladybird#2323](https://github.com/LadybirdBrowser/ladybird/pull/2323)
— adding `--width`/`--height` flags to `headless-browser` to get 500×500
output matching the suite's canvas.

### Proposed Donner integration

Two separate deliverables, both in the Extra Credit bucket:

#### E1: Donner-side CLI for rendering single SVGs at 500×500

Make sure `donner_svg_tool` (or a sibling binary) can render a single SVG
to a PNG at 500×500 with the right flags:

```
donner-svg-tool render --width=500 --height=500 --output=/tmp/out.png foo.svg
```

This is mostly already present — verify the surface area, add `--width` /
`--height` if missing, ensure exit code is nonzero on crash (not just on
parse error), and stabilize the output path so vdiff can find it.

Also add a flag to select the backend (`--backend=skia` / `--backend=tiny-skia`)
so we can submit one column per backend, or merge them into the best result.
Probably cleaner to submit **two columns** — `donner-skia` and `donner-tiny-skia` —
so the table reflects reality.

#### E2: Upstream PR against linebender/resvg-test-suite

Following Ladybird's pattern in
[linebender/resvg-test-suite#55](https://github.com/linebender/resvg-test-suite/pull/55):

1. Add `Donner` (and `DonnerTinySkia`?) entries to the `Backend` enum in
   `tools/vdiff/src/tests.h` — `BackendsCount` increments, which cascades
   into the hand-maintained `.ui` forms. Budget the pain.
2. Implement `renderViaDonner` in `tools/vdiff/src/render.cpp`:
   ```cpp
   QStringList arguments = {
       "render",
       "--width=500", "--height=500",
       "--output=" + Paths::workDir() + "/output.png",
       data.imgPath
   };
   Process::run(data.convPath, arguments, true);
   ```
3. Add `donnerPath` field + settings dialog entries for the binary picker.
4. Locally run vdiff across all ~1680 tests, triage Passed/Failed/Crashed
   for every SVG. This is **hours of clicking**; budget a quiet afternoon.
5. Append `donner` column to `results.csv` with those triaged scores.
6. Open the PR. Expect maintainer pushback if we're under 60% passing.

**Gating**: don't spend time on E2 until M2 has driven the automated pass
rate above **60% of non-UB tests** (matches the bar RazrFalcon set on
Ladybird's PR). Our automated pixel-diff count will be an over-estimate
of manual Passed (pixel-diff fails AA artifacts; manual triage accepts
them), so the manual vdiff score will probably be higher than our pixel
pass rate. But the automated number is the cheap proxy to track.

### Non-goals for Extra Credit

- **Don't rewrite vdiff** to use Donner's pixel-diff harness. vdiff exists
  as shared community triage infrastructure; changing its model is
  out-of-scope for Donner.
- **Don't try to automate the PR** (e.g., a bot that re-runs + updates the
  CSV). Other backends don't do this, maintainers won't accept it.
- **Don't block M1 or M2** on Extra Credit. It only makes sense after M2
  gives us a stable ≥60% pass rate.

### Risk: the bar keeps moving

If we hit 60% passing and open the PR, the maintainers might say "come
back when you're at 80%." Budget for multiple rounds. The value of landing
in the table is community recognition, not a one-time thing.

## Appendix: Rename Map JSON Format

```json
{
  "a-fill-001.svg": "tests/painting/fill/\#RGB-color.svg",
  "a-fill-010.svg": "tests/painting/fill/rgb(int int int).svg",
  "e-filter-034.svg": "tests/filters/filter/in=FillPaint.svg",
  "a-some-removed-test.svg": null
}
```

The script that generates it must:
- Include every `.svg` referenced in `resvg_test_suite.cc` overrides
- Additionally include every `.svg` that existed in `682a9c8` (to catch tests
  that currently use default params — they might still be worth knowing
  about)
- Use git rename detection with `--find-renames=50%` and log any matches
  below 80% similarity for manual review

## Related Docs

- [resvg_test_suite_bugs.md](./0009-resvg_test_suite_bugs.md) — golden override
  rationale (will be updated in Phase 5)
- [text_rendering.md](./0010-text_rendering.md) — text tests are a large chunk of
  the upgrade scope
- [ci_escape_prevention.md](./0016-ci_escape_prevention.md) — the migration PR must
  leave `bazel test //...` clean
