# Emergency wrapping of SVG text

Donner supports the inherited CSS `overflow-wrap` values `normal`, `break-word`, and `anywhere`.
`word-wrap` is a legacy name alias: both names write the same cascade property. Use a style or a
stylesheet; SVG2 does not define either name as a presentation attribute.

```xml
<text style="inline-size:120px;overflow-wrap:anywhere">AnOtherwiseUnbreakableWord</text>
```

`normal` retains the existing line-breaking behavior. When wrapping is allowed by `white-space`,
`anywhere` and `break-word` add emergency opportunities for a sequence that cannot fit on its own
line. Ordinary separator opportunities take priority. No hyphen is inserted. `pre` and `nowrap`
continue to suppress soft wrapping. Text on a path retains its existing single-flow layout.

`anywhere` and `break-word` remain distinct computed values. Their specified difference concerns
min-content intrinsic sizing; Donner's fixed SVG `inline-size` layout has no min-content sizing
consumer, so their rendered wrapping behavior is the same.

Emergency breaks retain Unicode extended graphemes, including combining marks, emoji sequences,
regional-indicator pairs, and Indic conjuncts. Original required shaping clusters remain atomic.
Unicode lam-alef candidates, including transparent marks, receive a bounded font-dependent
comparison with optional substitutions and `rlig` disabled. A candidate stays together when that comparison changes its glyphs
or fallback faces. Displayed text retains the original features and glyphs. Specialized alefs use
original shaping-cluster protection instead of the additional lam-alef candidate profile.
This conservative candidate profile does not classify every font-specific required ligature.
Context-dependent font units are kept together conservatively. An indivisible unit wider than the
measure can still overflow.

Fitting words retain their original shaping. Lines preserve logical source order, and a backward
font buffer retains visual glyph order within each line. An oversized optional ligature can be
split by contextual fragment shaping; Arabic joining forms retain whole-word context. This does
not add CSS `direction`, `unicode-bidi`, or a full mixed-direction paragraph resolver.
Cross-span contextual shaping remains an existing limitation. Font-dependent grouping follows the
output of each authored shaping range; grapheme boundaries still continue across spans.

Transformations, SVG position and rotation lists, text anchoring, and DOM character indices retain
their source relationships. The underlying SVG text is unchanged. Temporary segmentation and
fragment storage is charged against the document geometry budget and capped at 64 MiB. Additional
shaping work and glyph expansion are bounded; refused preparation produces no glyphs for the span
while leaving its source available.

The Unicode classifier uses pinned Unicode 17.0.0 data (UAX #29 revision 47), with a separate Arabic
lam-alef candidate profile from Unicode 17 chapter 9. The generator records input digests and the
Unicode license. Its owning tests exercise all 766 official grapheme conformance vectors.

References: [CSS Text 3 overflow wrapping](https://www.w3.org/TR/css-text-3/#overflow-wrap-property),
[CSS character units](https://www.w3.org/TR/css-text-3/#characters),
[Unicode 17 Arabic ligatures](https://www.unicode.org/versions/Unicode17.0.0/core-spec/chapter-9/).
