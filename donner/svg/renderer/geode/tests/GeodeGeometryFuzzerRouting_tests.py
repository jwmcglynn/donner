"""Resolved Bazel-query parity test for the Geode geometry fuzzer lanes."""

from pathlib import Path
import sys
import unittest


EXPECTED = {"//donner/svg/renderer/geode:geode_svg_geometry_budget_fuzzer"}


def _labels(path):
    return {line for line in Path(path).read_text(encoding="utf-8").splitlines() if line}


class GeodeGeometryFuzzerRoutingTest(unittest.TestCase):
    def test_asan_and_ubsan_select_the_exact_corpus_target(self):
        asan = _labels(sys.argv[1])
        ubsan = _labels(sys.argv[2])
        self.assertEqual(asan, EXPECTED)
        self.assertEqual(ubsan, EXPECTED)
        self.assertEqual(asan, ubsan)
        self.assertFalse(any(label.endswith("_soak") for label in asan | ubsan))


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
