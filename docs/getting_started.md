# Getting Started {#GettingStarted}

\tableofcontents

## Adding to Your Bazel Project

Donner's root `@donner` library uses the tiny-skia renderer by default. A separate Bazel module
can use a version from the Bazel Central Registry once that version is published, or build
against a checked-out source tree. The checked-in
[consumer example](https://github.com/jwmcglynn/donner/blob/main/examples/bazel_consumer/README.md)
exercises the latter path and is also
used by BCR preflight against the release candidate.

For the current v0.8 source tree, put this in your `MODULE.bazel`:

```py
module(name = "my_svg_app")

bazel_dep(name = "donner", version = "0.8.0")
# On macOS, register the Apple toolchain before rules_cc selects a host toolchain.
bazel_dep(name = "apple_support", version = "2.10.1")
bazel_dep(name = "rules_cc", version = "0.2.26")
```

Use standard `rules_cc` targets in `BUILD.bazel`:

```py
load("@rules_cc//cc:defs.bzl", "cc_binary")

cc_binary(
    name = "my_app",
    srcs = ["main.cc"],
    deps = ["@donner"],
)
```

Set C++20 in your `.bazelrc`, as the checked-in consumer does:

```text
build --cxxopt=-std=c++20
build --host_cxxopt=-std=c++20
```

Build against an adjacent checkout with a module override (replace the path with your Donner
checkout):

```sh
bazel build --override_module=donner=/path/to/donner //:my_app
```

Once a BCR version is published, set the dependency to the version listed in the registry and
run `bazel build //:my_app` without the override. Donner is built and tested against the Bazel
version pinned in its `.bazelversion` (currently 8.8.1).

## Adding to Your CMake Project {#GettingStartedCMake}

CMake support is generated from Donner's Bazel build metadata. Start from a Donner source checkout
and generate the CMake files once before configuring your application:

```sh
cd /path/to/donner
python3 tools/cmake/gen_cmakelists.py
```

Then add Donner to your own `CMakeLists.txt` with `add_subdirectory()` and link the exported
`donner` target:

```cmake
cmake_minimum_required(VERSION 3.20)
project(my_svg_app LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(DONNER_SOURCE_DIR "/path/to/donner" CACHE PATH "Path to a Donner source checkout")
add_subdirectory("${DONNER_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/donner")

add_executable(my_svg_app main.cc)
target_link_libraries(my_svg_app PRIVATE donner)
```

Use the same public headers as the Bazel build:

\snippet svg_quickstart.cc includes

The complete runnable example is under `examples/cmake_consumer/`. It parses a small SVG, queries
the DOM, renders it, and exits non-zero if any step fails. From the Donner checkout:

```sh
python3 tools/cmake/gen_cmakelists.py
cmake -S examples/cmake_consumer -B build/cmake-consumer
cmake --build build/cmake-consumer --target donner_cmake_consumer
ctest --test-dir build/cmake-consumer --output-on-failure
```

From another source tree, pass the Donner checkout explicitly:

```sh
cmake -S /path/to/donner/examples/cmake_consumer -B build/cmake-consumer \
  -DDONNER_SOURCE_DIR=/path/to/donner
cmake --build build/cmake-consumer --target donner_cmake_consumer
ctest --test-dir build/cmake-consumer --output-on-failure
```

Regenerate the CMake files after updating Donner or changing its Bazel build graph. Generated
`CMakeLists.txt` files are intentionally ignored by Git; handwritten examples under `examples/`
are the exception.

## Loading an SVG

First include the core SVG module, `donner/svg/SVG.h`. Then use `SVGParser` to load an SVG from a
string, which may have been read from a file:

\snippet svg_tree_interaction.cc svg_string

\snippet svg_tree_interaction.cc svg_parse

`ParseResult` contains either the document or an error, which can be checked with `hasError()` and `error()`:

\snippet svg_tree_interaction.cc error_handling

Then get the `SVGDocument` and start using it. For example, to get the `SVGElement` for the `<path>`:

\snippet svg_tree_interaction.cc get_path

The document tree can be traversed and modified in memory:

\snippet svg_tree_interaction.cc path_set_style

For multi-threaded DOM access and removed-element lifetime behavior, see
\ref SvgDomThreadingAndLifetime.

The complete example prints the path geometry, applies style edits, and then prints the computed style.

## Rendering an SVG

Include `donner/svg/renderer/Renderer.h` and use the `Renderer` class, which resolves to the
backend selected at build time. Drawing renders the document in memory, and `save()` writes it to a
PNG file:

\snippet svg_to_png.cc render

Pixel data can also be read back with a snapshot:

\snippet svg_quickstart.cc snapshot

\ref svg_quickstart.cc "The quickstart example" parses a document, edits it, and renders it in one
short program. See \ref BuildingDonner for details on choosing between tiny_skia (the compact CPU
default) and Geode (the GPU backend).

## Animating an SVG {#GettingStartedAnimation}

The default parser recognizes the declarative SVG animation elements, `<animate>`, `<set>`,
`<animateTransform>`, `<animateMotion>` and `<mpath>`, with no extra option or build flag. See
\ref elements_animation for the supported timing and interpolation features.

\snippet svg_animation.cc animated_svg

Donner has no animation clock of its own. The host sets the document time in seconds with
`SVGDocument::setTime()`, typically from its frame clock, and draws again. Each `draw()` samples
every animation at the document's current time; nothing else needs to be called between frames.
Any finite time is accepted, so a host can step forward, seek backward or render frames out of
order:

\snippet svg_animation.cc set_time

Pointer input stays with the host. To start `begin="id.click"` and similar event-based timing, hit
test the pointer, then deliver the event to the element under it. The triggered interval starts at
the current document time plus any offset in the trigger, such as `click+1s`, and the call returns
`false` when no animation timing changed:

\snippet svg_animation.cc dispatch_event

`SVGDocument::dispatchAnimationEvent()` delivers other named events, and
`SVGDocument::beginAnimation()` and `SVGDocument::endAnimation()` schedule an animation element's
begin or end at an offset from the current time. Donner has no "animation finished" callback: the
host chooses how long to play, and `SVGDocument::animationEvents()` lists the begin, repeat and end
boundaries in a time interval so the host can react to them without sampling every frame:

\snippet svg_animation.cc lifecycle_events

`SVGDocument::restartAnimations()` returns the document to time zero and discards event-started
intervals, for example when playback starts over:

\snippet svg_animation.cc restart

The document's processing mode, set with `SVGDocument::Settings::processingMode` when parsing,
limits animation for untrusted content:

| Mode                           | Authored animation | Host events and begin/end scheduling |
| ------------------------------ | ------------------ | ------------------------------------ |
| `DynamicInteractive` (default) | Runs               | Accepted                             |
| `SecureAnimated`               | Runs               | Rejected                             |
| `SecureStatic`                 | Never applies      | Rejected                             |

SVG documents loaded through `<image>` are parsed in `SecureStatic` mode, so they render their
static content, unless the host replaces `SVGDocument::Settings::svgParseCallback` with its own
parser callback.

The complete program, `examples/svg_animation.cc`, checks that the click starts an animation and
that the lifecycle boundaries match the document; run it with `bazel run //examples:svg_animation`.
The editor's Preview mode plays animations with this same API, and `donner-svg --gif` exports an
animation as an animated GIF (see \ref DonnerSvgTool).

## Third-Party License Attribution

Donner bundles several third-party libraries (EnTT, stb, tiny-skia-cpp, zlib, libpng, FreeType,
HarfBuzz, woff2, brotli). Most are under permissive licenses that require you to reproduce
their copyright notice and license text when redistributing binaries built from Donner. Donner
ships a `donner_notice_file` rule that aggregates every required license into a single
`NOTICE.txt` that you can embed in your application.

Pick the variant that matches your build configuration:

| Variant                          | Bazel target                                     |
| -------------------------------- | ------------------------------------------------ |
| Default (tiny-skia)              | `@donner//third_party/licenses:notice_default`   |
| tiny-skia + `--config=text-full` | `@donner//third_party/licenses:notice_text_full` |

### Previewing the aggregated notice

To see the exact text your users will receive, build the variant target and print its output.
When Donner is a dependency of your project the files land under
`bazel-bin/external/donner+/third_party/licenses/`:

```sh
# Default (tiny-skia) variant:
bazel build @donner//third_party/licenses:notice_default
cat bazel-bin/external/donner+/third_party/licenses/notice_default.txt
```

Inside the Donner repository itself, drop the `external/donner+` prefix:

```sh
bazel build //third_party/licenses:notice_default
cat bazel-bin/third_party/licenses/notice_default.txt
```

Each variant produces two files next to each other:

- `notice_<variant>.txt`: the concatenated NOTICE to embed in your application.
- `notice_<variant>.json`: a machine-readable manifest (package name, version, SPDX
  identifier, upstream URL, license text path) for producing your own formatting.

### Embedding the NOTICE into your application

The `donner_notice_file` rule exposes its text file (`notice_default.txt` for
`notice_default`) under `output_group = "notice"`; the `.json` manifest is not needed at runtime.
Select the text with a `filegroup` and pass it through Donner's `//tools:embed_resources` helper to
produce a linkable C++ symbol. With `donner_cc_binary` loaded from `@donner//build_defs:rules.bzl`,
the recipe is:

<!-- example: examples/BUILD.bazel [embed_notice] -->

```py
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
    includes = ["."],
)

donner_cc_binary(
    name = "third_party_notice",
    srcs = ["third_party_notice.cc"],
    deps = [
        ":embedded_notice",
        "@donner",
    ],
)
```

`includes = ["."]` lets sources include the generated header as `embedded/notice_embedded.h` from
any package. Donner builds this recipe as `//examples:third_party_notice`.

The generated `.cpp` filename is the input filename with non-alphanumeric characters replaced by
underscores, so `notice_default.txt` becomes `notice_default_txt.cpp`.

`embed_resources` generates a header that exposes each resource as a
`std::span<const unsigned char>` in the `donner::embedded` namespace. After
`#include "embedded/notice_embedded.h"`, the application can show the text behind an `--about` flag
or a menu item:

\snippet third_party_notice.cc third_party_licenses

Alternatively, declare the notice target as a `data` dependency of your binary and read the file
at runtime through Bazel runfiles.

### Keeping the attribution in sync

The variant lists live in
[`third_party/licenses/BUILD.bazel`](https://github.com/jwmcglynn/donner/blob/main/third_party/licenses/BUILD.bazel)
and are kept in sync with Donner's `//examples:svg_to_png` dependency graph. When you change
build configuration (for example, enabling `--config=text-full`), update your consumer BUILD
files to reference the matching `notice_*` target. The
[build report](build_report.md#external-dependencies) enumerates every third-party dep per
variant alongside its SPDX identifier and upstream link.

<div class="section_buttons">

| Previous           |                         Next |
| :----------------- | ---------------------------: |
| [Home](index.html) | [Donner API](DonnerAPI.html) |

</div>
