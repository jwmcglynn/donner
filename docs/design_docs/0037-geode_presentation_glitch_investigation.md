# Investigation: Geode Presentation Drag/Zoom Glitches

**Status:** Root cause identified
**Type:** Retrospective / Handoff
**Author:** Codex GPT-5
**Created:** 2026-05-21

## Summary

The Geode editor direct-texture presentation path showed intermittent drag/zoom
glitches:

- The active path overlay is sometimes offset from the moving object during a
  drag. Earlier it lagged; after immediate overlay uploads it updates promptly
  but can still pop out of position.
- During or shortly after zoom plus drag, stale or mis-sized textures could flash
  across the document. The most visible symptom was a small layer tile being
  sampled through a full-canvas presentation quad, so it appeared to fill the
  document for one frame.

The root cause was not just canvas resize. The ImGui WGPU backend caches
`WGPUBindGroup`s by hashing the raw `ImTextureID`; in Geode mode `ImTextureID`
is a raw `WGPUTextureView`. `GlTextureCache` retired old snapshots by upload
call, not by presented UI frame, and it never evicted the corresponding ImGui
bind-group cache entry. When wgpu-native later reused a texture-view handle, the
backend could reuse a bind group that still sampled the old texture while the
editor supplied the current tile's geometry. That exactly produces a
drastically wrong scale: old small texture, current large quad.

## Goals

- Preserve the direct Geode/WGPU presentation model: no production bitmap
  fallback and no normal-frame CPU readback.
- Make drag chrome and dragged content use one coherent UI-frame presentation
  state.
- Prevent stale or incompatible composited tile textures from being drawn after
  zoom/canvas-size changes.
- Keep repros inspectable: `.rnr` replay should produce either screenshots or
  machine-readable per-frame diagnostics that identify the bad frame.

## Non-Goals

- Do not reintroduce the old flat bitmap presentation path.
- Do not make Geode mode fall back to tiny-skia or CPU bitmap uploads.
- Do not remove canvas-size debounce as a workaround unless the owner accepts
  the latency tradeoff. Full-canvas stretching during zoom is intentional.
- Do not treat layer-inspector UI text as the only proof. Visible-frame or
  replay-frame diagnostics are required.

## Current Repros

All replay inputs below are tracked repository files. `/tmp` is used only as a
disposable `--out-dir` for screenshots and replay JSON captures.

### Overlay Misalignment Repro

The user identified the drag frame involving the background radial gradient as
reproducing the overlay misalignment. This is currently associated with:

```sh
bazel run --config=geode //donner/editor/tests:editor_rnr_gl_replay -- \
  --rnr zoom-out-drag-jump.rnr \
  --out-dir /tmp/donner-geode-overlay-repro \
  --capture-frame 142 \
  --max-frame 190 \
  --crop document-canvas \
  --print-diagnostics
```

Frame `142` is in the second drag window after zooming out. Before the latest
gating attempt, this frame also exposed stale tile canvas state.

### Texture Splat Repro

The same tracked fixture was the initial texture-splat repro:

```sh
bazel run --config=geode //donner/editor/tests:editor_rnr_gl_replay -- \
  --rnr zoom-out-drag-jump.rnr \
  --out-dir /tmp/donner-geode-texture-splat-repro \
  --capture-frame 142 \
  --capture-frame 150 \
  --max-frame 190 \
  --crop document-canvas \
  --print-diagnostics
```

The important frame range is roughly `124-150`.

Observed before the stale-split-preview gate:

- Frames `137-149`: viewport desired canvas is `1896x1088`.
- Presented split tiles can still report raster canvas `3203x1838`.
- By frame `160`, the presented split tiles have caught up to `1896x1088`.

After the latest gate, the diagnostic run no longer accepts the late `3203x1838`
split result. However, the user still reports visible glitches, so this was at
best one contributing cause.

The later root-cause repro was clearer because it showed a small tile sampled
through a large quad:

