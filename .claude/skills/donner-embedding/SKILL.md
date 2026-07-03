---
name: donner-embedding
description: Using Donner as a library from a consumer's point of view — the public SVG DOM API (SVGParser/SVGDocument/SVGElement/Renderer), the runnable examples/ programs, consuming Donner via Bazel module or the generated CMake mirror, Geode embedded (host-owned WebGPU device) mode, and the ECS-is-internal public-API boundary. Use when writing or reviewing code that parses/renders SVGs through the public API, when working in examples/ or examples/cmake_consumer/, when answering "how do I add Donner to my project", when editing docs/getting_started.md or docs/guides/embedding_geode.md, or when a change might leak ECS types (Registry, EntityHandle, EnTT) into a public header.
---

# Embedding Donner: the library-consumer view

"Embedding" means using Donner from the outside: parse an SVG, walk/modify the DOM, render pixels,
inside someone else's application. Everything a consumer touches must work through the public API
described here — never through ECS internals (see the boundary rule at the bottom).

## Public API in 30 seconds

The umbrella header pulls in the three core pieces (parser, document, element):

```cpp
#include "donner/svg/SVG.h"                 // SVGParser, SVGDocument, SVGElement
#include "donner/svg/renderer/Renderer.h"   // Renderer (backend-agnostic facade)
```

Parse → check errors → use the document:

```cpp
donner::ParseWarningSink warnings;  // or donner::ParseWarningSink::Disabled() to suppress
donner::ParseResult<donner::svg::SVGDocument> maybeDoc =
    donner::svg::parser::SVGParser::ParseSVG(svgSource, warnings);
if (maybeDoc.hasError()) { /* maybeDoc.error() has line:column + reason */ }
donner::svg::SVGDocument document = std::move(maybeDoc.result());
```

- `SVGParser::ParseSVG(std::string_view source, ParseWarningSink&, Options = {}, Settings = {})`
  is the only string entry point (`donner/svg/parser/SVGParser.h`). There is no
  warnings-omitted overload — pass `ParseWarningSink::Disabled()` if you don't care.
- The header contract says the source buffer "will not be modified", but the shipped examples
  keep the buffer alive for the document's lifetime (`examples/svg_to_png.cc` says it is
  referenced internally). Do the safe thing: keep the source alive as long as the `SVGDocument`.
- `Options::parseAsInlineSVG = true` accepts fragments without the `xmlns` attribute;
  `Options::disableUserAttributes = false` allows non-SVG attributes (e.g. `data-name`)
  without warnings.

DOM traversal and mutation (`donner/svg/SVGDocument.h`, `donner/svg/SVGElement.h`):

- `document.querySelector("path")` / `element.querySelector(":scope > rect")` — full CSS
  selectors, returns `std::optional<SVGElement>`.
- `SVGElement` is a lightweight value type: `parentElement()`, `firstChild()`, `nextSibling()`,
  `getAttribute()`, `setAttribute()`, `setStyle()`, `updateStyle()`, `getComputedStyle()`,
  `appendChild()`, `insertBefore()`, `removeChild()`, `remove()`, `id()` / `setId()`.
- `setStyle("fill: red")` **replaces** the entire `style=""` contribution (it clears previous
  style-attribute properties first — `donner/svg/components/style/StyleComponent.h`). To merge
  new declarations into the existing style without removing others, use
  `updateStyle("fill: red")`. The comment in `examples/svg_tree_interaction.cc` claiming
  setStyle calls "combine together and do not replace" is wrong (see stale-doc traps below).
- Downcast with `element.isa<SVGPathElement>()` then `element.cast<SVGPathElement>()` (cast
  asserts on mismatch). Typed wrappers live one header per element:
  `donner/svg/SVGCircleElement.h`, `SVGPathElement.h`, `SVGTextElement.h`, … or all at once via
  `donner/svg/AllSVGElements.h`.
