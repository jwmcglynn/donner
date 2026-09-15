# Vectorization in Donner

## Establish the visual structure

Use the raster as evidence of the desired appearance, not as a collection of boundaries that
must all become paths. Determine which objects, materials, silhouettes, lighting changes, and
occlusions explain the image. Clarify fidelity or permitted artistic changes only when the
request does not already establish them.

Start with a small layer plan: background, ground/contact shadow, complete object silhouettes,
material faces, recessed regions, foreground objects, and highlights. Give important groups,
contours, paints, and independently editable objects meaningful IDs. Use short titles or
descriptions when they explain the role or paint order.

Retain useful supplied vector geometry. Inspect its actual document/artboard extent; a preview
or PDF-compatible page may omit useful off-artboard content. Preserve the original inputs.

## Paint complete objects

Create solid base shapes, then overlap smaller shapes above them. A material ring can be the
visible part of several nested filled faces; it need not be a fragile hollow outline. Each
crystal, leaf, letter, or other meaningful object should have a complete silhouette underneath
its visible planes.

Share contours used for clipping and visible boundaries. Extend surface facets behind the
containing mask and opaque foreground face so rasterized edges cannot expose the background
between them. Do not force every facet edge to trace the final curved outline. Use gradients
for broad lighting and separate angular planes where the material calls for faceting.

Shared vertices matter at genuine planar junctions. Move all incident faces together when a
junction changes. Collapse only redundant short segments or zero-area slivers, and verify that
neighboring faces do not invert or acquire gaps. A small base fill underneath the planes protects
against antialiasing seams; heavy strokes around every triangle can introduce bumps and should
not substitute for sound geometry.

Outlining text is useful when the goal is a portable illustration. Keep letters independently
editable and preserve deliberate symmetry; an oval counter should not inherit accidental wobble
from a raster approximation. Retain live text when editability of the wording is part of the task.

## Compare in one coordinate space

Register the raster to the SVG viewBox once and reuse that transform for comparison and comments.
Use Vector view to prove the artwork stands on its own, Raster view to inspect the source, and
Hybrid view to compare placement and silhouettes. In a version without those controls, use
separate Donner renders and the supplied reference without embedding the raster into the export.

Compare at two scales: full composition for proportions, balance, color and lighting; close-up
for corners, mask edges, small facets and accidental fragments. Work in short reversible batches.
A before/after comparison should make the intended local improvement easy to judge.

## Preserve the requested character

A request to polish intersections is not permission to replace a richly faceted object with a
few broad planes. Preserve object inventory, facet density, silhouettes, and lighting hierarchy
unless the user asks to simplify or redesign them. Small artistic liberty usually means aligning
corners, removing accidental fragments, or improving a contour while retaining the image's
recognizable structure.

Use comments to locate intent. When a pale wedge protrudes beside a crystal, the right repair may
be moving the neighboring dark triangles to the existing crown corner, while preserving every
foreground face. When tiny dark fragments appear along a light rim, check whether surface facets
stop short and expose the dark base; extend their fills behind the mask instead of adding a new
outline or recoloring unrelated shapes.

After a scope correction, restore the last accepted detailed version before trying a narrower
edit. Do not stack compensating changes on a rejected redesign.

## Separate artwork defects from editor defects

Validate IDs, internal references, finite coordinates, and polygon topology, then inspect actual
rendering. Source and render behavior are separate evidence. A newly inserted polygon can have
correct XML but empty live geometry; a changed clip can have correct geometry while a cached
consumer still shows old pixels. Verify creation and subsequent editing, not only save/reopen.

For a renderer discrepancy, isolate a minimal reproduction and use the project's shared pixel
comparison path. Keep expected results inspectable, and fix the geometry/invalidation/rendering
issue rather than raising visual thresholds or changing correct artwork to hide it.

Deliver readable vectors with semantic layering, gradients and masks where appropriate. Report
what changed and what was verified; preserve the user's reference and feedback separately from
the finished SVG.