```sh
bazel run --config=geode //donner/editor/tests:editor_rnr_gl_replay -- \
  --rnr donner/editor/tests/geode_drag_zoom_o_pop.rnr \
  --svg donner_splash.svg \
  --out-dir /tmp/donner-geode-drag-zoom-o-pop-current \
  --capture-frame 78 \
  --capture-frame 79 \
  --capture-frame 80 \
  --capture-frame 81 \
  --max-frame 81 \
  --crop document-canvas
```

Before the cache fix, frames `79` and `80` could show a huge wrong texture.
After the cache fix, the same frames render as normal drag/zoom frames.

## Readback Status

Another agent added explicit WGPU framebuffer readback support for replay tests.
The option is test-only:

- `EditorWindowOptions::enableFramebufferReadback` defaults to `false`.
- `GlRnrReplay.cc` sets `.enableFramebufferReadback = true`.
- Production `main.cc` uses default `EditorWindowOptions` and calls
  `window.endFrame()`.
- Geode overlay presentation uses `takeTextureSnapshot()`, not `takeSnapshot()`.
- Geode composited presentation uses texture snapshots when
  `requiresTextureSnapshotPresentation()` is true.
- The WGPU surface is configured with `CopySrc` only when explicit readback is
  enabled.

Relevant validation already run:

```sh
bazel test --config=geode --nocache_test_results \
  //donner/editor/tests:gl_rnr_replay_tests
```

## Investigation Timeline

### Direct Texture Presentation

Geode editor presentation was changed from CPU readback plus OpenGL upload to
direct WGPU texture presentation:

- `RendererGeode` exports `RendererTextureSnapshot` objects.
- `AsyncRenderer` carries texture snapshots through `RenderResult`.
- `GlTextureCache` registers texture views for ImGui/WGPU instead of uploading
  pixels in Geode mode.
- Tiny-skia/default editor builds still use CPU bitmap payloads and GL uploads.

The direct path is required. A missing Geode texture payload should be a
diagnostic skip/assert, not a fallback to CPU pixels.

### Earlier Crash

The Geode editor previously hit:

```text
invalid bind group entry for bind group descriptor
```

That was fixed separately around Geode bind group / WGPU utility handling. A
test exists under the Geode renderer utility tests in the current worktree.

### Drag Fast Path and RAII Work

Several Geode presentation bugs were reduced by:

- Preserving fast-path drag behavior so active drags do not rerender unrelated
  textures.
- Keeping texture snapshots alive through RAII instead of relying on loose
  handles.
- Adding a WGPU-only retired snapshot queue in `GlTextureCache` so a texture
  view is not freed while an ImGui draw list may still reference it.

This reduced crashes and obvious stale-view hazards but did not eliminate all
visual glitches.

### Overlay Lag Fix

The active overlay used to wait behind `displayedDocVersion`. That made the
path overlay visibly lag dragged content. The current worktree adds:

- `RenderCoordinator::OverlayUploadMode`.
- `OverlayUploadMode::Immediate` for active drag overlay rasterization.
- An active-drag path in `EditorShell` that flushes the drag DOM mutation and
  uploads overlay chrome immediately.

This fixed the low-frequency overlay lag, but the user now sees intermittent
overlay pops. That means the remaining issue is not simply "overlay upload is
late"; it is likely a mismatch among:

- live UI-frame viewport,
- live DOM drag transform,
- displayed composited tile baseline,
- overlay raster canvas/transform,
- or a stale async result replacing one of those pieces.

### Metadata-Only Texture Identity

Texture splats initially looked like cache-key aliasing. The current worktree
therefore adds stronger identity for metadata-only composited tiles:

- tile id,
- tile kind,
- payload generation,
- texture dimensions,
- raster canvas size.

`GlTextureCache` now skips metadata-only tiles whose cached payload identity is
absent or incompatible. It also exposes:

- `metadataOnlyMissCount()`,
- `duplicateLiveTextureCount()`,
- per-tile diagnostic fields.

