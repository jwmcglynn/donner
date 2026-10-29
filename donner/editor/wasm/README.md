# Wasm editor package

The editor uses full text support on native and Wasm. The complete Wasm package includes
`editor.wasm`, JavaScript/bootstrap files, `assets/` with content-addressed UI fonts,
fallback fonts, chrome SVGs and samples, and `fonts/` with the twelve deferred catalog faces.
Application fonts and static SVG payloads are external to the shipping module. ImGui retains
its small built-in default font. Native packages carry their own verified offline resources.

File > Open uses the browser's native file picker. Save and Save As use a native
save picker when the browser supports writable file handles; other browsers download
an SVG copy without marking pending edits saved. A cancelled picker leaves the current
document unchanged, and failed writes
leave it unsaved. Opening and saving accept SVG text up to 8 MiB. Exporting a viewport
keeps the document's original save destination.

Build the complete bundle with:

```sh
bazel build --config=editor-wasm //donner/editor/wasm:wasm_web_package
```

Serve the complete output directory, preserving its layout. The bootstrap captures
its own package URL, so a versioned subdirectory also works. Font requests never
use an SVG document URL or an external font service. Serve TTF as `font/ttf`, OTF as
`font/otf`, SVG as `image/svg+xml`, and WOFF2 as `font/woff2`
(`application/octet-stream` is accepted), retain the editor's existing cross-origin
isolation headers, and keep fonts on the application's own origin without redirects.

Publish each bundle atomically at an immutable versioned path. Retain old bundles
while their clients remain supported: an already-open client can request its first
catalog font later and still needs the asset pinned by that client's compiled
manifest. The browser cache is optional and cannot replace this retention rule.
`catalog-fonts.json` and `packaged-resources.json` are audit artifacts. The generated
`packaged-resources.js` contains metadata only and starts essential downloads alongside
module compilation. Once initialized, the module's compiled manifest must match before
verified bytes can be installed. Application startup waits for all required UI, fallback
and chrome resources; a missing or invalid resource produces a readable page error. Samples
load only when selected or visible in the sample picker. Preserve every declared file.
Serve content-addressed assets with immutable cache headers where supported.

Catalog metadata and startup do not fetch catalog fonts. Visible document, font-picker,
and sample-preview demand can start requests after the first presented frame.
At most two transfers run together; document/output demand takes precedence over
previews. A pending known font renders with fallback until a safe frame boundary
adopts its verified bytes. This changes rendering without changing SVG source or
adding an undo entry. Geometry and pixel output require complete font readiness.

The browser verifies the compiled length, WOFF2 header, and SHA-256 before making
bytes available to the decoder. The application resource store and persistent encoded cache each have a 16 MiB budget.
The broker admits at most two transfers of up to 8 MiB each, plus one persistence write and
a bounded 16 MiB persistence queue. Staging allocations are released after installation. Persistent entries are reverified and shared across versions
under one cross-tab lock. Storage denial leaves online loading available; warm
cached assets can work offline, but cold offline access to every Wasm catalog
family is not promised. Network failures have one bounded automatic retry;
subsequent retries are explicit and rate limited.

The package includes `CatalogFontNotices.txt`; preserve it with the assets.
The [catalog documentation](https://github.com/jwmcglynn/donner/blob/main/third_party/google_fonts/README.md) describes
generation, pins, codec limits, and semantic equivalence checks.

Module bytes, JavaScript, cold essential bytes, deferred bodies and total package bytes are
measured separately. An unchanged asset uses the same digest cache key across release paths;
a warm code-only update does not fetch its body again. Cache hits are reverified. Cache eviction
falls back to verified fetch, storage denial falls back to the current session, and a cold offline
miss reports unavailability. The package integrity gate checks every declared digest and rejects
font or SVG payloads linked into the actual shipping module. The dependency audit also rejects
native catalog payloads, TinySkia and ReproFile infrastructure in the Wasm runtime graph.
