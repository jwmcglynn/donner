# Design: Editor Geode Chrome Migration

**Status:** Draft
**Author:** Codex
**Created:** 2026-05-28
**Related:** [0044-editor_fluid_canvas_rendering](0044-editor_fluid_canvas_rendering.md)

## Summary

Editor ropes and chips are mostly ImGui chrome today. The next low-risk step is to move their
decorative rendering into a Geode-backed screen-space chrome layer while keeping ImGui responsible
for labels, hit testing, focus, and tooltips.

This gives the editor a visual and performance path toward Geode without forcing a full UI or text
migration immediately.

## Current Rendering Split

- **SVG content:** Geode texture in Geode editor builds.
- **Selection/path overlay, AABBs, handles, and marquee:** `OverlayRenderer` renders into an
  `svg::Renderer`; Geode builds produce a `RendererTextureSnapshot`, then ImGui presents it as an
  image.
- **Source-reference ropes:** ImGui draw list. `TextEditor::renderFocusReferenceLinks()` simulates
  the rope, converts it to a Donner `Path`, then `StrokeDonnerPath()` emits ImGui path and bezier
  commands.
- **Source style chips, reference chips, and selection-size chip:** ImGui draw-list rectangles plus
  ImGui text.

Final composition still goes through ImGui draw lists, but the expensive canvas and overlay texture
payloads are already Geode-backed.

## Next Steps

1. **Render source-reference ropes through Geode.** Add a clipped screen-space chrome layer and feed
   it the existing rope `donner::Path` geometry. This is the lowest-risk first move because the
   rope path construction already exists.
2. **Move chip decorative rendering to Geode.** Draw chip backgrounds, borders, glow, and connector
   flair through Geode. Keep chip labels in ImGui until Geode has a UI-grade text path.
3. **Keep ImGui as the interaction owner.** Preserve the current ImGui hit rects, hover state,
   keyboard focus, and tooltips as invisible interaction surfaces over the Geode-rendered chrome.
4. **Match existing clip behavior.** Clip the Geode chrome layer to the source text area or canvas
   pane, matching the current ImGui `PushClipRect` behavior so ropes and chips cannot bleed into
   adjacent panes.

## Guardrails

- The path overlay must remain in lockstep with the SVG content beneath it. Chrome migration must
  not introduce a separate cached overlay path that can drift during drag, pan, or zoom.
- Do not migrate chip text until Geode has UI-grade text rendering and comparable interaction
  semantics.
- Do not replace ImGui state ownership in this step. This migration is visual-first: Geode draws the
  high-churn decorative pixels, while ImGui keeps input semantics stable.
- Reuse the source-pane culling and clipping rules from
  [0044](0044-editor_fluid_canvas_rendering.md) so chrome cost remains bounded by visible UI.

## Acceptance Criteria

- Source-reference ropes can render through a Geode-backed screen-space layer with the same visible
  shape and clipping as the ImGui implementation.
- Chip backgrounds, borders, glows, and connector flair can render through Geode while labels still
  render through ImGui.
- Existing ImGui hit rects and tooltips continue to behave as they do today.
- No decorative chrome renders outside the source text area or canvas pane.
- The migration does not require a full editor UI/text rewrite.
