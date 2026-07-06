# Donner Self-Hosted Vectorization Loop

An idea-file for a coding agent that vectorizes a raster reference image into a
clean, editable SVG using Donner itself as the render oracle. No third-party
rasterizer, no cloud tracing service: the same renderer Donner ships answers
"does this SVG look like the target?"

Related KB pages: `Donner Self-Hosted Vectorization Workflow`,
`Donner v1.0 Splash Reference`, `Hand Vectorization`, and the v1.0 splash
tracing milestone in `Backlog.md`.

## The loop

```
  edit SVG  ->  render (donner-svg)  ->  score  ->  inspect diff.png  ->  edit again
```

Each pass is one `vectorize.py` invocation. You (the agent) own the SVG editing;
the harness owns render + score + visualization. Drive the RMSE down and the
diff-pixel percent toward zero while keeping the SVG editable (see cleanliness
rules).

1. **Edit the candidate SVG.** Add/adjust shapes, groups, gradients, paths.
2. **Render + score:** run the command below. It renders the SVG through
   `donner-svg` at the reference's exact pixel size and diffs the two.
3. **Read `score.json`:** check `rmse`, `diff_pixel_percent`, and `worst_tiles`.
4. **Open `diff.png`:** bright red = large per-pixel error, near-black = match.
   The heatmap tells you *where* to work next; `worst_tiles` gives coordinates.
5. **Repeat** until the score meets the target and the structure is clean.

## The oracles (where render + diff come from)

- **Render oracle:** `//donner/svg/tool:donner-svg`, the repo's own CLI renderer
  (the same target the release workflow builds). Build once:
  `bazel build //donner/svg/tool:donner-svg`. The harness auto-discovers the
  binary at `bazel-bin/donner/svg/tool/donner-svg`; override with `--renderer`.
- **Diff oracle:** implemented in `vectorize.py` in pure Python (stdlib only, no
  PIL/numpy so it runs anywhere python3 does). The repo's C++ pixel-diff dep
  (`pixelmatch-cpp17`) is a Bazel-only dev dependency with no importable Python
  binding and no standalone CLI, so the harness carries its own PNG codec plus
  RMSE / diff-pixel / per-tile scoring. Both images are composited over a chosen
  opaque background (`--background white|black|gray`) before comparison so
  transparency is handled deterministically.

## Score metrics (score.json)

| field | meaning | target |
|-------|---------|--------|
| `rmse` | root-mean-square per-channel error, 0..255 | drive toward 0 |
| `rmse_normalized` | `rmse / 255`, 0..1 | < 0.02 is a strong match |
| `diff_pixel_percent` | % of pixels whose max channel diff > `--threshold` | < 1% |
| `quality_score` | `100 * (1 - rmse_normalized)`, higher is better | > 98 |
| `worst_tiles` | worst NxN grid tiles by RMSE, with pixel x/y/w/h | use to target edits |
| `diff_png` | red heatmap of per-pixel error | inspect every pass |

Suggested milestones for a real trace:
- **Silhouette lock** (`quality` > 85): major shapes in the right place, right
  colors. The background glow and the largest masses dominate RMSE first.
- **Structure match** (`quality` > 95): facets, rim, wordmark all present and
  roughly correct; `diff_pixel_percent` in the low single digits.
- **Polish** (`quality` > 98, `diff_pixel_percent` < 1%): edges, gradients, and
  anti-aliased boundaries. Diminishing returns; stop when the diff is noise.

Do not chase RMSE past the point of editability. A perfect pixel match built
from path soup is a failure (see below); a 98 built from clean named groups is
the win.

## Common failure modes (from the v1.0 splash milestone)

- **Path explosion.** Thousands of micro-paths that match pixels but can't be
  edited. Prefer few grouped polygons/paths per region. If a region needs > ~50
  nodes to trace, question whether it should be a gradient or a mask instead.
- **Raster fallback temptation.** Embedding a base64 `<image>` of the reference
  (or a slice of it) trivially zeroes the diff and defeats the entire exercise.
  **Never embed rasters.** The harness cannot tell, but the deliverable is a
  vector source asset; a raster is an automatic fail.
- **Uneditable fragment soup.** Auto-tracer output with no groups, no IDs,
  duplicate points, and suspicious joins. Keep layers separated (background
  glow, shell, rim, ring, inner facets, wordmark) as named groups.
