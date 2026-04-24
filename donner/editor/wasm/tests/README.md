# WASM editor end-to-end tests (Playwright)

This directory holds a headless-Chromium harness that drives the served
Donner WASM editor through the same pointer-event / CSS / emscripten-glfw
path a real browser user hits. It exists because the native desktop tests
(GLFW/Cocoa) don't exercise any of the regressions we keep seeing on the
WASM build:

- drag lag caused by emscripten-glfw pointer-handler wiring
- slow-load stalls from COOP/COEP misconfig or pthread-pool issues
- clipboard routing differences (focus, DOM event order)
- CSS `touch-action` / pointer-events interactions

## What's covered today

**Deliberately small — this is a foothold, not a suite.**

- `smoke.spec.ts` — loads the page, waits on `window.__donner_ready`,
  asserts the canvas is non-zero-sized and renders something, checks there
  are no console errors on startup.
- `drag_perf.spec.ts` — synthesizes a 60-step pointer drag and asserts
  median frame time &le; 20 ms and worst frame &le; 50 ms (budgets
  deliberately generous for a CI Chromium runner — we're gating "is drag
  catastrophic", not "is it 120 FPS").

## What's NOT covered (yet)

- File open / drag-and-drop flows
- Address bar / URL fetcher
- Clipboard (paste, copy)
- Keyboard shortcuts & ImGui text input
- Multi-backend visual parity

Don't mistake two tests for full coverage. Add the next spec before
claiming a bug class is regression-proof.

## Test hooks exposed by the editor

`main.cc` installs these on `window` only when the page URL contains
`?test=1`:

| hook | description |
| ---- | ----------- |
| `window.__donner_ready` | `true` once the first rendered frame has swapped after the initial `loadBytes`. |
| `window.__donner_metrics` | `{ frameTimesMs: number[], pollEventsCount, renderCount, skippedCount, lastFrameTimestampMs, readyTimestampMs }`. Rolling buffer (capacity 240). |
| `window.__donner_simulate([{type,x,y,buttons}, ...])` | Dispatches synthesized `PointerEvent`s to the canvas (with canvas-local coords converted to client coords). Types: `pointerdown`, `pointermove`, `pointerup`. |

The hooks are strictly additive and gated on the URL flag, so a normal
page launch is byte-for-byte unchanged.

## Running the tests

### Via Bazel (recommended)

```sh
bazel run //donner/editor/wasm/tests:playwright_tests
```

The wrapper (`run_tests.sh`):

1. Builds `//donner/editor/wasm:wasm_web_package` (skip with
   `DONNER_WASM_SKIP_BUILD=1`).
2. Runs `npm install` in this directory if `node_modules/@playwright/test`
   is missing.
3. Runs `npx playwright install chromium` (idempotent; fast when cached).
4. Starts `//donner/editor/wasm:serve_http --no-https` in the background
   and parses its "Serving at …" line for the URL.
5. Runs `npx playwright test`, then tears down the server.

### Standalone

```sh
# One-time setup.
(cd donner/editor/wasm/tests && npm install && npx playwright install chromium)

# Terminal 1 — serve the editor.
bazel run --config=editor-wasm //donner/editor/wasm:serve_http -- --no-https

# Terminal 2 — run the tests against it.
cd donner/editor/wasm/tests
DONNER_WASM_BASE_URL=http://127.0.0.1:8000 \
  DONNER_WASM_SKIP_WEBSERVER=1 \
  npx playwright test
```

### Running a single spec / updating traces

```sh
cd donner/editor/wasm/tests
DONNER_WASM_BASE_URL=http://127.0.0.1:8000 \
  DONNER_WASM_SKIP_WEBSERVER=1 \
  npx playwright test drag_perf.spec.ts --reporter=list
```

## Why not `bazel test //...`?

The Bazel target is tagged `manual` — it's not in the default test set
yet, and it intentionally does network + filesystem things outside the
sandbox (`npm install`, downloading Chromium, binding a localhost port).
A follow-up PR can wire it into CI with a `playwright install chromium`
step and drop the `manual` tag once we're confident it's stable on both
macOS and Linux runners. Until then, run it locally before touching any
WASM-specific code path.

## Debugging a failure

- `trace: "retain-on-failure"` is on in `playwright.config.ts`.
  Playwright writes traces under `test-results/`; open with
  `npx playwright show-trace test-results/<path>/trace.zip`.
- Check the serve log — `run_tests.sh` prints its path on startup
  (`[wasm-e2e] Starting serve_http (log: /tmp/donner-wasm-serve.XXXXXX)`).
- The `?test=1` flag is the only difference between harness-driven and
  human-driven pages. Open the URL manually (`http://127.0.0.1:8000/?test=1`)
  and poke at `window.__donner_metrics` in DevTools to confirm the hook
  is wiring up correctly.