The `zoom-out-drag-jump.rnr` diagnostics showed no metadata-only misses and no
duplicate live texture handles during the captured bad range. This points away
from simple tile-id aliasing and toward stale async result acceptance or
presentation-state mismatch.

### Stale Split Preview Gate

The strongest machine-readable texture-splat signal was a stale split preview:

- Current viewport desired canvas: `1896x1088`.
- Late split preview tile raster canvas: `3203x1838`.

The current worktree adds:

```cpp
ShouldPresentCompositedPreviewForViewport(...)
```

Policy:

- Full-canvas previews may stretch across canvas-size changes.
- Split composited previews are rejected when any tile's `rasterCanvasSize`
  differs from the current viewport desired canvas by more than one pixel.

Focused tests added to `//donner/editor/tests:async_renderer_tests`:

- stale split preview is rejected,
- current split preview is accepted,
- full-canvas preview may stretch.

This removed one reproducible stale split swap in the replay diagnostics. It was
only a partial fix; the later wrong-scale texture repro below is the root-cause
closure for the drastic texture splat.

### ImGui WGPU Bind Group Cache

The final root cause is in the presentation texture binding layer:

- `GlTextureCache::ToImTextureId` passes a raw `WGPUTextureView` as
  `ImTextureID`.
- `RenderPanePresenter` supplies current geometry to `ImDrawList::AddImage`.
- `imgui_impl_wgpu.cpp` hashes `ImTextureID` and caches a `WGPUBindGroup` for
  that hash.
- The backend had no remove/evict API for transient texture IDs.
- `GlTextureCache` released old texture snapshots after only three retirement
  calls, which can be fewer than three presented UI frames during overlay plus
  composited uploads.

Once the raw view value was reused, ImGui could bind the old texture through the
new draw command. This is why the failure looks like "the wrong texture rendered
at the wrong scale" rather than a coordinate-only bug.

The fix in the current worktree:

- Adds `ImGui_ImplWGPU_RemoveTexture(ImTextureID)` via the local ImGui patch and
  clears cached bind groups when device objects are invalidated.
- Stores retired WGPU snapshots in `GlTextureCache` by texture ID plus snapshot,
  advances retirement once per UI frame, and evicts the ImGui texture binding
  before releasing the old snapshot.
- Calls `GlTextureCache::advancePresentationFrame()` once at the start of
  `EditorShell::runFrame()`, so upload churn inside a frame no longer ages out
  textures prematurely.

## Current Diagnostics

`GlRnrReplay` now exposes per-frame diagnostics when the CLI is invoked with
`--print-diagnostics`:

- frame index,
- canvas freshness enum/status suffix,
- viewport desired canvas,
- document canvas,
- compositor canvas,
- metadata-only miss count,
- duplicate live texture handle count,
- paint-order tile list:
  - tile id,
  - kind,
  - generation,
  - texture dimensions,
  - raster canvas size,
  - canvas offset in document coordinates,
  - bitmap dimensions in document units,
  - drag translation in document coordinates,
  - texture/view handle,
  - metadata-only flag,
  - drag-target flag.

This is intended as a handoff tool. The current test coverage validates the
diagnostic path through `gl_rnr_replay_tests`. The checked-in
`geode_drag_zoom_o_pop.rnr` regression captures the bad-frame window, while the
deterministic root-cause guard lives in `gl_texture_cache_tests` because the raw
visible flicker depends on texture-view handle reuse timing.

## Important Files

- `donner/editor/AsyncRenderer.{h,cc}`
  - Converts compositor tile snapshots into `RenderResult::CompositedPreview`.
  - Publishes bitmap or texture payloads depending on backend.
  - Carries `bitmapDimsPx` and `rasterCanvasSize`.
- `donner/editor/GlTextureCache.{h,cc}`
  - Owns presentation texture cache and metadata-only reuse policy.
  - Contains WGPU retired snapshot queue.
  - Exposes tile diagnostics.