- **Chasing the background.** The soft glow/gradient background can eat huge
  RMSE if traced as regions. Model it as one radial/linear gradient or a blurred
  layer, not thousands of tiles.
- **Wordmark as outlines too early.** If the typeface is identifiable, keep the
  wordmark as live `<text>` for as long as possible; only convert to outline
  paths when the shape is final.
- **Global color drift.** A worst-tiles report that is uniformly warm usually
  means a fill/gradient stop is off across a whole region; fix the color before
  fussing over edges.

## Cleanliness rules (the SVG must stay a real source asset)

- Editable paths and primitives; no base64 rasters, ever.
- Named groups per visual layer; stable `id`s that survive iterations.
- Gradients/masks for smooth regions instead of many traced slivers.
- Keep node counts sane; simplify rather than over-fit anti-aliased edges.
- No renderer-specific junk; the file should round-trip through SVG optimization.

## Commands

Build the renderer once (from repo root):

```
bazel build //donner/svg/tool:donner-svg
```

Score one candidate against a reference:

```
python3 tools/vectorize/vectorize.py \
    --reference <reference.png> \
    --svg <candidate.svg> \
    --out <workdir>
```

Outputs land in `<workdir>`: `score.json`, `rendered.png` (the donner-svg
render), and `diff.png` (the error heatmap).

Useful flags: `--background white|black|gray` (composite background; use
`black` for the dark Geode splash), `--threshold N` (diff-pixel sensitivity),
`--grid N` (NxN worst-tile grid), `--worst K` (tiles reported),
`--width/--height` (render size override; defaults to the reference size),
`--renderer <path>` (explicit donner-svg binary).

### Starting a real vectorization session (the Geode v1.0 splash)

The reference raster is the `Geode` splash (1536x1024 RGB). It lives in the
zettelkasten, not in this repo:
`/Users/jwm/Documents/zettelkasten/assets/images/donner/geode-splash-v2.png`
(SHA-256 `0e536ed95bbbfc8aac1677fc425a53e5e41939b5407cd94db470ecae08a77246`).

```
# 1. Build the render oracle once.
bazel build //donner/svg/tool:donner-svg

# 2. Start from an empty/rough candidate and iterate. Use --background black
#    because the splash is near-black. Re-run after every SVG edit.
python3 tools/vectorize/vectorize.py \
    --reference /Users/jwm/Documents/zettelkasten/assets/images/donner/geode-splash-v2.png \
    --svg tools/vectorize/geode_candidate.svg \
    --out tools/vectorize/geode_run \
    --background black

# 3. Read geode_run/score.json, open geode_run/diff.png, edit the SVG, repeat.
```

Note: the diff is computed in pure Python; a 1536x1024 pass takes on the order
of tens of seconds. For fast early iterations, pass `--width 768 --height 512`
to score at half resolution, then drop the override for final polish.

## Worked example

See `example/`. `reference_source.svg` is rendered by donner-svg into
`reference.png` (the pretend "raster reference": a purple circle with a dark rim
and a "Donner" wordmark). Two candidate iterations show the score improving:

| iteration | file | rmse | quality | diff pixels |
|-----------|------|------|---------|-------------|
| v1 (rough: wrong-blue, offset, undersized, no rim, no text) | `candidate_v1.svg` | 58.57 | 77.03 | 29.57% |
| v2 (recentered, correct fill, rim added, wordmark added, named group) | `candidate_v2.svg` | 0.00 | 100.00 | 0.00% |

Reproduce:

```
cd tools/vectorize
# reference.png is committed; regenerate it if needed:
../../bazel-bin/donner/svg/tool/donner-svg example/reference_source.svg \
    --output example/reference.png --width 256 --height 256 --quiet

python3 vectorize.py --reference example/reference.png \
    --svg example/candidate_v1.svg --out example/run_v1
python3 vectorize.py --reference example/reference.png \
    --svg example/candidate_v2.svg --out example/run_v2
```

`example/run_v1/diff.png` shows the classic first-pass signature: a red crescent
where the circle is offset, a red ring where the rim is missing, and red text
where the wordmark has not been placed yet. `run_v2` is near-black: match found.
