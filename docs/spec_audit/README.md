# W3C Specification Audit Ledgers

These ledgers are the evidence for
[0071: W3C Specification Audit and Gap Closure](../design_docs/0071-w3c_spec_audit.md). They record,
row by row, how Donner compares with the SVG 2 Candidate Recommendation Snapshot of 06 October 2026
and with the latest published version of each specification it depends on. Everything is the result
of reading source at revision `b39a524a2`; nothing was built or executed.

## Implementation ledgers

Each ledger has a "Specs read" table with dated URLs, an "Inventory" section that states how its
rows were derived, the matrix, a "Tests to create" list and an "Unverified" list.

| Ledger                                                          | Scope                                                                                        | Rows |
| --------------------------------------------------------------- | -------------------------------------------------------------------------------------------- | ---- |
| [structure](structure.md)                                       | SVG 2 document structure, conformance classes, linking and the `style` element               | 214  |
| [shapes_paths](shapes_paths.md)                                 | Basic shapes, path data and the geometry properties                                          | 156  |
| [text_layout](text_layout.md)                                   | The SVG 2 text chapter: elements, positioning, the layout algorithm, text on a path          | 185  |
| [paint_servers_markers_images](paint_servers_markers_images.md) | Gradients, patterns, markers and the `image` element                                         | 199  |
| [clipping_masking](clipping_masking.md)                         | CSS Masking 1 with the SVG 2 clipping, masking and overflow rules                            | 192  |
| [filters_core](filters_core.md)                                 | Filter Effects 1: the `filter` element and property, `feBlend` to `feDropShadow`             | 212  |
| [filters_generators_lighting](filters_generators_lighting.md)   | Filter Effects 1: `feFlood` to `feTurbulence`, lighting and light sources                    | 151  |
| [animation](animation.md)                                       | SVG 2 Appendix D, SVG Animations Level 2 and SMIL Animation                                  | 145  |
| [interactivity_accessibility](interactivity_accessibility.md)   | Hit testing, `pointer-events`, `cursor`, focus, events and the accessibility attributes      | 233  |
| [painting_properties](painting_properties.md)                   | The SVG 2 painting chapter and its properties                                                | 191  |
| [text_font_properties](text_font_properties.md)                 | CSS Fonts, Text, Text Decoration, Writing Modes, Inline Layout, UAX #9, font formats, WOFF   | 566  |
| [styling_properties](styling_properties.md)                     | The SVG 2 styling chapter and the properties it requires                                     | 374  |
| [css_syntax_values](css_syntax_values.md)                       | CSS Syntax, Values, Custom Properties, Conditional Rules, Media Queries, Namespaces, Nesting | 355  |
| [selectors_cascade](selectors_cascade.md)                       | Selectors 3 and 4, CSS Cascading and Inheritance 4 to 6, CSS Scoping                         | 200  |
| [color_compositing](color_compositing.md)                       | CSS Color 4 and 5, Compositing and Blending, the rendering model, raster image requirements  | 351  |
| [transforms_coordinates](transforms_coordinates.md)             | SVG 2 coordinate systems, CSS Transforms 1 and 2, Geometry Interfaces                        | 254  |
| [xml_data_formats](xml_data_formats.md)                         | XML 1.0, Namespaces, `xml-stylesheet`, XLink, URL processing, data URLs, compressed SVG      | 162  |

## Other files

- [test_coverage](test_coverage.md) compares the SVG 2 feature set with the resvg test suite and the
  Donner SVG suite: coverage verdicts, cases to author (TC identifiers) and reference conflicts (RC
  identifiers).
- `matrix.tsv` has one line for each of the 4,140 matrix rows: ledger, key, class, impact, size and
  whether a test exists. Totals in the design document are computed from it.

## Reading a row

A row is identified by its ledger and its key. Keys start with a kind: `el:` for an element, `attr:`
for an attribute (with `@element` where the row is specific to elements), `prop:` for a property,
`val:` for one value or grammar alternative of an attribute or property, and `feat:` for a prose
rule or algorithm, followed by the short name of the specification that defines it.

Matrix columns are: Key, Class, Spec anchor, Requirement, Parse evidence, Consume evidence, Tests,
Impact, Size, Notes.

| Class                 | Meaning                                                                             |
| --------------------- | ----------------------------------------------------------------------------------- |
| `conformant`          | The implementing code was read against the requirement and a named test asserts it. |
| `conformant-untested` | The implementing code was read and matches; no test asserts it.                     |
| `partial`             | Implemented with named missing values, cases or sub-features.                       |
| `divergent`           | Implemented, and behaves differently from the specification.                        |
| `missing`             | Not implemented.                                                                    |
| `not-applicable`      | Outside what a script-free XML renderer must do, with a stated reason.              |
| `spec-ambiguity`      | The specification is unclear, or two normative texts conflict.                      |
| `legacy`              | Donner implements something the specification removed.                              |

Impact is `high`, `medium` or `low`. Size is for the row taken alone: S up to 2 engineer-days, M 3
to 7, L 8 to 20, XL above 20. One fix often closes many rows, so sizes do not add up; the design
document estimates grouped work items instead.

When a fix lands, update the affected rows, the matching `matrix.tsv` lines, and the test named in
the row, in the same change.
