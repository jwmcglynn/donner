# System Architecture {#SystemArchitecture}

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

| Namespace                | Description                                                                                                                                                                                                                                                                                                                                                                                                                                 |
| ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| \ref donner::parser      | Parsers for shared data types such as \ref donner::parser::NumberParser "NumberParser" and \ref donner::parser::LengthParser "LengthParser"                                                                                                                                                                                                                                                                                                 |
| \ref donner::css::parser | Parsers for various CSS data types, such as the top-level \ref donner::css::parser::StylesheetParser "StylesheetParser" and \ref donner::css::parser::SelectorParser "SelectorParser", as well as internal details such as \ref donner::css::parser::ColorParser "ColorParser".<br><br>These are wrapped in the \ref donner::css::CSS convenience API. Using these lower-level APIs allows for finer-grained control and error propagation. |
| \ref donner::svg::parser | Parsers for the SVG XML format, \ref donner::svg::parser::SVGParser "SVGParser", as well as individual parsers for SVG components, such as \ref donner::svg::parser::PathParser "PathParser" and \ref donner::svg::parser::TransformParser "TransformParser".                                                                                                                                                                               |
| \ref donner::xml         | \ref donner::xml::XMLParser "XMLParser" and an XML document tree represented by \ref donner::xml::XMLDocument "XMLDocument" and \ref donner::xml::XMLNode "XMLNode".                                                                                                                                                                                                                                                                        |

