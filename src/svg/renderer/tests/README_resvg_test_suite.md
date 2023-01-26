# Resvg Test Suite Instructions

`//src/svg/renderer/tests:resvg_test_suite` uses https://github.com/RazrFalcon/resvg-test-suite
to validate Donner's rendering end-to-end. The test suite provides .svg files that can be rendered
with the static subset of SVG (and some SVG2), and resvg's golden images to compare against.

To validate against this suite continuously, use https://github.com/jwmcglynn/pixelmatch-cpp17 to
perceptually difference the images and wrap it in a gtest. While execution appears to be
single-threaded, it's fast enough to be run in CI, and sufficiently fast to be run as part of inner
loop development.

To run the suite:

    $ bazel run //src/svg/renderer/tests:resvg_test_suite

Or in debug mode:

    $ bazel run -c dbg //src/svg/renderer/tests:resvg_test_suite

Since this is a gtest, it will also be run as part of any bazel test targeting this directory:

    $ bazel test //...

To run as part of gtest, a parameter-driven test is automatically created by grabbing scanning the
directory, wildcard-matching the filenames, and then generating a unique test for each filename
being tested.

Some tests require more lenient matching, or must be skipped entirely due to incomplete Donner
functionality.  To do this per-test params may be specified.  Combined together, a test
registration appears as:

    INSTANTIATE_TEST_SUITE_P(
        Transform, ImageComparisonTestFixture,
        ValuesIn(getTestsWithPrefix(
            "a-transform",
            {
                {"a-transform-007.svg",
                Params::WithThreshold(0.05f)},  // Larger threshold due to anti-aliasing artifacts.
            })),
        TestNameFromFilename);

The Resvg test suite files all start with a letter, either "a-" for attribute, or "e-" for element,
followed by a dash-delimited name, and then a zero-prefixed number.  So "a-transform" will match
all tests like "a-transform-007.svg" or "a-transform-origin-001.svg" (hypothetically).

The test name is generated based on the test suite name, "Transform" above and the sanitized
filename.  For the above example, an example test would be named:

    Transform/ImageComparisonTestFixture.ResvgTest/a_transform_001

To run a test with only one test:

    $ bazel run -c dbg //src/svg/renderer/tests:resvg_test_suite -- --gtest_filter="*a_transform_001"

If a test is skipped, it's still useful to manually run it without code changes. With gtest, the
quickest way to enable that is to automatically prefix the test names with `DISABLED_`. Prefixing
this has special meaning in gtest, where the test is automatically marked disabled, but it can
still be run manually from the command line.

To run a test that has been disabled, invoke it the same way:

    $ bazel run -c dbg //src/svg/renderer/tests:resvg_test_suite -- --gtest_filter="*a_transform_008" --gtest_also_run_disabled_tests

With suffix-matching, the same test identifier can be used.
