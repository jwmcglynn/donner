# Text path reference images

The two references below are generated independently with resvg 0.47.0, using the
unchanged vendored SVG inputs and only the corpus `NotoSans-Regular.ttf` font.
They replace references that predate the current relative-position layout. Donner
output is never used to generate these references.

Font SHA-256: `6b04c8dd65af6b73eb4279472ed1580b29102d6496a377340e80a40cdb3b22c9`.

| Case | SVG SHA-256 | PNG SHA-256 |
| --- | --- | --- |
| `tspan-with-relative-position` | `c751ba99bdc9e453507029cc0b3333b44c161542e821913009813525061c2670` | `fda4f9409e5b1cd52b3368157eca7ff5af20b438c68eb82b0e263b0cb33b8e83` |
| `dy-with-tiny-coordinates` | `396e4edd7ad7bde78ab0b64b1b06328853b78860065be0d3aac58096b4a7f4a0` | `518e1ba79b2d93a906aecf1d109fdb3362794b36fc2b7ac4df28cc4b80fa26a4` |

Run these commands from the repository root:

```sh
resvg --skip-system-fonts --use-font-file third_party/resvg-test-suite/fonts/NotoSans-Regular.ttf --width 500 third_party/resvg-test-suite/tests/text/textPath/tspan-with-relative-position.svg donner/svg/renderer/testdata/golden/resvg-tspan-with-relative-position.png
resvg --skip-system-fonts --use-font-file third_party/resvg-test-suite/fonts/NotoSans-Regular.ttf --width 500 third_party/resvg-test-suite/tests/text/textPath/dy-with-tiny-coordinates.svg donner/svg/renderer/testdata/golden/resvg-dy-with-tiny-coordinates.png
```