- \ref donner::parser::NumberParser "NumberParser" uses [absl::from_chars](https://abseil.io/about/design/charconv) as `std::from_chars` is not fully implemented in libc++.

### CSS

Provides a fully-featured CSS3 toolkit, which can be used to parse CSS stylesheets, style strings, or selectors, and match those selectors against a document tree.

See [Using the CSS API](DonnerAPI.html#using-the-css-api) for more details.

The CSS layer parses stylesheets into lists of \ref donner::css::SelectorRule "SelectorRule" objects, which contains:

- A \ref donner::css::Selector "Selector" object, which contains a matching pattern that can be used to match against a document tree.
- A list of \ref donner::css::Declaration "Declaration" objects, which correspond to the key-value pairs such as `color: red`

At this layer, the style information has no semantics, it contains raw parsed data and the ability to cascade it to the document tree. This raw data is consumed by the **Styling** component to parse these values into meaningful styling information.

### Styling

Consumes information from the CSS parser and implements the SVG style model. This includes:

- \ref donner::svg::Property, which holds the high-level style information. `fill: red` turns into:
  ```cpp
  Property<PaintServer, PropertyCascade::PaintInherit> fill{
      "fill", []() -> std::optional<PaintServer> {
        return PaintServer::Solid(css::Color(css::RGBA::RGB(0, 0, 0)));
      }};
  ```

  - This also holds state necessary for CSS cascading and default values, as this information is property-specific.

- \ref donner::svg::PropertyRegistry, which contains all known properties used by Donner. This corresponds to the full set of SVG presentation attributes ([supported list from github](https://github.com/jwmcglynn/donner/issues/149), [full list from SVG2 spec](https://www.w3.org/TR/2018/CR-SVG2-20181004/styling.html#PresentationAttributes))

\ref donner::svg::components::StyleSystem "StyleSystem" is the top-level component that manages the styling of the document tree.

#### Data model

Style information is held on each entity inside \ref donner::svg::components::StyleComponent. During the rendering process, CSS cascading and inheritance is performed and cached on \ref donner::svg::components::ComputedStyleComponent.

- \ref donner::svg::components::ComputedStyleComponent "ComputedStyleComponent" contains absolute styling information for each entity at render time.

### API Frontend

Donner provides a high-level API for interacting with the SVG document model. This API is designed to be easy to use and understand, while still providing access to the full power of the underlying document model.

The API takes a principled approach, focusing on:

- Minimal memory allocations
- Clean error propagation (`std::expected`-inspired)
- High usability with C++20 features such as concepts

See \ref DonnerAPI for more details.

### Document Model

The Document Model is built on top of the [EnTT](https://github.com/skypjack/entt) Entity-Component-System (ECS), which is used to build a tree of entities, components, and systems that represent the SVG document. It is designed to be efficient and flexible, allowing for easy modification and rendering of SVG documents.

- See \ref EcsArchitecture for more details.
- See \ref ecs_systems for a list of systems.

### Rendering Backend

The rendering backend traverses the internal ECS document model and instantiates rendering components such as \ref donner::svg::components::RenderingInstanceComponent "RenderingInstanceComponent", which are then rendered by the Skia renderer.

Rendering components are attached to the same entities as the document model components, allowing for easy synchronization between the document model and the rendering backend. When the document model is modified, the associated rendering components are invalidated.

## Base library

The `//donner/base` library contains common utility code used by the other libraries. This includes:

- \ref donner::RcString "RcString" - a reference-counted string class
- \ref donner::Vector2 "Vector2" - a simple 2D vector class
- \ref donner::Transform "Transform" - which handles 2D affine transformation and stores a 3x2 matrix.
- \ref donner::Length "Length" - a simple class to represent a length with a specific unit, such as `10px` or `10cm`.
- and more...

This library also contains common parsers such as \ref donner::parser::NumberParser "NumberParser", which can parse a string into a number.

The base library has minimal dependencies and the types within it may be suitable for other libraries, however the base library is not publicly exported.

## Testing strategy

Donner has a multi-level testing strategy and aims to make the library production-grade and suitable for parsing untrusted inputs (eventually).

### Unit tests

All components should be unit-tested, and test coverage is measured using [Codecov](https://app.codecov.io/gh/jwmcglynn/donner).

### Image comparison tests

As SVG is a visual format, image comparison tests are used to validate the rendered output.

These come in three flavors:

- Low-level "unittest" image comparison using ASCII art.

  ```cpp
  TEST(SVGPatternElementTests, UserSpaceOnUseRendering) {
    const AsciiImage generatedAscii = RendererTestUtils::renderToAsciiImage(R"-(
          <pattern id="a" patternUnits="userSpaceOnUse" width="8" height="8">
            <rect x="0" y="0" width="4" height="4" fill="lime" />
            <rect x="4" y="4" width="4" height="4" fill="gray" />
          </pattern>
          <rect width="16" height="16" fill="url(#a)" />
          )-");

    EXPECT_TRUE(generatedAscii.matches(R"(
          ####....####....
          ####....####....
          ####....####....
          ####....####....
          ....++++....++++
          ....++++....++++
          ....++++....++++
          ....++++....++++
          ####....####....
          ####....####....
          ####....####....
          ####....####....
          ....++++....++++
          ....++++....++++
          ....++++....++++
          ....++++....++++
          )"));
  }
  ```

- High-level "integration" image comparison using the [pixelmatch-cpp17](https://github.com/jwmcglynn/pixelmatch-cpp17) library.

  `bazel run //donner/svg/renderer/tests:renderer_tests`

- Using the external [Resvg Test Suite](ReSvgTestSuite.html) to validate against a large corpus of SVG files and comparing against the reference output with pixelmatch.

  `bazel run //donner/svg/renderer/tests:resvg_test_suite`

### Fuzzing

Since SVG and CSS require a large collection of parsers, fuzz tests are individually written for each parser.

- These are written using libfuzzer and can be run by following the [fuzzing instructions](fuzzing.md).

- In CI the fuzzers are executed with a small corpus containing interesting inputs for bugs which have been previously fixed.

- Note that the fuzzers are not currently run automatically, but may be onboarded to [OSS-Fuzz](https://google.github.io/oss-fuzz/) in the future.

<div class="section_buttons">

| Previous                     |                             Next |
| :--------------------------- | -------------------------------: |
| [Donner API](DonnerAPI.html) | [Coding style](CodingStyle.html) |

</div>