- Create new elements with the static factory: `SVGTSpanElement::Create(document)`, then attach
  via `appendChild`/`insertBefore` (see `examples/svg_text_interaction.cc`).
- Canvas sizing: `document.setCanvasSize(800, 600)` (like resizing a browser window) or
  `document.useAutomaticCanvasSize()`.

Rendering (`donner/svg/renderer/Renderer.h` — resolves to the build-selected backend,
tiny-skia by default):

```cpp
donner::svg::Renderer renderer;
renderer.draw(document);
renderer.save("output.png");                                  // bool
donner::svg::RendererBitmap px = renderer.takeSnapshot();     // in-memory pixels
```

Hit-testing: `donner::svg::DonnerController` (`donner/svg/DonnerController.h`) wraps a document;
`controller.findIntersecting(Vector2d(x, y))` returns `std::optional<SVGGeometryElement>`.

Depth docs: `docs/getting_started.md` (walkthrough + license attribution), `docs/api.md`
(library map), `docs/svg_dom_threading_and_lifetime.md` (multithreaded DOM access,
removed-element lifetime).

## The examples/ directory

List current targets with `grep 'name = ' examples/BUILD.bazel` — don't trust a memorized list.
As of writing, all run from the **repo root**:

| Target                              | Run                                                                                               | Shows                                                 |
| ----------------------------------- | ------------------------------------------------------------------------------------------------- | ----------------------------------------------------- |
| `//examples:svg_to_png`             | `bazel run //examples:svg_to_png -- donner_splash.svg`                                            | parse → render → save PNG                             |
| `//examples:svg_tree_interaction`   | `bazel run //examples:svg_tree_interaction`                                                       | querySelector, cast, setStyle, computedSpline         |
| `//examples:svg_text_interaction`   | `bazel run //examples:svg_text_interaction -- output.png`                                         | `<text>`/`<tspan>` DOM creation + restyle             |
| `//examples:svg_filter_interaction` | `bazel run //examples:svg_filter_interaction -- output.png`                                       | edit `<feGaussianBlur>` via the DOM                   |
| `//examples:custom_css_parser`      | `bazel run //examples:custom_css_parser`                                                          | donner/css standalone with a fake DOM (`ElementLike`) |
| `//examples:svg_viewer`             | `bazel run //examples:svg_viewer -- donner_splash.svg`                                            | minimal ImGui viewer (macOS/Linux only)               |
| `//examples:geode_embed`            | `bazel run --config=geode //examples:geode_embed -- file.svg`                                     | host-owned WebGPU device + Geode                      |
| `//examples:render_test`            | `bazel run //examples:render_test` (also `render_test_wasm` via `--config=wasm`, tagged `manual`) | pipeline smoke test, WASM-oriented                    |
| `//examples:wasm_reproducer`        | tagged `manual` (also `wasm_reproducer_wasm`)                                                     | minimal WASM bug-repro harness                        |

Notes:

- Examples use `BUILD_WORKING_DIRECTORY` to chdir back to your shell's cwd, so relative SVG paths
  ("donner_splash.svg" at the repo root) work under `bazel run`.
- `examples/` has its **own MODULE.bazel** (`bazel_dep(name = "donner", version = "0.0.0")` +
  `local_path_override(path = "..")`), but standalone builds (`cd examples && bazel build ...`)
  do **not** work today: `examples/BUILD.bazel:1` loads repo-root-relative
  `//build_defs:rules.bzl`, which resolves to a nonexistent `examples/build_defs` when
  `examples/` is the root module — the whole package fails to load. Build everything from the
  repo root. Several targets also depend on repo-root labels directly: `svg_viewer` and
  `geode_embed` (`//:donner`, `//donner/editor:text_editor`), `render_test` and
  `wasm_reproducer` (`//donner/svg/renderer`, `@tiny-skia-cpp//src:tiny_skia_lib_native`).