- `donner/editor/RenderCoordinator.{h,cc}`
  - Polls async render results.
  - Uploads overlays and composited previews.
  - Contains immediate overlay mode and stale split-preview gate.
- `donner/editor/RenderPanePresenter.cc`
  - Draws composited tiles and overlay texture in the current viewport.
  - Uses `PresentedFrameComposer`.
- `donner/editor/PresentedFrameComposer.{h,cc}`
  - Resolves drag baseline and output tile rects.
- `donner/editor/repro/GlRnrReplay.{h,cc}`
  - Shared `.rnr` GL/WGPU replay harness.
  - Sets explicit framebuffer readback option.
- `donner/editor/tests/EditorRnrGlReplay.cc`
  - CLI wrapper. `--print-diagnostics` prints replay JSON.
- `zoom-out-drag-jump.rnr`
  - Current tracked repro for zoom-out plus drag texture/overlay issues.
- `donner/editor/tests/geode_drag_zoom_o_pop.rnr`
  - Checked-in root-cause replay for the small O tile being sampled through a
    large presentation quad.

## Superseded Hypotheses

### Hypothesis A: Overlay Raster Uses Current DOM But Stale Presented Baseline

The overlay now uploads immediately from live UI-frame DOM state. The dragged
tile may still be displayed using a cached composited baseline plus residual
drag delta. If the displayed baseline is stale or missing for one UI frame, the
overlay can be correct for the DOM while the content tile is correct for a
different presented baseline.

Next checks:

- Add diagnostics that record the active drag translation and displayed
  presentation baseline in the same frame.
- Assert that overlay rasterization and `ComputePresentedTileRect` consume the
  same active translation/baseline for drag-target frames.
- Compare active frame and same-cursor mouseup frame using identity pixelmatch
  for `zoom-out-drag-jump.rnr`, not only `filter_post_drag_jump.rnr`.

### Hypothesis B: Polling Drops Stale Result Too Late But Leaves Side Effects

The current stale split-preview gate returns early from `pollRenderResult` before
uploading textures or updating `displayedDocVersion_`. This avoids one bad
presentation swap. Confirm that no other side effects from the stale result
remain:

- frame-history backend time update,
- async renderer's published tile bookkeeping,
- compositor diagnostics rows,
- layer inspector status,
- selection-bounds cache refresh.

The helper gate only protects UI upload. It does not prevent `AsyncRenderer`
from marking its published tile state internally when it staged the result.
If the worker's published metadata advances for a result the UI rejects, a later
metadata-only result may assume the UI has payloads it never accepted.

This is a strong next investigation target.

### Hypothesis C: Published Tile State Must Be Acknowledged By The UI

`AsyncRenderer::notePublishedCompositedPreview` runs before the UI accepts or
rejects a result. The name "published" currently means "staged by the worker",
not "accepted by `GlTextureCache`". If the UI rejects a stale split preview, the
worker can still believe those texture payloads are live and send metadata-only
tiles later.

Potential fix direction:

- Move published-tile acknowledgment out of the worker and into the UI after
  `GlTextureCache::uploadComposited` accepts payloads.
- Or add an explicit "accepted presentation epoch" feedback path.
- Until then, treat metadata-only reuse after a rejected result as suspect even
  when `GlTextureCache` has strong identity checks.

### Hypothesis D: Full-Canvas Stretch Is Safe Only Before Split Promotion

The design intent is that a full-canvas preview can stretch during zoom to
avoid lag. Split tile presentation may need stricter phase boundaries:

- full-canvas stale image while zoom is unsettled,
- split tiles only after document canvas and viewport desired canvas match,
- no split-to-split replacement across stale canvas epochs.

The current gate implements one part of this but may still allow an old split
set to remain visible while the viewport changes. That avoids a new stale split
swap, but it can still show stale split geometry stretched. If that is visible
as a glitch, the presenter may need to fall back to the last full-canvas
presentation during canvas freshness mismatch instead of holding split tiles.

