# CSS rectangle length values

SVG2 gives [x](https://w3c.github.io/svgwg/svg2-draft/single-page.html#XProperty)
the CSS grammar `<length-percentage>`. The
[presentation attribute syntax](https://w3c.github.io/svgwg/svg2-draft/single-page.html#presentation-attribute-css-value)
also permits numbers in user units. That relaxation does not apply to CSS declarations.
[CSS Values 3 lengths](https://www.w3.org/TR/2018/CR-css-values-3-20180814/#lengths)
permit unitless zero, including `.0`.

The inline and stylesheet `x:8` declarations are invalid, leaving the presentation attribute
`x="4"` effective. Their references contain opaque red pixels in `[4,8) x [4,8)`.
The valid inline `x:.0` overrides `x="4"`; its reference contains opaque red pixels in
`[0,4) x [4,8)`. Every image is 16x16 RGBA, with all other pixels transparent black.

The PNGs are constructed directly from these integer coordinate intervals, not rendered from
an SVG implementation. These pairs cover rectangle x syntax, not general cascade behavior or
other geometry elements.
