# Donner SVG Architecture Summary

This document summarizes the core architectural concepts of the Donner SVG library, based on information from the `docs/` directory.

## Core Philosophy

*   **Dynamic SVG Engine:** Unlike static renderers, Donner treats SVGs as dynamic scenes, similar to browsers. It builds an in-memory Document Object Model (DOM) that can be inspected, modified, styled, and re-rendered efficiently.
*   **Separation of Concerns:** Components (parsing, CSS, styling, rendering) are designed for isolation to facilitate testing and potential integration with different backends.

## Main Components

*   **Parser Suite:** A layered set of parsers:
    *   `donner::xml`: Base XML parsing and document tree (`XMLParser`, `XMLDocument`).
    *   `donner::svg::parser`: Parses SVG-specific XML structure and attributes (`SVGParser`, `PathParser`, `TransformParser`).
    *   `donner::css::parser`: Parses CSS stylesheets, selectors, declarations, and values (`StylesheetParser`, `SelectorParser`, `ColorParser`). Wrapped by `donner::css::CSS`.
    *   `donner::parser`: Parses shared data types like numbers and lengths (`NumberParser`, `LengthParser`).
*   **CSS (`donner::css`):** Provides a CSS3 toolkit for parsing stylesheets/selectors and matching them against a document tree. It produces raw `SelectorRule` and `Declaration` objects.
*   **Styling (`donner::svg::Property`, `PropertyRegistry`, `StyleSystem`):** Consumes raw CSS data, implements the SVG style model (including presentation attributes), handles cascading and inheritance, and computes final styles (`ComputedStyleComponent`).
*   **Document Model (ECS):**
    *   Built using the **EnTT** Entity-Component-System library.
    *   **Entities:** Represent SVG elements (e.g., `SVGPathElement`).
    *   **Components:** Store data associated with entities (e.g., `TreeComponent`, `StyleComponent`, `PathComponent`).
    *   **Systems:** Contain logic operating on components (e.g., `LayoutSystem`, `StyleSystem`, `ShapeSystem`). Systems manage global state or perform stateless operations.
*   **API Frontend (`donner::svg::SVG*Element`):** A high-level, user-facing API providing efficient wrappers around ECS entities and components (e.g., `SVGPathElement` interacts with `PathComponent`). Focuses on minimal allocations and clean error handling.
*   **Rendering Backend:** Traverses the ECS model, instantiates rendering data (`RenderingInstanceComponent`), and uses a renderer (currently **Skia**) to produce output.
*   **Base Library (`donner::base`):** Contains common utilities (`RcString`, `Vector2`, `Transform`, `Length`) used across the library.

## Rendering Pipeline (ECS Transformations)

Donner processes the SVG through several stages, transforming components along the way:

1.  **User-provided Tree:** Initial state after parsing, represented by components like `StyleComponent`, `PathComponent`, `RectComponent`.
2.  **Style Propagation (`StyleSystem`):** Applies CSS rules, inheritance, and presentation attributes. Transforms `StyleComponent` -> `ComputedStyleComponent`.
3.  **Computed Tree (`LayoutSystem`, `ShapeSystem`, etc.):** Calculates final geometry and other properties based on computed styles. Transforms `PathComponent` -> `ComputedPathComponent` (containing `PathSpline`). This handles SVG2 features where geometry can be defined in CSS.
4.  **Render Tree Instantiation (`RenderingContext`):** Traverses the computed tree to create a sorted list of `RenderingInstanceComponent` objects, ready for the rendering backend.

## Key Concepts

*   **ECS:** Central to storing and manipulating the SVG document efficiently.
*   **Component Transformation:** The pipeline relies on transforming components (e.g., `Style` -> `ComputedStyle`, `Path` -> `ComputedPath`) at different stages.
*   **CSS Integration:** CSS parsing is separate from styling logic. The Styling component interprets the parsed CSS data according to SVG rules.
*   **SVG2 Compliance:** The architecture handles SVG2 features like defining shape attributes via CSS.
