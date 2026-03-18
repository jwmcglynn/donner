# Resvg Test Suite Golden Image Bugs

Known cases where resvg's golden images differ from the correct rendering per the SVG/CSS spec.
When a resvg golden is incorrect, we generate Donner's own golden and use `Params::WithGoldenOverride()`
to compare against it instead.

Custom goldens are stored in `donner/svg/renderer/testdata/golden/resvg-<test-name>.png`.

## a-filter-013: Drop-shadow blur on circle (mm units)

**SVG:** Circle with `filter: drop-shadow(5mm 5mm 1mm)`.

**Symptom:** ~9100 pixel diff between Donner and resvg golden at default threshold.

**Root cause:** Blur algorithm difference. Donner uses a three-pass box blur approximation of
Gaussian blur (per SVG spec recommendation). Resvg uses a different blur implementation that
produces a visually different halo shape around the shadow. The difference is concentrated at
the edges of the blur radius where the two approximation methods diverge.

**Ruled out:** Color space (sRGB vs linearRGB) was tested and made no difference to this test —
switching to linearRGB changed the diff by <30 pixels.

**Resolution:** Custom golden generated from Donner's output. Donner's rendering is correct
per CSS Filter Effects Level 1 §8 (sRGB color space) and SVG §15.9 (Gaussian blur approximation
via three-pass box filter).

## a-filter-015: Drop-shadow blur on circle (em units)

**SVG:** Circle with `filter: drop-shadow(1em 1em 0.2em)`.

**Symptom:** ~14600 pixel diff between Donner and resvg golden at default threshold.

**Root cause:** Same blur algorithm difference as a-filter-013. The larger blur radius (0.2em
at the test's font size) amplifies the halo shape difference between the two blur implementations.

**Ruled out:** Unit resolution was verified correct — em units resolve against the element's
computed font-size, and `Lengthd::toPixels()` produces the same pixel values as manual
calculation. Color space switching had no effect.

**Resolution:** Custom golden generated from Donner's output.

## e-marker-045: Marker direction at beginning/end of closed shapes

**Symptom:** Disagreement about direction to place markers at the beginning/end of closed paths.

**Resolution:** Custom golden generated from Donner's output.

## e-marker-051: Marker direction on cusp

**Symptom:** Disagreement about marker direction on cusp points.

**Resolution:** Custom golden generated from Donner's output.
