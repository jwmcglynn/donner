# Resvg Test Suite Instructions {#ReSvgTestSuite}

`//donner/svg/renderer/tests:resvg_test_suite` uses https://github.com/RazrFalcon/resvg-test-suite
to validate Donner's rendering end-to-end. The test suite now ships its SVG and PNG files inside a
`tests/` directory (with assets under `resources/` and fonts under `fonts/`), and the Bazel target
bundles those directories so they can be used as runfiles.

To validate against this suite continuously, https://github.com/jwmcglynn/pixelmatch-cpp17 is used
to perceptually difference the images and wrap it in a gtest. Execution is single-threaded but it's
fast enough to be run in CI, and sufficiently fast to be run as part of inner loop development.

To run the suite:

```
bazel run //donner/svg/renderer/tests:resvg_test_suite
```

Or in debug mode:

```
bazel run -c dbg //donner/svg/renderer/tests:resvg_test_suite
```

Since this is a gtest, it will also be run as part of any bazel test targeting this directory:

```
bazel test //...
```

The parameter-driven test scans a curated list of directories under `tests/` and instantiates a
test for every `.svg` file it finds. Test names are derived from the relative path inside
`tests/` and sanitized so that repeated file names in different directories remain unique. Every
test also uses a 500x500 canvas size by default to mirror the original suite expectations.

The harness also supports per-test configuration to allow more lenient matching or to skip tests
that exercise unsupported features. Each override is keyed by the relative path under `tests/`
and can set thresholds, mark tests as skipped, or point at alternate goldens. For example, the
resvg layout now contains `painting/stroke-dasharray/em-units.svg` (a case Donner does not yet
support), which is mapped to `ImageComparisonParams::Skip()` in
`resvg_test_suite.cc`. To lower the acceptable pixel count for a noisy test instead, use
`ImageComparisonParams::WithThreshold` with the desired values.

Extending the suite means updating the directory lists and overrides in
`donner/svg/renderer/tests/resvg_test_suite.cc`. A typical override block looks like:

```cpp
const TestDirectoryConfig paintServerConfig{
    .relativePaths = {
        "paint-servers/pattern",
    },
    .overrides = {
        {"paint-servers/pattern/tiny-pattern-upscaled.svg",
         ImageComparisonParams::WithThreshold(0.2f)},  // Anti-aliasing artifacts
    },
    .defaultParams = ImageComparisonParams::WithThreshold(0.02f),
};
```

With the new layout, a test name is generated from the sanitized relative path. For the above
example, the test would be named:

`Resvg/ImageComparisonTestFixture.ResvgTest/paint_servers_pattern_tiny_pattern_upscaled_svg`

To run a test with only one test:

```sh
bazel run -c dbg //donner/svg/renderer/tests:resvg_test_suite -- --gtest_filter="*a_transform_001"
```

If a test is skipped, it's still useful to manually run it without code changes. With gtest, the
quickest way to enable that is to automatically prefix the test names with `DISABLED_`. Prefixing
this has special meaning in gtest, where the test is automatically marked disabled, but it can
still be run manually from the command line.

To run a test that has been disabled, invoke it the same way:

```sh
bazel run -c dbg //donner/svg/renderer/tests:resvg_test_suite -- --gtest_filter="*a_transform_008" --gtest_also_run_disabled_tests
```

With suffix-matching, the same test identifier can be used.
