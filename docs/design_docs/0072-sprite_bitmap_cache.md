# Design: Sprite Bitmap Cache for External-Use Occurrences

**Status:** Draft
**Author:** Jeff McGlynn
**Model:** Withheld
**Created:** 2026-11-02

## Summary

A game that draws sprites from an external sprite sheet through `<use href="sheet.svg#sprite-N">`
draws about 150 occurrences per frame. Each one prepares the sheet for the occurrence, restyles or
reuses the referenced subtree, rebuilds the sheet's render tree when the previous occurrence
referenced another sprite, traverses the subtree, and draws every path, filter layer and group
in it. Measured on a game scene natively, the traversal of 141 such draws costs about 10 ms of
CPU per frame (7.4 ms of it in the sheet traversals and 1.4 ms in sheet preparation), and the GPU
fills every path of every occurrence.

[Cached filter results](0071-cached_filter_results.md) already render each filtered layer once
and draw the result at every occurrence. This design generalizes that to whole occurrences, the
technique Flash called `cacheAsBitmapMatrix`: rasterize everything an occurrence paints (paths,
gradients, clip paths, opacity groups and filters) once into a texture, and draw every later
occurrence with the same content and the same non-translation transform as one textured quad.
A cached occurrence does no sheet preparation, styling, render-tree work, traversal, font work,
path draw or filter execution.

## Goals

- An occurrence is rasterized once across N frames and M occurrences when only translations, or
  the `<use>` element's own opacity, change.
- It is rasterized again after any change to what it paints: a DOM mutation or font load in the
  sheet, a change to the style the referenced root inherits from the `<use>`, the `<use>`
  element's fill or stroke (context paint), its `x`, `y`, `width` or `height`, a `setHref`
  retarget, or a change to the non-translation part of the device transform.
- Cached frames match uncached frames exactly for whole-pixel motion over a transparent backdrop,
  and within one premultiplied level over other content (see Compositing over a backdrop);
  sub-pixel motion has the documented bounded error of cached filter results.
- Several static scales of one sprite each keep a rasterization; a continuously animated scale
  falls back to the uncached path without recording a rasterization per frame.
- Rasterizations share the cached filter results' memory bound, trim and idle rules.
- Content whose pixels depend on anything the key cannot name is never cached.

## Non-Goals

- Same-document `<use>` and plain groups: they have no content identity that survives a frame.
  Retained group layers are a separate compositor-level design.
- Atlas packing and instanced quads. Each cached occurrence is still one textured draw; batching
  them is the next step and reuses the same cache entries.
- Mipmapped entries for animated scale. An animated scale draws uncached, as before.

## Design

### Where the cache sits

`RendererDriver::drawExternalUse` probes the sprite cache before it touches the sheet. The probe
needs only data that is cheap to read from the host: the sheet's identity and revisions, the
referenced root, the `<use>` element's computed style, its fill and stroke, and its sizing
attributes. On a hit the renderer draws the cached texture at the occurrence's transform and the
driver returns. On a miss the occurrence draws as before; when the renderer admits it for
recording, the driver brackets the sheet traversal with `pushSpriteRecording` and
`popSpriteRecording`.

```
drawExternalUse
  probeCachedSprite ---- hit ----> renderer.drawCachedSprite: one textured quad; return
        | miss (Record or Uncached)
  style scope, instance layout, prepareSubDocument
  traverseSubDocument
    prepareFilterGraphs -> measureSprite (eligibility, bounds, resampling hint)
    [pushSpriteRecording]  traverse  [popSpriteRecording]
```

### Key

The key reuses `FilterResultContentKey` with a `sprite` flag, so sprites and filter layers share
one index, one memory bound and one eviction policy:

- `document`, `documentRevision`, `fontResourceRevision`: the sheet's identity and revisions. Any
  DOM mutation or font load in the sheet changes them.
