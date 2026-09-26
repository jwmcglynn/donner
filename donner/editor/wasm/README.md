# Wasm editor package

The editor uses full text support on native and Wasm. The Wasm package contains
`editor.wasm`, its JavaScript/bootstrap files, and a `fonts/` directory of twelve
content-addressed WOFF2 catalog assets. UI fonts and the Public Sans fallback stay
inside the module. Native editors embed the same catalog WOFF2 bytes and decode
in memory; they need no companion font files, network connection, or writable
font cache.

Build the complete bundle with:

```sh
bazel build --config=editor-wasm //donner/editor/wasm:wasm_web_package
```

Serve the complete output directory, preserving its layout. The bootstrap captures
its own package URL, so a versioned subdirectory also works. Font requests never
use an SVG document URL or an external font service. Serve WOFF2 as `font/woff2`
(`application/octet-stream` is accepted), retain the editor's existing cross-origin
isolation headers, and keep fonts on the application's own origin without redirects.

Publish each bundle atomically at an immutable versioned path. Retain old bundles
while their clients remain supported: an already-open client can request its first
catalog font later and still needs the asset pinned by that client's compiled
manifest. The browser cache is optional and cannot replace this retention rule.
`catalog-fonts.json` is an audit artifact; runtime trust comes from metadata
compiled into the application.

Catalog metadata and startup do not fetch fonts. Visible document, font-picker,
and sample-preview demand can start requests after the first presented frame.
At most two transfers run together; document/output demand takes precedence over
previews. A pending known font renders with fallback until a safe frame boundary
adopts its verified bytes. This changes rendering without changing SVG source or
adding an undo entry. Geometry and pixel output require complete font readiness.

The browser verifies the compiled length, WOFF2 header, and SHA-256 before making
bytes available to the decoder. Memory and persistent encoded caches are each
bounded to 4 MiB. Persistent entries are reverified and shared across versions
under one cross-tab lock. Storage denial leaves online loading available; warm
cached assets can work offline, but cold offline access to every Wasm catalog
family is not promised. Network failures have one bounded automatic retry;
subsequent retries are explicit and rate limited.

The package includes `CatalogFontNotices.txt`; preserve it with the assets.
The [catalog documentation](https://github.com/jwmcglynn/donner/blob/main/third_party/google_fonts/README.md) describes
generation, pins, codec limits, and semantic equivalence checks.

The Wasm module goal is approximately 3.2 MB with gzip level 9 and a zero timestamp.
With rich text, visual stroke controls, and background font previews, the measured
module is 3,170,662 bytes; the regression gate allows 3,205,000 bytes. The generic
sans-serif face reuses the renderer's existing Public Sans font on Web, while
desktop-only preview-cache persistence and Noto faces stay out of the module.
Deferred font bytes, JavaScript, and total package size are measured separately.
The dependency audit rejects native catalog payloads, TinySkia, and ReproFile
infrastructure in the Wasm runtime graph.
