# Standalone renderer WebAssembly

`donner_wasm` exposes the software renderer C API and `donner_wasm_geode` exposes its Geode
counterpart. The named `wasm-size` and `wasm-geode` configurations disable text by default and do
not need font assets. Text is an explicit build choice:

```sh
bazel build --config=wasm-size --//donner/svg/renderer:text=true \
  //donner/svg/renderer/wasm:test_page
bazel build --config=wasm-geode --//donner/svg/renderer:text=true \
  //donner/svg/renderer/wasm:geode_browser_test_package
```

Keep each complete package together. A text-enabled package includes the pinned Public Sans
fallback under `assets/`, its bundled license notice, and the static resource transport. The browser pages await authenticated
resource installation before calling the original initialization/render APIs. The runtime reads
identities from the compiled module, not fetched audit JSON; it permits only same-origin,
content-addressed bodies, rejects redirects and oversized/corrupt data, and asks the core to
independently authenticate every installation. Page teardown closes pending resource work.

An embedding host can supply its own bytes without browser transport: obtain the bounded compiled
manifest with `DonnerPackagedResourceLoader.compiledManifest(module)`, copy a known resource into
module memory, and call `DonnerInstallPackagedResource`. A successful installation returns `1`;
unknown identities, wrong sizes/digests or budget failures return `0`. A text-enabled module also
exports `donner_prepare_resources`, which returns `1` when required resources are ready. Text
render calls enforce that check themselves and return null with `donner_get_last_error` on failure.
The existing init/render/free/error signatures remain unchanged. Never fake editor `callMain` or
use an editor worker to initialize this renderer.

`render_text_test` runs real software-Wasm glyph pixels with host-owned font bytes and rejects
missing, corrupt and mismatched packages. `renderer_resource_bootstrap_test` covers the browser
wrapper's transport and cancellation seams. The private Geode Playwright fixture explicitly selects
basic text (`text=true`, `text_full=false`); its existing shape golden and a glyph-versus-fontless
pixel case qualify actual browser rendering. The private fixture does not change the public
module/page configuration or the named no-text size profiles.
