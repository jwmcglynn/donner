# System architecture {#SystemArchitecture}

\tableofcontents

## Why is Donner different?

At its core, Donner is an SVG "engine". Instead of treating SVGs as static images, they are dynamic scenes which can be modified, animated, or transformed.

Many SVG libraries load an SVG, and render and image as an output, but browsers are different: SVG in browsers is a graphical version of HTML, and HTML isn't static: It can be queried, modified, and styled.

Donner intends to provide browser-level functionality as a standalone C++ library:

- Instead of simply rendering `.svg` files, Donner constructs a DOM tree that allows inspecting and modifying the file contents in-memory.
- Donner transforms the document tree into an efficient in-memory representation that can be repeatedly rendered.

Donner currently renders with Skia, which is the core rendering library used by Chrome and Firefox. Skia is a high-performance, hardware-accelerated 2D graphics library that provides a common API for drawing text, shapes, and images.

## System context

![System context diagram, Donner SVG Library](/docs/img/arch_system_context.svg)

Donner consists of a core library and a renderer, which are built with separation of concerns to enable future integration with other rendering libraries.

## Components

![Container diagram, Donner SVG Library](/docs/img/arch_container.svg)

Each component of Donner is designed to be used in isolation, with minimal dependencies on other components. This allows for easy testing and integration with other systems.

### Parser Suite

The parser suite consists of parsers in three layers:

| Namespace                 | Description                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| ------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| \ref donner::base::parser | Parsers for shared data types such as \ref donner::base::parser::NumberParser "NumberParser" and \ref donner::base::parser::LengthParser "LengthParser"                                                                                                                                                                                                                                                                                     |
| \ref donner::css::parser  | Parsers for various CSS data types, such as the top-level \ref donner::css::parser::StylesheetParser "StylesheetParser" and \ref donner::css::parser::SelectorParser "SelectorParser", as well as internal details such as \ref donner::css::parser::ColorParser "ColorParser".<br><br>These are wrapped in the \ref donner::css::CSS convenience API. Using these lower-level APIs allows for finer-grained control and error propagation. |
| \ref donner::svg::parser  | Parsers for the SVG XML format, \ref donner::svg::parser::XMLParser "XMLParser", as well as individual parsers for SVG components, such as \ref donner::svg::parser::PathParser "PathParser" and \ref donner::svg::parser::TransformParser "TransformParser".                                                                                                                                                                               |

- \ref donner::svg::parser::XMLParser "XMLParser" depends on [rapidxml_ns](https://github.com/svgpp/rapidxml_ns)
- \ref donner::base::parser::NumberParser "NumberParser" uses [absl::from_chars](https://abseil.io/about/design/charconv) as `std::from_chars` is not fully implemented in libc++.

### Styling

TODO

- SVG property registry

### CSS

Provides a fully-featured CSS3 toolkit, which can be used to parse CSS stylesheets, style strings, or selectors, and match those selectors against a document tree.

See [Using the CSS API](DonnerAPI.html#using-the-css-api) for more details.

### API Frontend

Donner provides a high-level API for interacting with the SVG document model. This API is designed to be easy to use and understand, while still providing access to the full power of the underlying document model.

The API takes a principled approach, focusing on:

- Minimal memory allocations
- Clean error propagation (`std::expected`-inspired)
- High usability with C++20 features such as concepts

See \ref DonnerAPI for more details.

### Document Model

The Document Model is built on top of the [EnTT](https://github.com/skypjack/entt) Entity-Component-System (ECS), which is used to build a tree of entities, components, and systems that represent the SVG document. It is designed to be efficient and flexible, allowing for easy modification and rendering of SVG documents.

See \ref EcsArchitecture for more details.

### Rendering Backend

The rendering backend traverses the internal ECS document model and instantiates rendering components such as \ref donner::svg::components::RenderingInstanceComponent "RenderingInstanceComponent", which are then rendered by the Skia renderer.

Rendering components are attached to the same entities as the document model components, allowing for easy synchronization between the document model and the rendering backend. When the document model is modified, the associated rendering components are invalidated.

## Base library

TODO

- String, math, and parsing helpers
- Vector2, RcString, NumberParser

## Testing strategy

TODO

<div class="section_buttons">

| Previous                     |                             Next |
| :--------------------------- | -------------------------------: |
| [Donner API](DonnerAPI.html) | [Coding style](CodingStyle.html) |

</div>
