# Editor SVG art style spec (W6)

One family of SVG art, rendered by Donner itself, covering the editor's OS
cursors and toolbar tool icons. Cursors rasterize through the
`RotateCursorSet` pipeline (Donner render at 4x, box downsample, GLFW cursor
with a hotspot). Toolbar icons preserve authored colors through
`RenderEmbeddedSvgArtwork` and rasterize at 80 px for exact downsampling to
20 logical px at common 1x and 2x display scales. Authoring both against one
spec is what makes the two suites read as a set.

## Shared grid

- Cursors: `viewBox="0 0 32 32"`, `width="32" height="32"`. The pipeline
  substitutes the width/height to 128 (4x) before rendering; keep both
  attributes present and equal to 32 in source.
- Icons: `viewBox="0 0 24 24"`, `width="24" height="24"`. The 24 grid matches
  Bootstrap icon art (LayersPanel / SidebarPresenter), so a tool icon and a
  Bootstrap affordance icon sit on the same optical weight.
- Keep art inside a 2px safe margin from the viewBox edge so downsampling never
  clips a stroke.

## Cursors: two-tone for contrast on any canvas

- Black core `#000000`, white halo `#ffffff`. A cursor must stay legible over
  both light and dark artwork, so every filled glyph carries a white outline.
- Filled glyphs: `fill="#000000"` with `stroke="#ffffff"`, halo
  `stroke-width` 1.5 to 2 on the 32 grid.
- Line glyphs (pen, scale arrows): draw the shape twice, a wider white pass
  under a narrower black pass (white ~3.35, black ~2 on the 32 grid), matching
  the existing pen cursor.
- `stroke-linejoin="round"`, `stroke-linecap="round"` everywhere.

## Icons: two-tone contrast

- Use the cursor contrast system in the toolbar: black core `#000000`, white
  halo `#ffffff`.
- Filled glyphs use either a black fill with a 1.25 to 1.5 white outline on the
  24 grid or concentric white-outer and black-inner silhouettes when acute
  joins would let the core leak through the halo. Line glyphs use a wider white
  underlay with a 2 to 2.4 black core.
- Keep geometry simple enough to retain both opaque black and opaque white
  pixels after downsampling to 20 logical px. Raster tests reject clipped,
  single-tone, or empty artwork.
- Normal toolbar state preserves both colors. Disabled state applies a muted
  tint without replacing the black core.
- Expose only ready tools. Do not reserve a disabled toolbar slot for an
  unfinished interaction.

## Hotspots (cursor pointer origin, in 32-grid px)

| Cursor        | Hotspot | Notes                                              |
|---------------|---------|----------------------------------------------------|
| select arrow  | (5, 4)  | Arrowhead tip, top-left. No tail (unlike the icon).|
| pen           | (4, 4)  | Nib tip, top-left.                                 |
| pen add       | (4, 4)  | Base pen tip; `+` badge bottom-right.              |
| pen remove    | (4, 4)  | Base pen tip; `-` badge bottom-right.              |
| pen close     | (4, 4)  | Base pen tip; `o` badge bottom-right.              |
| rotate        | (16,16) | Centered; art rotated per corner by the pipeline.  |
| scale         | (16,16) | Centered double arrow; rotated per corner.         |
| path modify   | (6, 6)  | Angle vertex at the top-left.                      |
| pan open/close| (15,15) | Palm center.                                       |

## Adding art

1. Drop the `.svg` in this directory.
2. Cursors: add an `embed_resources` rule in `donner/editor/BUILD.bazel` and a
   case in `RotateCursorSet.cc` (`EditorCursor` enum + hotspot table + render
   dispatch). The completeness test iterates the enum, so a new cursor without
   art fails the build.
3. Icons: add a case in `ToolbarIconSet.cc` (`ToolbarIcon` enum + SVG map). The
   registry-coverage test iterates the enum.
</content>
</invoke>