- `geode_embed` is gated on `--config=geode`; without it the target is `@platforms//:incompatible`
  (BUILD selects on `//donner/svg/renderer/geode:geode_enabled`).
- Stale-reference trap: `examples/svg_viewer_test.mjs` mentions a `//examples:svg_viewer_web_package`
  target and `.bazelrc` comments mention `//examples:svg_viewer_wasm` — neither target exists in
  `examples/BUILD.bazel` today. Verify targets with grep before citing them.

## Consuming Donner from Bazel

**Known broken as of 2026-07 for non-root consumers.** The intended recipe
(`docs/getting_started.md`) is a consumer `MODULE.bazel` with:

```py
bazel_dep(name = "donner", version = "0.0.0")
git_override(
    module_name = "donner",
    remote = "https://github.com/jwmcglynn/donner",
    commit = "<pin a real commit: git ls-remote https://github.com/jwmcglynn/donner main>",
)
```

then `deps = ["@donner"]` on any `cc_binary`/`cc_library` (`@donner` is the root
`donner_cc_library` in `BUILD.bazel`: SVG DOM + the active renderer). But today the `@donner`
package fails to even _load_ from an external module: root `BUILD.bazel` loads
`//build_defs:rules.bzl`, whose line 7 unconditionally loads `@rules_python//python:defs.bzl`,
and `rules_python` is declared `dev_dependency = True` (`MODULE.bazel:139`) — invisible whenever
donner is not the root module. Error signature:
`No repository visible as '@rules_python' from repository '@@donner+'`. The consumer cannot fix
this by adding rules_python to their own MODULE.bazel (each module resolves its own repo
mapping). Until donner makes that load non-dev or conditional, point consumers at the CMake
path below (verified working end-to-end) — or a source checkout where donner is the root.

If/when the load bug is fixed, two more prerequisites for a from-scratch consumer:

- **Pin Bazel to donner's version** (`.bazelversion`, currently 8.7.0). Newer Bazel (9.x)
  fails inside `build_defs/rules.bzl` with `The CcInfo symbol has been removed ...`.
- Modern bzlmod has no native `cc_binary`: the consumer needs
  `bazel_dep(name = "rules_cc", ...)` + `load("@rules_cc//cc:cc_binary.bzl", "cc_binary")`.

BCR publishing is wired via `.bcr/` + the `publish-to-bcr` job in
`.github/workflows/release.yml` (see **donner-release**) but is not live yet — no version of
donner is on the registry, so `git_override` (or `local_path_override`) is mandatory; a bare
version-pinned `bazel_dep` will not resolve. The module version is in `MODULE.bazel`
(`0.8.0-pre` line).

Consumers tune features with the module extension (`config/extensions.bzl`):

```py
donner = use_extension("@donner//config:extensions.bzl", "donner")
donner.configure(
    renderer = "tiny_skia",   # accepted values: "tiny_skia", "skia"
    filters = True,
    text = True,              # stb_truetype tier: TTF/OTF, kern-table kerning
    text_full = False,        # FreeType + HarfBuzz + WOFF2 tier
)
use_repo(donner, "donner_config")
```

All options default sensibly; omitting `donner.configure()` entirely is fine. Non-BCR deps
(harfbuzz, wgpu-native, tracy, …) live behind a `dev_dependency = True` extension in Donner's
own `MODULE.bazel` — external consumers **never fetch them at all** (dev extensions only run
when donner is the root module, regardless of feature flags); targets needing those repos are
skipped via `target_compatible_with` guards (see `third_party/bazel/non_bcr_deps.bzl`).

License attribution: Donner bundles permissively-licensed third parties whose notices you must
redistribute. Build the aggregate NOTICE target matching your configuration —
`@donner//third_party/licenses:notice_default` (default tiny-skia config), `notice_text_full`
(tiny-skia + `text_full = True`), `notice_skia_text_full` (stale "skia" backend),
`notice_editor` (editor binary; auto-derived) — and embed the produced `notice_<variant>.txt`.
Re-list with `grep 'name = "notice' third_party/licenses/BUILD.bazel`. Full recipe
(filegroup + `//tools:embed_resources`) is in `docs/getting_started.md` § Third-Party License
Attribution.

