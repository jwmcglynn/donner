# Text transformation

Donner supports `text-transform: none` and `text-transform: full-width` on SVG text in both text
backends. The property inherits and participates in the CSS cascade, including presentation
attributes and CSS-wide keywords. A nested `text-transform: none` restores that element's original
characters.

`full-width` follows the [CSS Text 3 mapping rules](https://www.w3.org/TR/css-text-3/#text-transform):
Unicode width mappings convert narrow forms to their full-width counterparts. This includes ASCII
letters, digits and punctuation, halfwidth katakana and Hangul, and width variants of symbols.
Characters without a width mapping remain unchanged. Halfwidth voiced marks map to Unicode combining
marks; the font backend shapes these with the adjacent kana rather than applying an extra Unicode
normalization pass.

Only preserved U+0020 spaces become U+3000 IDEOGRAPHIC SPACE. Collapsed spaces stay U+0020. This applies
to CSS whitespace modes and to preserved legacy `xml:space`. Tabs and line breaks retain their
whitespace behavior. Full-width spaces participate in wrapping, hanging, and word spacing.

The transformation is applied to a temporary shaping buffer. It does not change the SVG document's
text, normalized source spans, DOM character counts, or the source indices used by SVG positioning
lists and character metrics. Vertical orientation and font fallback use the transformed characters.
The combined transformed UTF-8 and source-index payload is limited to 64 MiB per span; an oversized
span produces no glyphs while its source remains available.

Case conversion (`uppercase`, `lowercase`, `capitalize`), `full-size-kana`, and combinations of those
values are not implemented. Such declarations are rejected, allowing the usual cascade to retain
an earlier supported value. This is partial coverage of `text-transform`.

## Unicode data

The generated mapping contains 226 one-codepoint mappings from Unicode 17.0.0 `UnicodeData.txt`.
`<narrow>` decompositions map forward; `<wide>` decompositions map in reverse. The generated header
retains the Unicode License V3 notice, source URL, version, and SHA-256 digest. The full data file is
not included in the repository.

To reproduce or verify the table, download the
[pinned Unicode data](https://www.unicode.org/Public/17.0.0/ucd/UnicodeData.txt), then run:

```sh
python3 tools/unicode/generate_full_width.py UnicodeData.txt --check
```

The generator validates the pinned input digest and mapping count before comparing the output.
