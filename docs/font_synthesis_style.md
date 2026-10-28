# Controlling synthetic oblique text {#FontSynthesisStyle}

`font-synthesis-style` controls permission to synthesize an oblique outline when a selected font
lacks the requested style. It inherits through text spans and supports the CSS-wide keywords.

```css
text { font-style: italic; font-synthesis-style: none; }
.oblique { font-style: oblique; font-synthesis-style: oblique-only; }
```

| Value | Synthetic oblique permission |
| --- | --- |
| `auto` | Allows the backend's existing synthesis behavior. This is the initial value. |
| `none` | Disables synthetic oblique outlines. |
| `oblique-only` | Allows synthesis for an `oblique` request, and disables it for an `italic` request. |

The property preserves face matching, glyph advances and positioning. A designed italic face remains
italic with `none`. Full-backend native variable-font axes also remain enabled. Opposite policies can
use the same loaded face without sharing the synthesized outline.

Donner currently synthesizes provider oblique outlines for its generic sans-serif and catalog faces.
This control applies to that existing path in Basic and Full text. It does not add synthesis to
other document fonts, add variable-font support to Basic, or promise synthesis wherever `auto` is
specified. The property is a CSS longhand, not an SVG presentation attribute.

`font-synthesis-weight`, `font-synthesis-position`, and the `font-synthesis` shorthand remain
unsupported. The separate `font-synthesis-small-caps` policy continues to control small-caps synthesis.
See [CSS Fonts 4, synthetic styles](https://www.w3.org/TR/css-fonts-4/#font-synthesis-style).