- `styleEpoch`: the id of the occurrence's inputs (below).
- `instanceRoot`: the referenced element.
- `userToPixelScale`: carries the host rendering size, which the sheet's filter graphs scale
  lengths by. (`primitiveViewport` stays unset: primitive lengths resolve against the sheet's own
  viewports, which its revision and the occurrence's sizing cover.)
- `filterRegion`: the sprite's bounds in the `<use>` element's user space, grown by one device
  pixel at the occurrence's scale.
- The placement: the exact linear part of the device transform, plus the quarter-pixel bucket of
  the translation (or any fraction, below).

**Occurrence inputs.** The style an occurrence contributes is what the referenced root inherits
from the `<use>`: the `<use>` element's computed style reduced to inherited properties, its
resource opacities, its fill and stroke (none or a solid color; a paint server reference is not
cached, since it names host content), and, when it references a `<symbol>` or `<svg>`, the
viewport size it gives it: its `width` and `height` in pixels, resolved by the same layout helper
the instance layout uses (`LayoutSystem::resolveUseViewportSize`), against the host viewport, so
a percentage or a viewport unit follows the host viewport. The `<use>` element's `x` and `y` are
not inputs: they translate the `<use>` itself, which the draw transform carries, so moving a sprite
by its `x` attribute keeps it cached like moving it by `transform` does.
Each sheet keeps a table of recently seen
inputs, at most 64, in its registry context; each distinct set gets a process-unique id. Because
the inputs hold only inherited properties, the `<use>` element's own opacity, transform or blend
mode never change the id. They apply outside the cached content, exactly as they do uncached.
The one way a sheet reads a non-inherited host value is the `inherit` keyword on a property
that does not inherit, such as `opacity: inherit`, on the referenced root: style computation
marks such a root, and its occurrences are not cached. (Descendants that use `inherit` read the
root's value, which comes from the sheet.)

### Eligibility and bounds

The driver measures each set of inputs once per sheet revision, font revision and host rendering
size, during the first draw after one changed, once the instance's filter graphs are prepared. An
occurrence is cacheable when:

- its styles are retained for the occurrence (the cached filter result rule: no paint server
  reference from the host, no style resource limit hit) and the sheet has no animation;
- its root does not use `inherit` on a property that does not inherit, whether a style property
  or an element-specific one such as `transform` or `width`;
- every element in the referenced subtree is one a cached filter layer may hold (shapes, groups,
  gradients, clip paths, filter primitives), or the referenced `<symbol>` or `<svg>`;
- no element draws content from elsewhere: no mask, marker, pattern paint, text, image or nested
  `<use>`;
- every filter in the subtree is translation invariant: no turbulence, no `feImage`, and lighting
  only from a distant light (a point or spot light is positioned on the pixel raster; lighting
  has no kernel unit length, so its normals come from neighboring device pixels and move with
  the input);
- no element has a `mix-blend-mode` other than `normal`, which would read a backdrop the cached
  rasterization does not have.

The bounds are the union of the root's stroke-inclusive painted bounds and every filter region in
the subtree, in the `<use>` element's user space. One device pixel of margin covers anti-aliased
coverage. A clip path or a `<symbol>` viewport only shrinks what is painted.

### Recording and drawing

A sprite is recorded the way Flash's `cacheAsBitmap` renders a display object: in isolation,
into its own surface. `pushSpriteRecording` begins a capture sized to the sprite's whole bounds on
the device pixel grid, whether or not they lie on the canvas, and the driver does not cull the
sprite's content against the canvas while it records. So a sprite partly or wholly past the
canvas edge records all of what it paints, and every later occurrence can draw it. The capture
is a filter layer surface with an empty graph: the pop composites the captured content back
unchanged, records it into the shared index under the sprite key, and runs nothing. A capture is
not a filter: it is not admitted through the frame's filter budget, so it neither spends nor
latches it. Before it begins, it checks that the frame's surface budget holds the capture; when
it does not, `pushSpriteRecording` returns false having changed nothing, and the occurrence draws
exactly as it would uncached. A hit draws the texture with `drawFilterResult`: on the pixel grid
at the whole-pixel delta from where it was recorded, or resampled bilinearly at an exact
fractional offset when the placement allows it. The pending-until-submit commit, the thrash
guard, the record-time budget check, trimming and idle decay are those of cached filter results.

Admission is two-touch as for filter results: a miss at a linear transform records only after a
second miss at the same linear transform within the admission window, so a continuously
changing scale never records. Scale and rotation reuse a rasterization only when they match
exactly; there is no tolerance. Two-touch admission makes every new sprite of a scene record in
the same frame, so a frame begins at most 32 captures, whose surfaces take at most an eighth of
the frame surface byte limit; the rest record in the following frames, and a scene of a hundred
or so sprites warms within a few frames. A capture that can never begin (its surface would be
larger than any surface may be) is remembered for its key and never admitted again; since the
key changes with any revision or transform that could make it fit, nothing is lost. A filter
result whose region reached past the raster the canvas allowed is likewise not marked for
recording again for 30 frames.

### Compositing over a backdrop

Drawing a sprite from the cache flattens it first, which costs one extra 8-bit rounding wherever
the sprite is partly transparent and lands on content already drawn. The uncached path blends
each shape onto the canvas: an anti-aliased edge pixel becomes
`round(source * coverage + backdrop * (1 - coverage))`, rounded once. The cached path stores the
sprite on its own transparent surface first, which rounds `source * coverage` to 8 bits, and then
blends that onto the canvas, rounding again. Over a transparent backdrop the second blend is a
copy and the two paths match exactly; over an opaque backdrop the two roundings can land one level
apart. The same holds for pixels an opacity group or a semi-transparent fill leaves partly
transparent; fully covered, opaque pixels match exactly. This is the usual cost of caching a
group as a bitmap and is inherent to it: storing sprites at higher precision would remove the
extra rounding for a single shape but double their memory, and would still not match a sprite
whose own shapes overlap at partly transparent pixels, because flattening a group before blending
it is not associative under 8-bit rounding. `BackdropRoundingIsBounded` holds it to at most one
level.

### Sub-pixel placement

`setSpriteSubpixelPlacement` chooses how a sprite is placed at a fractional device translation;
`GeodeSurfaceRenderer` forwards it.

- `Exact`, the default: a sprite is kept per quarter-pixel bucket of its translation and drawn on
  the pixel grid, so sharp edges are never smeared; positions differ from an uncached render by
  less than a quarter pixel. A sprite content may fill all sixteen buckets of a linear transform,
  so sprites at many fractional positions, still or moving, hit once each bucket has recorded.
  When everything the sprite paints is one filtered element (outside it only plain containers,
  and no clip or mask on it) whose filter result may be resampled under the cached filter result
  rules (a final blur of at least one device pixel, fed from the source through per-pixel
  primitives only, fading inside its region), the sprite is resampled at any offset instead: one
  rasterization serves every position, and each premultiplied channel differs from an uncached
  render by a few levels at most (4 measured).
- `Resample`, as Flash smooths a cached bitmap: a sprite is kept once per content and linear
  transform and drawn bilinearly at its exact translation. It is recorded with one extra device
  pixel of transparent margin so its edges resample against transparency. A sprite moving by
  fractions of a pixel every frame hits every frame; the price is that sharp edges soften by up
  to half a device pixel, which moves coverage at edge pixels by up to about half (160 of 255
  levels bounds it in the test), while interiors match.

### Memory

Sprites and filter results share the index: at most 256 entries and the smaller of a quarter of
the frame surface byte limit and 64 MiB native, or 16 MiB on WebAssembly, where a phone browser
reclaims a tab whose GPU process grows. On a game scene of about 125 sprites at a 1206x2622
canvas, resampled sprites hit as often under 16 MiB as under 25 MiB, so the bound stays. Each kind has its own
share: sprites hold at most 192 of the entries and three quarters of the byte limit, filter layer
results at most 64 entries. When a share is full, a new result replaces the least recently used
results of its own kind that neither of the last two frames drew, or is not recorded, so neither
kind crowds out the other. Nothing is evicted for a result the index or the frame's surface budget
would then refuse anyway, and a sprite is not even captured when its share could not hold the
result, so a full share never costs a capture whose result would be dropped. (An earlier cap of
four buckets per sprite content kept a fractionally moving sprite from crowding out filter
results in a single shared quota; with separate shares it only cost hits: on a game scene of 97
background pieces at varied fractional positions it held a still frame to 22 hits.)
`trimTexturePool` with
`ToFloor` or `All` and the two-second idle decay drop sprites with the filter results.
`setSpriteCacheEnabled(false)` stops caching sprites while filter results stay cached; disabling
the filter result cache disables both.

### What a hit skips

A hit returns before the sheet is touched, so the work an uncached draw does for the sheet on the
way does not happen:

- the occurrence's style retention is not consulted, so the retained subtree's last-drawn order is
  not refreshed and it may be evicted from the retention while its sprite stays cached; the next
  uncached draw restyles it;
- the host's font dependencies for the occurrence are marked prepared without a traversal; cached
  content holds no text, so it has none;
- nothing in the sheet reports a pending invalidation. Content whose pixels can change without a
  sheet revision (images, `feImage`, nested `<use>`, text with loadable fonts) is never cached;
  font loads change the font revision in the key.

## Testing and Validation

`//donner/svg/renderer/tests:external_use_sprite_cache_tests` (Geode cases skip on other
backends):

- counters: one rasterization across six frames of six moving occurrences, and every occurrence
  of a warm frame drawn from the cache with no path draw and no filter execution;
- translation, by `transform` or by the `x` and `y` attributes, and the `<use>` element's
  opacity keep every occurrence cached, and match an uncached render exactly;
- invalidation: a sheet geometry change, a filter primitive change, a gradient stop change, an
  inherited host style change, a `setHref` retarget, a scale change and a device scale change
  each rasterize again and match an uncached render exactly; `<symbol>` uses of two sizes keep
  one rasterization each;
- three static scales record once each while an animated scale records nothing;
- sub-pixel motion: a hit in the recorded bucket is exactly the uncached render at the recorded
  translation moved by whole pixels, and within 0.1 over at most 160 edge pixels of the render at
  the actual translation; a single blurred element is resampled at any offset with one
  rasterization, within the bilinear bound;
- large sprites partly past the canvas, drawn before the rest, record whole and every occurrence
  hits from the fourth frame, exactly; over an opaque backdrop cached sprites stay within one
  premultiplied level of an uncached render; a sprite lit by a distant light is cached and exact, and one lit by
  a point light is not cached;
- under `Resample` placement, sprites moving by a fraction of a pixel every frame record once and
  hit every frame, differing from an exact render only at edges, within half a pixel's coverage;
- fallbacks: a `mix-blend-mode`, text, an image, `opacity: inherit` on the root, or a host
  gradient paint is never cached and matches an uncached render exactly;
- host-dependent sizing: a size from a stylesheet on the `<use>`, and a viewport-unit size across
  a host resize, match an uncached render exactly;
- host effects: a clip around the occurrences draws them cached and exact; a filter around them
  draws them uncached and exact;
- a `<symbol>` that scales a blurred child, at fractional offsets, within the bucket bound;
- on Geode: a capture the surface budget refuses still draws every occurrence; a capture that
  can never begin is not admitted again; an abandoned
  recording frame is never reused; captures run no filter and never reject the filter budget;
- disabling the filter result cache drops held sprites.

The existing cached filter result tests run with sprites disabled so they keep measuring filter
layers. The manual wall-clock test `external_use_sprite_tests_wallclock` measures each scene
uncached, with filter results cached, and with sprites cached, including a 150-sprite scene.

## Security and Privacy

The cache holds rasterizations of documents the renderer already draws, keyed by a weak document
identity, and is bounded in entries and bytes. Keys hold no host document references: an
occurrence painted with a host paint server is not cached.

## Alternatives Considered

- **Key on the sheet's retained style epoch**, as filter results do. The epoch is known only after
  the occurrence's styles are installed, which is the work a hit must skip. Comparing the
  occurrence inputs directly is cheap and is what the retention compares anyway.
- **A pass-through filter layer** (a zero offset) for the capture. It would be admitted through
  the frame's filter budget, so a refused admission would drop the occurrence's content and latch
  the frame's filter rejection, and every recording would spend a filter execution. The capture
  reuses the filter layer's surface sizing, recording and compositing without its admission.
- **Rasterize at a fixed scale and transform the bitmap** for every scale. Scaling a raster
  softens edges and loses the vector fidelity of uncached draws; recording per linear transform
  keeps output identical.

## Open Questions

- Whether a fully cached sheet should also skip its per-frame font bookkeeping on the host side
  beyond marking the host's font dependencies prepared.
- Atlas packing and instancing of cached sprites (the next milestone).
