# System Architecture

## High-level Architecture

<div style="text-align: center">
![System context diagram, Donner SVG Library](/docs/img/Donner%20-%20System%20Context.svg)

*[System context diagram, Donner SVG Library]*
</div>

At its core, Donner is an SVG engine. Instead of treating SVGs as static images, they are dynamic scenes which can be modified, animated, or transformed.

Many SVG libraries load an SVG, and render and image as an output, but browsers are different: SVG in browsers is a graphical version of HTML, and HTML isn't static: It can be queried, modified, and styled.

Donner allows loading SVGs, interacting with them through the Donner API, and then transforms the in-memory representation of the document into draw commands to render the document each frame.

Donner does not render the document itself, but instead uses Skia to do the rendering, which is the same library that Chrome uses. Skia technically supports rendering SVGs, but Donner provides a more browser-like feature set.


<div style="text-align: center">
![Container diagram, Donner SVG Library](/docs/img/Donner%20-%20Container.svg)

*[Container diagram, Donner SVG Library]*
</div>

- Parser
  - Parses XML with rapidxml_ns
  - Parser suite for SVG components, such as paths, transforms, lengths, etc.
- ECS Document Model
- API Frontend
- Renderer
- CSS
  - Hand-rolled CSS3 parser
  - Cascading and selector logic
  - SVG property registry
- Base library
  - String, math, and parsing helpers
  - Vector2, RcString, NumberParser
- Test Suite
