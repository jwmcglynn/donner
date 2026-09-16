# Pixelmatch 2.0 migration {#Pixelmatch2Migration}

This note records the pixelmatch-cpp17 1.0.3 to 2.0.0 migration, landed through
PR #1285 (a Renovate major bump plus the recalibration below). The dependency
resolves to 2.0.0 from the Bazel Central Registry; the CMake FetchContent pin
tracks tag `v2.0.0` (commit `25b82299cbedc24b61af3f440c83f9b64cf18ce9`).

## What changed in the comparator

- Transparency blends against a checkerboard by default instead of white, and
  stays at full precision instead of being quantized back to uint8.
- The color metric changed from YIQ to toe-corrected OKLab HyAB.
- Anti-alias detection now uses fixed-white brightness comparisons.

No renderer, golden, or skip-list change is part of this migration. Every
failure below is a comparator-sensitivity exposure in a previously passing
test, proven by running the exact 1.0.3 and 2.0.0 sources against the test
artifacts: all listed cases count zero under 1.0.3 and fail only under 2.0.0.

## Decision

Keep the upstream checkerboard defaults in Donner's comparison helpers and
recalibrate thresholds to measured minimal passing values at unchanged pixel
allowances (operator decision, PR #1285). White-background comparison would
restore the old transparency contract but was explicitly declined. The large
thresholds in the second table therefore accept the documented alpha deltas;
they are an explicit override of the don't-mask-failures rule for these named
cases, not a claim that the underlying filter-math and background-convention
differences are resolved.

## Modest recalibrations (17)

OKLab metric sensitivity on opaque or near-opaque content. Each value is the
minimal passing hundredth-step threshold; pixel allowances are unchanged.

| Test under `third_party/resvg-test-suite/tests/`             | Previous | Landed |            Differing pixels before → after | Allowance |
| ------------------------------------------------------------ | -------: | -----: | -----------------------------------------: | --------: |
| `filters/feDropShadow/only-stdDeviation.svg`                 |     0.04 |   0.05 |             CPU 190 → 150; Geode 183 → 156 |       160 |
| `filters/fePointLight/complex-transform.svg`                 |     0.10 |   0.11 |                                    122 → 0 |       120 |
| `paint-servers/stop/no-stop-color.svg`                       |     0.02 |   0.03 |                                    199 → 0 |       100 |
| `structure/image/embedded-jpeg-as-image-jpeg.svg`            |     0.02 |   0.04 |              CPU 2479 → 7; Geode 2730 → 19 |       100 |
| `structure/image/embedded-jpeg-as-image-jpg.svg`             |     0.02 |   0.04 |              CPU 2479 → 7; Geode 2730 → 19 |       100 |
| `structure/image/embedded-png.svg`                           |     0.02 |   0.03 |               CPU 151 → 10; Geode 201 → 13 |       100 |
| `structure/image/external-jpeg.svg`                          |     0.02 |   0.04 |              CPU 2479 → 7; Geode 2730 → 19 |       100 |
| `structure/image/external-png.svg`                           |     0.02 |   0.03 |               CPU 151 → 10; Geode 201 → 13 |       100 |
| `structure/image/preserveAspectRatio=xMaxYMax-slice.svg`     |     0.02 |   0.04 |             CPU 1850 → 10; Geode 1857 → 10 |       100 |
| `structure/image/preserveAspectRatio=xMidYMid-slice.svg`     |     0.02 |   0.03 |                                   1252 → 0 |       100 |
| `structure/image/preserveAspectRatio=xMinYMin-slice.svg`     |     0.02 |   0.04 |             CPU 1857 → 10; Geode 1870 → 10 |       100 |
| `structure/image/raster-image-and-size-with-odd-numbers.svg` |     0.02 |   0.03 |               CPU 151 → 10; Geode 201 → 13 |       100 |
| `structure/svg/preserveAspectRatio=xMinYMin.svg`             |     0.13 |   0.14 |                                    122 → 0 |       100 |
| `structure/svg/proportional-viewBox.svg`                     |     0.13 |   0.14 |                                    122 → 0 |       100 |
| `text/text-decoration/all-types-inline-comma-separated.svg`  |     0.10 |   0.12 | simple-text 102 → ≤100; full-text 103 → 68 |       100 |
| `text/text-decoration/all-types-inline-no-spaces.svg`        |     0.10 |   0.12 | simple-text 102 → ≤100; full-text 103 → 68 |       100 |
| `text/text-decoration/all-types-inline.svg`                  |     0.10 |   0.12 | simple-text 102 → ≤100; full-text 103 → 68 |       100 |

The text-decoration value is 0.12 rather than the staged 0.11: 0.11 passes
simple text at exactly the allowance but counts 103 on full text, while 0.12
counts 68 on both. The `only-stdDeviation` Geode count is byte-identical (156)
on lavapipe and Metal-backed Geode, so the 160 allowance holds across GPU
backends deterministically.

## Large-diff dispositions (24 CPU, 20 with Geode variants)

Every counted pixel in these cases was classified with the exact 2.0.0
comparator. Zero opaque-content diffs were found: the failures are
semi-transparent filter-output math deltas (convolve, specular lighting,
filter-region compositing) and transparent-vs-white background-convention
pixels (morphology erosion boundaries, text backgrounds and AA fringes).
All pass under white blending, which proves the rendered content matches and
isolates the cause to transparency handling, not content bugs. No small-fix
renderer bug was found; aligning the filter math is rework, not a follow-up
tweak. Each landed threshold below is the minimal passing hundredth verified
to fail one step lower, on both CPU and Geode artifacts where both exist.

| Test                                                                        | Threshold | CPU count at threshold | Geode count at threshold | Allowance |
| --------------------------------------------------------------------------- | --------: | ---------------------: | -----------------------: | --------: |
| `filters/feConvolveMatrix/edgeMode=none.svg`                                |      0.53 |                      0 |                        0 |       100 |
| `filters/feConvolveMatrix/edgeMode=wrap.svg`                                |      0.43 |                    194 |                      194 |       200 |
| `filters/feConvolveMatrix/order=4-2.svg`                                    |      0.48 |                      0 |                        0 |       100 |
| `filters/feConvolveMatrix/order=4-4.svg`                                    |      0.48 |                      0 |                        0 |       100 |
| `filters/feConvolveMatrix/order=4.svg`                                      |      0.48 |                      0 |                        0 |       100 |
| `filters/feConvolveMatrix/preserveAlpha=true.svg`                           |      0.16 |                     56 |                       56 |       100 |
| `filters/feConvolveMatrix/targetX=0.svg`                                    |      0.15 |                     76 |                       76 |       100 |
| `filters/feConvolveMatrix/targetX=2.svg`                                    |      0.15 |                     76 |                       76 |       100 |
| `filters/feConvolveMatrix/unset-order.svg`                                  |      0.15 |                     78 |                       78 |       100 |
| `filters/feMorphology/radius=0.5.svg`                                       |      0.89 |                      0 |                        0 |       100 |
| `filters/feMorphology/radius=1-10.svg`                                      |      0.89 |                      0 |                        0 |       100 |
| `filters/feMorphology/radius=10-1.svg`                                      |      0.89 |                      0 |                        0 |       100 |
| `filters/feSpecularLighting/with-feSpotLight-and-specular-and-exponent.svg` |      0.31 |                     63 |                       63 |       100 |
| `filters/feSpecularLighting/with-feSpotLight-and-specularConstant=5.svg`    |      0.63 |                     97 |                       97 |       100 |
| `filters/filter/everything-via-xlink-href.svg`                              |      0.18 |                      0 |                        0 |       100 |
| `filters/filter/negative-subregion.svg`                                     |      0.18 |                      0 |                        0 |       100 |
| `filters/filter/some-attributes-via-xlink-href.svg`                         |      0.18 |                      0 |                        0 |       100 |
| `filters/filter/with-region-and-subregion.svg`                              |      0.18 |                      0 |                        0 |       100 |
| `filters/filter/with-subregion-1.svg`                                       |      0.18 |                      0 |                        0 |       100 |
| `filters/filter/with-subregion-2.svg`                                       |      0.18 |                      0 |                        0 |       100 |
| `text/text-anchor/coordinates-list.svg`                                     |      0.89 |                      0 |         no Geode variant |       100 |
| `text/textPath/link-to-rect.svg`                                            |      0.89 |                     14 |         no Geode variant |       100 |
| `text/textPath/with-path-and-xlink-href.svg`                                |      0.89 |                      0 |         no Geode variant |       100 |
| `text/textPath/with-path.svg`                                               |      0.89 |                      0 |         no Geode variant |       100 |

## Identity-test allowances (5)

One-to-two-pixel 1-LSB-class diffs proven pre-existing by byte comparison of
committed goldens/oracles against fresh renders (the renderer is unchanged, so
the bytes were always like this; 1.x was structurally blind to them):

| Test                                                                           | Evidence                                                              |           Allowance |
| ------------------------------------------------------------------------------ | --------------------------------------------------------------------- | ------------------: |
| `MetalColorMatrixTest.GaussianAndBoxBlurPreservePixelsAndFoldedClip`           | 2 px, RGB identical, alpha 127 vs 128 (GPU float vs CPU oracle)       |  2 at threshold 0.0 |
| `MetalColorMatrixTest.SlugGradientClipMask`                                    | 2 px, RGB identical, alpha 127 vs 128                                 |  2 at threshold 0.0 |
| `VulkanColorMatrixTest.GaussianAndBoxBlurPreservePixelsAndFoldedClip`          | same 2 bytes as Metal (shared helper)                                 |   shared with Metal |
| `RnrReplayTest.FilterDisappearRepro3MatchesGoldenAfterSecondMouseUp`           | 2 counted of 51,665 1-LSB dark-background pixels at 0.01              | 2 at threshold 0.01 |
| `LayerThumbnailGoldenTest.DonnerSplashLayerThumbnailsMatchGoldens` (Geode arm) | 1 counted of 35 ≤3-LSB pixels at 0.02, inside documented GPU variance | 2 at threshold 0.02 |

## Validation

- `tools/cmake/gen_cmakelists_test.py`: 39/39 pass; `gen_cmakelists.py --check`
  passes (covers the CMake pin on all lanes).
- resvg default-text and full-text suites: all 41 previously failing cases pass
  locally with the landed thresholds; Geode variants verified with the exact
  comparator against CI artifacts and re-verified on Metal-backed Geode.
- `metal_color_matrix_tests` blur and slug cases pass locally with the 2px
  allowance; Vulkan shares the helper and is covered by CI.
- Full CI on PR #1285 is the final gate (Linux/macOS, CPU/Geode, CMake, lint).
