# Automatic radius references

These seven SVG/PNG pairs isolate explicit `rx="auto"`, `ry="auto"`, both values,
and CSS automatic rectangle radii. An omitted counterpart also has initial value `auto`.
Both automatic radii have used value zero: rectangles retain square corners and ellipses
produce no geometry.

Every reference is the same independently specified 100x100 RGBA image: transparent pixels
except an opaque red rectangle covering `[20,60) x [20,60)`. The PNG bytes are copied without
rerendering or regenerating an oracle from the implementation under test. The ellipse cases
include that rectangle as a visible control and must add no blue pixels.

The assertions follow [SVG2 rx](https://svgwg.org/svg2-draft/geometry.html#RX),
[ry](https://svgwg.org/svg2-draft/geometry.html#RY),
[rectangles](https://svgwg.org/svg2-draft/shapes.html#RectElement), and
[ellipses](https://svgwg.org/svg2-draft/shapes.html#EllipseElement).
These cases do not depend on the draft's conflicting rectangle percentage-radius rules.
The manifest's existing requirement IDs retain their original baseline identifiers for compatibility.
