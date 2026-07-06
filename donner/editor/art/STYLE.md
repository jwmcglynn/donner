# Editor SVG art style spec (W6, QA-F7, QA-F22)

One family of SVG art, rendered by Donner itself, covering the editor's OS
cursors, toolbar tool icons, and small UI affordance/chip marks. Authoring them
against one spec is what makes the suites read as a set.

## The style law (operator, QA-F7)

Every cursor AND its toolbar tool icon share one theme: a **solid black core with
a white outline** (the classic Illustrator-style two-tone). A tool's toolbar icon
is the same glyph as its cursor. The pen cursor is the classic fountain-nib
silhouette. The ONLY exception is the hand (pan) cursor, which keeps its white
glove.

Affordance and chip marks (tree chevrons, layer/inspector icons, the text-region
rope chip marks) are NOT cursors or tool icons; they stay single-color tintable
masks (below), because they are recolored to the surrounding UI/chip color.

## How each suite rasterizes

- Cursors: through the `RotateCursorSet` pipeline (Donner render at 4x, box
  downsample, GLFW cursor with a hotspot).
- Toolbar tool icons: through `EmbeddedSvgIcon`'s COLOR path
  (`RenderEmbeddedSvgIconColor`), which preserves the authored two-tone paint.
  Drawn with an identity (white) tint so ImGui does not recolor them.
- Affordance / chip icons: through `EmbeddedSvgIcon`'s mask path
  (`RenderEmbeddedSvgIcon`), which collapses RGB to a white alpha mask that the
  caller tints.

### Why two-tone icons via the color pipeline, not a silhouette mask (QA-F7)

The toolbar sits on a dark button, and a plain black silhouette would vanish on
it. The operator's law requires the icon itself to carry a white outline (the
same reason a cursor needs one to stay legible over any canvas artwork). A
single-color tintable mask is one channel of coverage tinted one color, so it
physically cannot show a black core AND a white outline at once - a
silhouette-mask approach would have to drop the outline and lean on button
contrast, breaking the shared theme. Rendering the authored two-tone SVG in full
color is therefore the only option that satisfies the law, and it makes the
toolbar icon pixel-for-pixel the same glyph as the cursor. Crispness at 24px: the
art rasterizes through Donner at 96px and displays at ~20px, so the ~1.5px (24
grid) white outline stays sharp at both 1x and 2x DPR, and because the outline
carries its own contrast the icon reads on idle, hover, and selected
(accent-fill) button states without tinting.

## Shared grid

- Cursors: `viewBox="0 0 32 32"`, `width="32" height="32"`. The pipeline
  substitutes the width/height to 128 (4x) before rendering; keep both
  attributes present and equal to 32 in source.
- Icons: `viewBox="0 0 24 24"`, `width="24" height="24"`. The 24 grid matches
  the Bootstrap affordance icons (LayersPanel / SidebarPresenter), so a tool
  icon and an affordance icon sit on the same optical weight.
- Keep art inside a 2px safe margin from the viewBox edge so downsampling never
  clips a stroke.

## Two-tone glyphs (cursors and toolbar tool icons)

- Black core `#000000`, white halo/outline `#ffffff`. Every filled glyph carries
  a white outline so it stays legible on any background.
- Filled glyphs: `fill="#000000"` with `stroke="#ffffff"`, halo `stroke-width`
  1.5 to 2 on the 32 grid (1.5 on the 24 grid). The select arrow, rotate, and
  pen nib use this.
- Line glyphs (pen slit, scale arrows, the type-tool T): draw the shape twice, a
  wider white pass under a narrower black pass (white ~3.35, black ~2 on the 32
  grid; white ~3.8, black ~2.4 on the 24 grid).
- Pen: the classic fountain-nib silhouette - a solid black leaf pointed at the
  tip (top-left hotspot) with a white slit from the tip and a small white vent
  hole near the shoulder. The pen toolbar icon and the pen_add/remove/close
  contextual cursors reuse this nib.
- `stroke-linejoin="round"`, `stroke-linecap="round"` everywhere.

## Single-color tintable masks (affordance and chip icons)

- Author in a single ink color (`#000000` fill / stroke); alpha carries the
  shape. `EmbeddedSvgIcon`'s mask path collapses RGB to a white mask that ImGui
  tints with the current text/affordance color, so the source color is
  irrelevant beyond coverage.
- No halo: these draw on flat UI, and the tint provides contrast. Stroke weight 2
  on the 24 grid, round joins/caps.
- Text-region rope chip marks (QA-F22): `chip_style_source_icon.svg` (a
  four-point sparkle) and `chip_overflow_icon.svg` (a six-spoke asterisk),
  replacing the missing-font sparkle/asterisk Unicode glyphs that rendered as a
  "?" placeholder. Tinted with the rope/chip color.

## Hotspots (cursor pointer origin, in 32-grid px)

| Cursor        | Hotspot | Notes                                              |
|---------------|---------|----------------------------------------------------|
| select arrow  | (5, 4)  | Arrowhead tip, top-left. No tail (unlike the icon).|
| pen           | (4, 4)  | Nib tip, top-left.                                 |
| pen add       | (4, 4)  | Base nib tip; `+` badge bottom-right.              |
| pen remove    | (4, 4)  | Base nib tip; `-` badge bottom-right.              |
| pen close     | (4, 4)  | Base nib tip; `o` badge bottom-right.              |
| rotate        | (16,16) | Centered; art rotated per corner by the pipeline.  |
| scale         | (16,16) | Centered double arrow; rotated per corner.         |
| path modify   | (6, 6)  | Angle vertex, top-left (Illustrator anchor tool).  |
| pan open/close| (15,15) | Palm center.                                       |

Note: the select TOOLBAR ICON is now tailless too (QA-F7 supersedes the earlier
tailed mask), matching the tailless select cursor.

## Adding art

1. Drop the `.svg` in this directory.
2. Cursors: add an `embed_resources` rule in `donner/editor/BUILD.bazel` and a
   case in `RotateCursorSet.cc` (`EditorCursor` enum + hotspot table + render
   dispatch). The completeness test iterates the enum, so a new cursor without
   art fails the build.
3. Toolbar tool icons: add the SVG to the `tool_icons_svg` embed target and a
   case in `ToolbarIconSet.cc` (`ToolbarIcon` enum + SVG map). Author two-tone;
   the registry-coverage test iterates the enum and asserts both tones render.
4. Affordance / chip icons: add the SVG to the `editor_icons` embed target and a
   case in the owning registry (e.g. `TextChipIconSet.cc`). Author as a
   single-color mask; the registry-coverage test fails on any id without art
   (guarding against a regression back to a "?" placeholder glyph).
