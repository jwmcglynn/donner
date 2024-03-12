# Getting started

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
cc_binary(
    name = "my_library",
    # ...
    deps = [
        "@donner",
    ],
)
```

## Loading an SVG

TODO: Change this to be <donner/...>

First include the core SVG module with:

```cpp
#include "src/svg/svg.h"
```

Use XMLParser to load an SVG from a string, which may be loaded from a file. Note that the string needs to be mutable as it is modified by the parser.

```cpp
// This is the base SVG we are loading, a simple path containing a line.
MutableString svgContents(R"(
  <svg xmlns="http://www.w3.org/2000/svg" width="100" height="100">
    <path d="M 1 1 L 2 3" fill="blue" stroke-width="3" />
  </svg>
)");

donner::ParseResult<donner::svg::SVGDocument> maybeResult =
    donner::svg::XMLParser::ParseSVG(svgContents);
```

`ParseResult` contains either the document or an error, which can be checked with `hasError()` and `error()`:

```cpp
if (maybeResult.hasError()) {
  const auto& e = maybeResult.error();
  std::cerr << "Parse Error " << e.line << ":" << e.offset << ": " << e.reason << "\n";
  // Handle the error per your project's conventions here.
}
```

Then get the `SVGDocument` and start using it. For example, to get the `SVGElement` for the `<path>`:

```cpp
donner::svg::SVGDocument document = std::move(maybeResult.result());

// querySelector supports standard CSS selectors, anything that's valid when defining a CSS rule
// works here too, for example querySelector("svg > path[fill='blue']") is also valid and will
// match the same element.
auto maybePath = document.svgElement().querySelector("path");
UTILS_RELEASE_ASSERT_MSG(maybePath, "Failed to find path element");
```

The document tree can be traversed via the Donner API, and the SVG can be modified in-memory:

```cpp
// Set styles, note that these combine together and do not replace.
path.setStyle("fill: red");
path.setStyle("stroke: white");

// Get the parsed, cascaded style for this element and output it to the console.
std::cout << "Computed style: " << path.getComputedStyle() << "\n";
```

Outputs

```
Computed style: PropertyRegistry {
  fill: PaintServer(solid Color(0, 0, 255, 255)) (set) @ Specificity(0, 0, 0)
  stroke-width: 3px (set) @ Specificity(0, 0, 0)
}
```

## Rendering an SVG

To render the SVG using the Skia renderer, include the renderer header and invoke it like so

```cpp
donner::svg::RendererSkia renderer;
renderer.draw(document);
```

Outputs can be saved to a PNG file

```cpp
const bool success = renderer.save("output.png");
```

Or pixel data can be accessed directly

```cpp
std::span<const uint8_t> data = renderer.pixelData();
std::cout << "Size: " << renderer.width() << "x" << renderer.height() << "\n";
```