## Verification Status

Commands run during this investigation:

```sh
bazel test //donner/editor/tests:async_renderer_tests \
  --test_filter='RenderCoordinatorTest.*CanvasEpoch*|RenderCoordinatorTest.FullCanvasPreviewCanStretchAcrossCanvasEpochs'

bazel test --config=geode //donner/editor/tests:async_renderer_tests \
  --test_filter='RenderCoordinatorTest.*CanvasEpoch*|RenderCoordinatorTest.FullCanvasPreviewCanStretchAcrossCanvasEpochs'

bazel test --config=geode --nocache_test_results \
  //donner/editor/tests:gl_rnr_replay_tests

bazel build --config=geode //donner/editor -c opt

git diff --check
```

The tests above do not prove the user-reported glitches are fixed. They prove
only the helper policy, replay readback/diagnostic path, and Geode editor build
are healthy.

Additional validation after the bind-group cache fix:

```sh
bazel build --config=geode //donner/editor/tests:editor_rnr_gl_replay

bazel test --config=geode //donner/editor/tests:gl_texture_cache_tests

bazel test --config=geode //donner/editor/tests:async_renderer_tests \
  --test_filter='RenderCoordinatorTest.*CanvasEpoch*|RenderCoordinatorTest.FullCanvasPreviewCanStretchAcrossCanvasEpochs'

bazel test --config=geode --nocache_test_results \
  //donner/editor/tests:gl_rnr_replay_tests

bazel run --config=geode //donner/editor/tests:editor_rnr_gl_replay -- \
  --rnr donner/editor/tests/geode_drag_zoom_o_pop.rnr \
  --svg donner_splash.svg \
  --out-dir /tmp/donner-geode-drag-zoom-o-pop-cache-fix \
  --capture-frame 78 \
  --capture-frame 79 \
  --capture-frame 80 \
  --capture-frame 81 \
  --max-frame 81 \
  --crop document-canvas
```

The captured frames in the checked-in root-cause replay are visually clean after
the fix.

## Suggested Next Steps

- [x] Check in the root-cause `.rnr` fixture and cover its bad-frame window from
      `gl_rnr_replay_tests`. The deterministic texture-view reuse guard is the
      WGPU `GlTextureCacheTest.RetiredSnapshotsAgeByPresentationFrame` test; the
      raw visible flicker is timing-sensitive and `--print-diagnostics` can mask
      it.
- [ ] Add active-drag baseline diagnostics to replay JSON:
      active drag entity, active translation, displayed preview entity, and
      displayed represented translation.
- [ ] Audit whether `AsyncRenderer::notePublishedCompositedPreview` can advance
      worker-side published metadata for a UI-rejected result.
- [ ] If so, move "published" tile acknowledgment to the UI after
      `GlTextureCache::uploadComposited` accepts a result.
- [ ] Decide whether split tiles should be hidden or replaced by last
      full-canvas presentation whenever `CanvasFreshness != Current`.
- [ ] Add a replay assertion that no live frame displays split tiles whose
      `rasterCanvasSize` mismatches `viewportDesiredCanvas`, unless that frame
      is explicitly marked as full-canvas stretch.
- [ ] Re-run manual acceptance:

  ```sh
  bazel run --config=geode //donner/editor -c opt -- donner_splash.svg
  ```

  Verify zooming while dragging several `DONNER` letter tiles does not show
  overlay pops or texture splats.

## Handoff Notes

- The worktree is dirty and contains unrelated earlier Geode direct-texture
  changes. Do not revert files wholesale.
- The user's latest report says glitches still occur after the stale
  split-preview gate. Start from repro and diagnostics, not from assuming the
  latest gate is sufficient.
- The stale split-preview and worker-side "published" metadata issues remain
  worth cleaning up, but they were not the final root cause for the drastic
  wrong-scale texture splat.
- Keep Geode production presentation direct-texture only. The replay readback
  flag exists for tests and proof artifacts, not as a presentation fallback.
