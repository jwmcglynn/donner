# Transform Inspector (ImGui) Developer Guide

## Overview
The Transform Inspector is an editor-integrated ImGui tool that visualizes and edits SVG transform
strings while showing before/after geometry bounds. It parses the current transform with the
project's `Transformd` utilities, renders overlays for rectangles or sampled paths, and can diff
against an optional reference parse. The tool is designed to keep dependencies minimal—no WebView
or Skia—and reuses existing math and path helpers such as `PathSpline`.

### Capabilities
- Edit transforms directly or via decomposed translation, rotation, scale, and skew controls that
  stay in sync with the raw string.
- Toggle parser angle units (degrees or radians) with a default of degrees and minimal additional
  options to keep the UI focused.
- View Donner and reference matrices side by side with per-cell deltas and transformed bounds.
- Sample rectangles or SVG paths into polylines for drawing original, parsed, and reference
  geometry/bounds overlays.
- Generate edge-case transform presets and keep a short history of recent inputs.
- Export a ready-to-paste gtest snippet containing the current transform, parsed matrix, and
  before/after bounds; copy or reset inputs with dedicated buttons.

## UI map and workflow
1. **Transform input and history**: The top section exposes the raw transform string plus a recent
   history dropdown (up to 10 entries). A default value of `translate(30,20) rotate(30)` is
   provided, and the geometry defaults to a rectangle at `(0,0,120,80)`.
2. **Geometry inputs**: Rectangle fields (x, y, w, h) are always available. An optional SVG path `d`
   string overrides the rectangle when present; invalid paths fall back to the rectangle to keep the
   view responsive.
3. **Parser options**: A single angle-unit toggle switches between degrees and radians. Additional
   parser switches are intentionally minimal and default to permissive, predictable behavior.
4. **Reference comparison**: A checkbox enables an independent reference parse (still using
   `Transformd`) and highlights per-entry matrix differences plus reference-transformed bounds.
5. **Decomposition editor**: Translation, rotation, scale, and skew sliders/inputs round-trip with
   the raw transform. Reset buttons restore canonical defaults per component or the entire
   transform.
6. **Edge-case helpers**: Buttons generate nested transforms, scientific notation, separator/space
   stress cases, or randomized parameters. Generated strings automatically enter history.
7. **Geometry overlay**: The canvas draws blue outlines for source geometry, green for original
   bounds, red for parsed bounds, and orange for reference bounds. Parse errors surface inline and
   leave the overlay in its last valid state.
8. **Test export and clipboard actions**: When parsing succeeds, **Copy gtest snippet** emits a
   `DestinationFromSource`-style test with matrices and bounds. Additional buttons copy the current
   transform or reset fields to defaults.
9. **Help panel**: An inline help section summarizes workflows, color cues, comparison tips, and how
   to use generators and export tools.

## Architecture notes
- **Class entry point**: `experimental/viewer/TransformInspector` owns all state and rendering. The
  viewer hooks it into the Tools menu and docking layout so it renders alongside other panels.
- **State**: Managed by `TransformInspector::State`, which keeps visibility, transform string,
  rectangle/path inputs, recent history, parser toggles, reference options, and the latest
  decomposition. Reference flags are persisted across renders in-process only.
- **Parsing and decomposition**: Parsing uses `TransformParser::Parse` with the selected angle unit
  option. Successful parses decompose to translation/rotation/scale/skew values to keep the
  decomposition editor and raw string synchronized.
- **Geometry sampling**: Rectangles become a single closed polyline. Paths are parsed with
  `PathParser` into `PathSpline` and sampled into polylines for overlay drawing. Bounds are derived
  from axis-aligned min/max over sampled points.
- **Transforms and bounds**: Parsed and reference matrices apply via `Transformd` to generate
  transformed polylines and bounds. Errors are captured per path so UI can continue rendering prior
  valid geometry when new input fails.
- **Test export**: Uses the currently parsed matrix/bounds and rectangle inputs to emit a snippet
  with `EXPECT_NEAR` assertions and an inline comment showing the transform string.

## Operating guidelines
- Keep dependencies minimal: avoid WebView and Skia; rely on `Transformd`, `PathSpline`, and base
  utilities that already exist in the codebase.
- Maintain short, readable transform strings when serializing from decomposition (three decimal
  places) to reduce visual noise in the UI and exported tests.
- Favor degrees for user-facing angles; radians remain available for parser validation via the
  toggle.
- When extending the tool, preserve the minimal option surface and default reset values so the
  inspector remains quick to use for debugging and test authoring.

## Usage tips
- Keep reference comparison disabled until needed to avoid extra parsing overhead.
- Use history entries to pivot quickly between edge cases while retaining the current geometry.
- For path debugging, start with a rectangle to verify overall behavior, then paste the path and
  re-run the comparison/export steps.