## Consuming Donner from CMake

CMake support is a **generated mirror** of the Bazel build. The generator writes `CMakeLists.txt`
files that are gitignored (`**/CMakeLists.txt` in `.gitignore`) — the tracked exceptions are
under `examples/`. So the first step in any checkout is always:

```sh
python3 tools/cmake/gen_cmakelists.py    # generates root + per-dir CMakeLists.txt
```

Consumer pattern (verified against `examples/cmake_consumer/`):

```cmake
set(DONNER_SOURCE_DIR "/path/to/donner" CACHE PATH "Path to Donner")
add_subdirectory("${DONNER_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/donner")
add_executable(my_app main.cc)
target_link_libraries(my_app PRIVATE donner)   # the exported umbrella target
```

Runnable end-to-end check (same commands CI's `cmake.yml` runs):

```sh
python3 tools/cmake/gen_cmakelists.py
cmake -S examples/cmake_consumer -B build/cmake-consumer
cmake --build build/cmake-consumer --target donner_cmake_consumer
ctest --test-dir build/cmake-consumer --output-on-failure
```

Key CMake facts (root `CMakeLists.txt` is generated; options validated with `FATAL_ERROR`):

- `DONNER_RENDERER_BACKEND` supports only `"tiny_skia"` in CMake today — anything else fails
  configure. Geode is Bazel-only.
- Options: `DONNER_TEXT` (ON), `DONNER_TEXT_FULL` (OFF), `DONNER_TEXT_WOFF2` (ON, requires
  `DONNER_TEXT`), `DONNER_FILTERS` (ON), `DONNER_BUILD_TESTS` (OFF). Deps come via
  `FetchContent` (abseil, EnTT, googletest, nlohmann_json, …).
- Error `"Run python3 tools/cmake/gen_cmakelists.py in DONNER_SOURCE_DIR first"` means exactly
  that — the consumer project checks for the generated root `CMakeLists.txt`.
- **If you touch the CMake mirror or `gen_cmakelists.py`**, project rule: also run
  `python3 tools/cmake/gen_cmakelists.py --check --build` (`--check` is fast/static; `--build`
  actually compiles the generated tree and catches real drift before CI). See the
  **donner-build-test** skill.

## Geode embedded mode (host-owned WebGPU device)

Full guide: `docs/guides/embedding_geode.md` — read it before writing embedded-mode code. The
short version: normally Geode owns its `wgpu::Device`; in embedded mode the **host** owns the
device/queue/target texture and Geode draws into the host's texture.

```cpp
#include "donner/svg/renderer/geode/GeodeDevice.h"
#include "donner/svg/renderer/RendererGeode.h"

donner::geode::GeodeEmbedConfig config;           // device, queue, textureFormat, adapter(optional)
// CreateFromExternal returns std::unique_ptr<GeodeDevice> (non-owning of the wgpu objects);
// nullptr if config.device or config.queue was null. The RendererGeode ctor takes a shared_ptr,
// which a moved unique_ptr converts to.
auto geodeDevice = donner::geode::GeodeDevice::CreateFromExternal(config);
donner::svg::RendererGeode renderer(std::move(geodeDevice));

// Per frame:
renderer.setTargetTexture(swapChainTex);
renderer.draw(document);
renderer.clearTargetTexture();   // reverts to internal offscreen target
```

- Build with `--config=geode` (in `.bazelrc` this sets
  `--//donner/svg/renderer:renderer_backend=geode` and
  `--//donner/svg/renderer/geode:enable_geode=true`). The guide's mention of an `enable_dawn`
  flag is stale — the live flag is `enable_geode`.
- Lifetime: host objects must outlive every `GeodeDevice`/`RendererGeode`; destroy renderers
  before the host `wgpu::Device`; target texture must stay alive through the frame's draw.
- Target texture must have `RenderAttachment` usage, `sampleCount == 1`, and a format matching
  `GeodeEmbedConfig::textureFormat` — a mismatched format is rejected at frame start and Geode
  silently falls back to its internal offscreen target for that frame (symptom: nothing appears
  in your swap chain).
- Runnable host: `bazel run --config=geode //examples:geode_embed -- file.svg`. Its
  platform-surface helpers also demonstrate the X11 macro-collision fix (`None`/`True`/`False`/
  `Status` from Xlib vs `wgpu::Status`) — isolate GLFW-native includes in their own translation
  unit (`examples/geode_embed_surface_linux.cc`).
- wgpu-native quirk: `Surface::getCurrentTexture` success status is `SuccessOptimal` (not
  `Success`); treat `SuccessSuboptimal` as renderable, reconfigure on `Outdated`.
- For Geode internals, headless/llvmpipe setup, and Geode test lanes, see **donner-geode-backend**.

## Public-API boundary: the ECS is internal

From `AGENTS.md` § Public API Boundary — this is a review-blocking rule:

- Donner's document model is an EnTT-based Entity-Component-System (ECS), but that is an
  **implementation detail**. Public docs and APIs speak in DOM terms: documents, elements,
  styles, resources, rendering. Rationale: consumers who program against ECS shapes lock Donner
  out of refactoring its storage, and ECS-typed APIs are unusable without internal knowledge.
- Do **not** add public APIs whose names or parameter types depend on entities, components,
  registries, systems, raw entity handles, or EnTT types — prefer DOM-shaped wrappers and typed
  value handles (`SVGElement`, `SVGDocumentHandle`).
- Existing escape hatches are documented as such and stay out of guides/examples/release notes:
  `SVGDocument::registry()` / `unsafeRegistry()` are marked "unsafe advanced escape hatch" and
  `rootEntityHandle()` is marked "for advanced use" in `donner/svg/SVGDocument.h`. Follow that
  pattern only with explicit approval; never let an example or getting-started doc use them.
- Quick self-check before sending a PR that touches public headers: does any new public
  signature mention `Registry`, `Entity`, `EntityHandle`, or an `entt::` type? If yes, wrap it.

## Known-stale doc traps (verified 2026-07)

- `docs/getting_started.md` says the parse input "needs to be mutable" and hints at a
  warnings-omitted `ParseSVG(fileData)` overload (via `examples/svg_to_png.cc`'s comment) —
  the live signature takes `std::string_view` ("will not be modified") and always requires a
  `ParseWarningSink&`.
- `examples/svg_tree_interaction.cc` (and the `getting_started.md` snippet embedding it) says
  consecutive `setStyle` calls "combine together and do not replace" — inverted.
  `setStyle` replaces the style-attribute contribution; `updateStyle` is the composing API
  (`donner/svg/components/style/StyleComponent.h`).
- References to a full "Skia" backend (`docs/getting_started.md`, the `notice_skia_text_full`
  target, `renderer = "skia"` in the module extension) describe a configuration that the Bazel
  `renderer_backend` flag no longer offers (`tiny_skia` | `geode` per
  `donner/svg/renderer/BUILD.bazel`) and CMake rejects. Don't design new consumer docs around it.
- When citing any example target or doc claim, re-verify with grep first; this repo's own docs
  have drifted before.

## Related skills

- **donner-build-test** — Bazel configs, variant lanes, CMake mirror gate.
- **donner-geode-backend** — Geode internals, WGSL, headless GPU environments.
- **donner-rendering-pipeline** — how the renderer consumes the DOM (internal view).
- **donner-release** — BCR publishing, release notes, version stamping.
- **donner-docs** — editing docs/getting_started.md and Doxygen pages.
