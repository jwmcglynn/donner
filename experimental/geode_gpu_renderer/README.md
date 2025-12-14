# WebGPU Geode Prototype

This prototype exercises the first milestone of the geode renderer design by rendering a single
geode-encoded glyph with WebGPU and WGSL analytic coverage. It now routes through a small
Canvas-like API so later milestones can extend the surface without rewriting the demo glue.

## Running
1. Start a local static server in this directory (WebGPU requires HTTPS or localhost):
   ```bash
   cd experimental/geode_gpu_renderer
   python3 -m http.server 8000
   ```
2. Open http://localhost:8000 in a WebGPU-enabled browser.
3. You should see a green glyph drawn via the geode pipeline. If WebGPU is unavailable, the page
   reports an error because this prototype requires GPU execution.

## Notes
- `shaders/geode_eval.wgsl` contains the draft WGSL modules for geode evaluation (line and quadratic
  segments with analytic AA). The fragment shader computes signed distances per segment and
  converts them to coverage with a simple smoothstep; future steps will tighten AA and extend to
  full glyph data.
- `geode_canvas.js` defines the Canvas-like API that accumulates path commands and emits geode storage
  buffers. It requires WebGPU and does not include a CPU fallback path.
- `main.js` now imports the Canvas-like API, builds a simple path with line and quadratic segments,
  and triggers a fill to drive the WGSL pipeline.
- The demo uses hardcoded geometry; later milestones will add path parsing, text layout, and golden
  image capture for regression testing.
