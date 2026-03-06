# Getting Started {#GettingStarted}

\tableofcontents

## Adding to your bazel project

Add the following to your `MODULE.bazel` (bazel 7.0.0 required):

```py
bazel_dep(name = "donner", version = "0.0.0")
git_override(
    module_name = "donner",
    remote = "https://github.com/jwmcglynn/donner",
    commit = "<latest commit>",
)
```

## Adding a dependency

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

<div class="section_buttons">

| Previous           |                         Next |
| :----------------- | ---------------------------: |
| [Home](index.html) | [Donner API](DonnerAPI.html) |

</div>
