"""Resolved macro-metadata parity test for the Geode geometry fuzzer lanes."""

from pathlib import Path
import sys
import unittest


CORPUS = "geode_svg_geometry_budget_fuzzer"
BINARY = CORPUS + "_bin"
SOAK = CORPUS + "_soak"


def _routing(path):
    result = {}
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        target, tag = line.split("\t")
        result.setdefault(target, set()).add(tag)
    return result


class GeodeGeometryFuzzerRoutingTest(unittest.TestCase):
    def test_asan_and_ubsan_select_the_exact_corpus_target(self):
        routing = _routing(sys.argv[1])
        self.assertEqual(set(routing), {CORPUS, BINARY, SOAK})
        self.assertIn("fuzz_geode", routing[CORPUS])
        self.assertIn("fuzz_ubsan_geode", routing[CORPUS])
        self.assertNotIn("fuzz_ubsan_geode", routing[BINARY])
        self.assertNotIn("fuzz_ubsan_geode", routing[SOAK])

    def test_gpu_opening_targets_are_serialized(self):
        routing = _routing(sys.argv[1])
        self.assertTrue(all("exclusive-if-local" in tags for tags in routing.values()))

    def test_boundary_oracle_skips_when_no_device_is_available(self):
        source = Path(sys.argv[2]).read_text(encoding="utf-8")
        self.assertIn("GeodeDevice::CreateHeadless()", source)
        self.assertIn("if (!device)", source)

    def test_device_creation_is_not_charged_to_a_per_input_timeout(self):
        # The soak lanes pass -timeout=2. libFuzzer arms that timeout inside the input callback,
        # so a device created there (it takes seconds on a loaded software-Vulkan runner) trips
        # the timeout on the empty-input smoke test before any input is actually slow. Creation
        # belongs in LLVMFuzzerInitialize, which runs before the timer is armed.
        source = Path(sys.argv[2]).read_text(encoding="utf-8")
        self.assertIn(
            "LLVMFuzzerInitialize",
            source,
            "the harness must create its device outside the per-input timeout",
        )
        initialize = source.index("LLVMFuzzerInitialize")
        timed = source.index("LLVMFuzzerTestOneInput")
        self.assertLess(initialize, timed)
        self.assertIn("CreateHeadless", source[initialize:timed])
        self.assertNotIn("CreateHeadless", source[timed:])


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
