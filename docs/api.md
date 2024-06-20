# Donner API {#DonnerAPI}

The Donner API is a C++ API for interacting with SVG documents. It provides a low-level interface for parsing, traversing, and modifying SVG documents.

## Parsing an SVG

To parse an SVG document, use the `XMLParser` class:

```cpp
donner::ParseResult<donner::svg::SVGDocument> maybeResult =
    donner::svg::XMLParser::ParseSVG(svgContents);
```

`ParseResult` contains either the document or an error, which can be checked with `hasError()` and `error()`:

```cpp
if (maybeResult.hasError()) {
  const auto& e = maybeResult.error();
  std::cerr << "Parse Error " << e << "\n";
  // Handle the error per your project's conventions here.
}
```

Then get the `SVGDocument` and start using it. For example, to get the `SVGElement` for the `<path>`:

```cpp
donner::svg::SVGDocument document = std::move(maybeResult.result());

std::optional<donner::svg::SVGElement> maybePath = document.svgElement().querySelector("path");
```

## SVGElement

`SVGElement` implements a DOM-like API for querying and modifying the SVG document.

The document tree is traversable with `firstChild()`, `lastChild()`, `nextSibling()`, and `previousSibling()`. The element's tag name can be retrieved with `tagName()`.

Example iterating over children:

```cpp
for (std::optional<SVGElement> child = element.firstChild(); child;
      child = child->nextSibling()) {
  std::cout << "Child tag name: " << child->tagName() << "\n";
}
```

TODO: Documentation on the namespace hierarchy
TODO: Documentation about rendering
TODO: Documentation about css
