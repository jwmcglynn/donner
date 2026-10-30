# Text indentation

Donner supports inherited CSS `text-indent` as a signed absolute length, `em`, or percentage,
with initial value `0`. It accepts CSS-wide keywords and the normal cascade through stylesheets
and inline styles. SVG2 does not define a `text-indent` presentation attribute.

```svg
<text style="inline-size:160px;text-indent:1.5em">An indented first line followed by ordinary lines.</text>
```

The margin applies to the first formatted line of a `<text>` with a positive `inline-size`,
including an explicit area with `white-space:pre` or `nowrap`. Percentages use that text area's
own inline measure. `em` computes against the declaring element's unadjusted computed font-size
before inheritance; a descendant's font-size or `font-size-adjust` does not reinterpret it.
An inline `<tspan>` does not establish another indented block. Text without an inline-size area
and text-on-path retain their existing flow.

The margin participates in first-line fitting, emergency wrapping and CSS tab stops. SVG
relative positions apply afterward. A forced newline consumes the first line even when it is
empty; an empty span does not. Later lines have no margin. Negative values enlarge the first
line's available measure. A margin wider than the area still admits the first typographic unit.
The margin is excluded from `textLength` content stretching.

This property preserves Donner's existing per-line pen-extent anchoring profile: an indent of
`I` contributes `I`, `I/2`, or `0` to the first content position for start, middle, or end anchoring.
This is a compatibility profile, not a claim of complete SVG2 content-area and line-box
alignment. The existing base progression is left-to-right horizontally and top-to-bottom
vertically. Backward-shaped runs retain their shaping; paragraph direction and bidi layout
remain existing limitations. This feature adds no `text-align` extension.

The `hanging` and `each-line` flags, calculated values, and `ex`, `ch`, `rem`, and viewport units
are rejected rather than accepted without effect. Other wrapping and text-path limitations
are unchanged.

References: [CSS Text 3 text-indent](https://www.w3.org/TR/css-text-3/#text-indent-property),
[SVG2 text layout](https://www.w3.org/TR/SVG2/text.html#TextLayoutAlgorithm),
[SVG2 inline-size](https://www.w3.org/TR/SVG2/text.html#InlineSizeProperty).
