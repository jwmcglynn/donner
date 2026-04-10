# Getting Started {#GettingStarted}

\tableofcontents

## Adding to Your Bazel Project

Add the following to your `MODULE.bazel` (bazel 8.0 or newer required):

```py
bazel_dep(name = "donner", version = "0.0.0")
git_override(
    module_name = "donner",
    remote = "https://github.com/jwmcglynn/donner",
    commit = "<latest commit>",
)
```

## Adding a Dependency

Donner with the default renderer is available through the `@donner` dependency, add to your rule like so:

```py
donner_cc_binary(
    name = "my_library",
    # ...
    deps = [
        "@donner",
    ],
)
```

## Loading an SVG

First include the core SVG module with:

```cpp
#include "donner/svg/SVG.h"
```

Use SVGParser to load an SVG from a string, which may be loaded from a file. Note that the string needs to be mutable as it is modified by the parser.

\snippet svg_tree_interaction.cc svg_string

\snippet svg_tree_interaction.cc svg_parse

`ParseResult` contains either the document or an error, which can be checked with `hasError()` and `error()`:

\snippet svg_tree_interaction.cc error_handling

Then get the `SVGDocument` and start using it. For example, to get the `SVGElement` for the `<path>`:

\snippet svg_tree_interaction.cc get_path

The document tree can be traversed via the Donner API, and the SVG can be modified in-memory:

\snippet svg_tree_interaction.cc path_set_style

Outputs

```
Computed style: PropertyRegistry {
  fill: PaintServer(solid Color(rgba(0, 0, 255, 255))) (set) @ Specificity(0, 0, 0)
  stroke-width: 3px (set) @ Specificity(0, 0, 0)
}
```

## Rendering an SVG

Use the backend-agnostic `Renderer` class, which resolves to the active build backend:

```cpp
#include "donner/svg/renderer/Renderer.h"

donner::svg::Renderer renderer;
renderer.draw(document);
```

Outputs can be saved to a PNG file:

```cpp
const bool success = renderer.save("output.png");
```

Or pixel data can be accessed via snapshot:

```cpp
donner::svg::RendererBitmap snapshot = renderer.takeSnapshot();
std::cout << "Size: " << renderer.width() << "x" << renderer.height() << "\n";
```

The backend is selected at build time. See \ref BuildingDonner for details on choosing between
TinySkia (lightweight default) and Skia (full-featured).

## Third-Party License Attribution

Donner bundles several third-party libraries (EnTT, stb, tiny-skia-cpp, zlib, libpng, FreeType,
HarfBuzz, woff2, brotli, Skia). Most are under permissive licenses that require you to reproduce
their copyright notice and license text when redistributing binaries built from Donner. To make
this painless, Donner ships a `donner_notice_file` rule that aggregates every required license
into a single `NOTICE.txt` you can embed in your application.

Pick the variant that matches your build configuration:

| Variant                            | Bazel target                                            |
| ---------------------------------- | ------------------------------------------------------- |
| Default (TinySkia)                 | `@donner//third_party/licenses:notice_default`          |
| TinySkia + `--config=text-full`    | `@donner//third_party/licenses:notice_text_full`        |
| Skia + `--config=text-full`        | `@donner//third_party/licenses:notice_skia_text_full`   |

### Previewing the aggregated notice

To see exactly what text your users will receive, build the variant target and print its output.
When Donner is a dependency of your project the files land under
`bazel-bin/external/donner+/third_party/licenses/`:

```sh
# Default (TinySkia) variant:
bazel build @donner//third_party/licenses:notice_default
cat bazel-bin/external/donner+/third_party/licenses/notice_default.txt
```

When working inside the Donner repo itself, drop the `external/donner+` prefix:

```sh
bazel build //third_party/licenses:notice_default
cat bazel-bin/third_party/licenses/notice_default.txt
```

Each variant produces two files next to each other:

  - `notice_<variant>.txt` — the concatenated NOTICE you embed in your app.
  - `notice_<variant>.json` — a machine-readable manifest (package name, version, SPDX
    identifier, upstream URL, license text path) in case you need to drive your own formatting.

### Embedding the NOTICE into your application

Wire the notice target into your own binary as a data dependency, then load it at runtime. The
`donner_notice_file` rule exposes its NOTICE.txt under an `output_group = "notice"`, so you can
pick it out cleanly with `filegroup` and feed it through `//tools:embed_resources` (a helper
Donner already exposes) to produce a linkable C++ symbol:

```py
load("@donner//build_defs:rules.bzl", "donner_cc_binary")

# Pull just the NOTICE.txt out of the notice target (it also produces a
# .json manifest we don't need at runtime).
filegroup(
    name = "notice_txt",
    srcs = ["@donner//third_party/licenses:notice_default"],
    output_group = "notice",
)

genrule(
    name = "embed_notice",
    srcs = [":notice_txt"],
    outs = [
        "embedded/notice_embedded.h",
        "embedded/notice_default_txt.cpp",
    ],
    cmd = """
        mkdir -p $(@D)/embedded
        $(location @donner//tools:embed_resources) \\
            --out $(@D)/embedded \\
            --header notice_embedded.h \\
            kDonnerNotice=$(location :notice_txt)
    """,
    tools = ["@donner//tools:embed_resources"],
)

cc_library(
    name = "embedded_notice",
    srcs = ["embedded/notice_default_txt.cpp"],
    hdrs = ["embedded/notice_embedded.h"],
)

donner_cc_binary(
    name = "my_app",
    srcs = ["my_app.cc"],
    deps = [
        ":embedded_notice",
        "@donner",
    ],
)
```

The generated `.cpp` filename mirrors the input filename with non-alphanumerics turned into
underscores (so `notice_default.txt` becomes `notice_default_txt.cpp`).

`embed_resources` generates a header that exposes each resource as a
`std::span<const unsigned char>` inside the `donner::embedded` namespace, so you can surface it
behind an `--about` flag or a menu item:

```cpp
#include "embedded/notice_embedded.h"

#include <span>
#include <string_view>

std::string_view thirdPartyLicenses() {
  const auto& span = donner::embedded::kDonnerNotice;
  return std::string_view(
      reinterpret_cast<const char*>(span.data()), span.size());
}
```

If you prefer a simpler approach, you can also declare the notice target as a `data` dependency
on your binary and read the file at runtime via Bazel runfiles — whichever fits your
distribution model.

### Keeping the attribution in sync

The variant lists live in
[`third_party/licenses/BUILD.bazel`](https://github.com/jwmcglynn/donner/blob/main/third_party/licenses/BUILD.bazel)
and are kept in sync with Donner's `//examples:svg_to_png` dependency graph. When you change
build configs (enabling `--config=text-full`, switching to Skia), update your consumer BUILD
files to reference the matching `notice_*` target. The
[build report](build_report.md#external-dependencies) enumerates every third-party dep per
variant alongside its SPDX identifier and upstream link.

<div class="section_buttons">

| Previous           |                         Next |
| :----------------- | ---------------------------: |
| [Home](index.html) | [Donner API](DonnerAPI.html) |

</div>
