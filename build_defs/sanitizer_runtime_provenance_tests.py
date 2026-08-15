import os
import re
import unittest
from pathlib import Path


class SanitizerRuntimeProvenanceTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        runfiles = Path(os.environ["TEST_SRCDIR"]) / os.environ["TEST_WORKSPACE"]
        cls.rules = (runfiles / "build_defs/rules.bzl").read_text()

    def function_body(self, name):
        match = re.search(
            rf"^def {name}\(.*?(?=^def |\Z)",
            self.rules,
            re.DOTALL | re.MULTILINE,
        )
        self.assertIsNotNone(match, f"{name} definition is missing")
        return match.group(0)

    def test_ordinary_targets_do_not_search_upstream_macos_runtimes(self):
        self.assertNotIn("llvm21_macos_runtime_rpath_linkopts", self.rules)
        for macro in ("donner_cc_binary", "donner_cc_test"):
            body = self.function_body(macro)
            self.assertNotIn("rpath", body.lower())
            self.assertNotIn("toolchains_llvm", body)
            self.assertNotIn("@llvm_toolchain", body)

    def test_fuzzers_keep_explicit_upstream_runtime_inputs(self):
        linkopts = self.function_body("fuzzer_linkopts")
        linker_inputs = self.function_body("fuzzer_linker_inputs")
        self.assertIn("libclang_rt.fuzzer_osx.a", linkopts)
        self.assertIn(
            "@llvm_toolchain//:linker-components-aarch64-darwin",
            linker_inputs,
        )


if __name__ == "__main__":
    unittest.main()
