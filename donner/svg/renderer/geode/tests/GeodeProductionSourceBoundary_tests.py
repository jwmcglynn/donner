"""Production Geode sources stay independent of the test-only wgpu-native reference.

The Linux resvg comparison renders the production Geode context through a test-only wgpu-native
reference, adopted as an external runtime device source. The production sources reach every
backend through the GPU runtime contract alone, so none of them may name the reference's types,
its build macro, the retired transitional backend, or a WebGPU C or C++ API.

The seam that adopts a runtime device source is internal to this package. `geode_device` links
its library as an implementation dependency, so the header does not reach the compile inputs of
anything that depends on `geode_device`; the public root header only forward-declares the source.
"""

import ast
from pathlib import Path
import re
import sys
import unittest


FORBIDDEN = re.compile(
    r"GeodeWgpuAdapterDevice|GeodeWgpuUtil|GeodeCallbackState|WgpuReference"
    r"|DONNER_GEODE_WGPU_REFERENCE|TransitionalWgpu|transitionalAdapter|adapterDevice"
    r"|webgpu/webgpu|webgpu\.hpp|\bwgpu::|\bWGPU[A-Z][A-Za-z]*|\bwgpu[A-Z][A-Za-z]*"
)

# The reference's own sources, which the BUILD file excludes from the scanned set by name.
REFERENCE_SOURCES = {
    "GeodeCallbackState.h",
    "GeodeWgpuAdapterDevice.cc",
    "GeodeWgpuAdapterDevice.h",
    "GeodeWgpuUtil.h",
}

# Production sources the scan must see, so a broken file set cannot pass vacuously.
REQUIRED_SOURCES = {
    "GeodeBrowserRoot.cc",
    "GeodeBrowserRoot.h",
    "GeodeDevice.cc",
    "GeodeDevice.h",
    "GeodeNativeRoot.cc",
    "GeodeNativeRoot.h",
    "GeodeRuntimeDeviceSource.h",
}

SEAM_LIBRARY = ":geode_runtime_device_source"
SEAM_HEADER = "GeodeRuntimeDeviceSource.h"
# The one production file that may include the seam header: the native root that implements it.
SEAM_INCLUDERS = {"GeodeNativeRoot.cc"}


def _arguments():
    build = next(arg.split("=", 1)[1] for arg in sys.argv[1:] if arg.startswith("--build="))
    sources = [Path(arg) for arg in sys.argv[1:] if not arg.startswith("--build=")]
    return Path(build), sources


def _findings(paths):
    for path in paths:
        for number, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
            for match in FORBIDDEN.finditer(line):
                yield f"{path.name}:{number}: {match.group(0)}"


def _rule(build, name):
    for node in ast.walk(ast.parse(build.read_text(encoding="utf-8"))):
        if isinstance(node, ast.Call) and any(
            keyword.arg == "name" and isinstance(keyword.value, ast.Constant)
            and keyword.value.value == name for keyword in node.keywords
        ):
            return node
    raise AssertionError(f"BUILD.bazel defines no rule named {name}")


def _labels(rule, attribute):
    value = next((keyword.value for keyword in rule.keywords if keyword.arg == attribute), None)
    if value is None:
        return set()
    return {node.value for node in ast.walk(value)
            if isinstance(node, ast.Constant) and isinstance(node.value, str)}


class GeodeProductionSourceBoundaryTest(unittest.TestCase):
    def test_scan_covers_the_production_sources_and_excludes_only_the_reference(self):
        names = {path.name for path in _arguments()[1]}
        self.assertLessEqual(REQUIRED_SOURCES, names, "the scanned set lost a production source")
        self.assertFalse(REFERENCE_SOURCES & names, "the scan includes the reference's own files")

    def test_production_sources_name_no_wgpu_reference(self):
        findings = list(_findings(_arguments()[1]))
        self.assertEqual(findings, [], "production Geode sources name the wgpu reference")

    def test_seam_header_reaches_no_dependent_of_the_production_device(self):
        build, sources = _arguments()
        device = _rule(build, "geode_device")
        self.assertNotIn(SEAM_LIBRARY, _labels(device, "deps"),
                         "geode_device would export the seam header to every dependent")
        self.assertIn(SEAM_LIBRARY, _labels(device, "implementation_deps"),
                      "geode_device must link the seam privately")
        self.assertEqual(_labels(_rule(build, "geode_runtime_device_source"), "visibility"),
                         {"//visibility:private"})
        includers = {path.name for path in sources
                     if SEAM_HEADER in path.read_text(encoding="utf-8")}
        self.assertEqual(includers, SEAM_INCLUDERS,
                         "a production source other than the native root names the seam")


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
