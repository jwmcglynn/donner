import pathlib
import sys
import unittest


class EditorWasmTransitionTest(unittest.TestCase):
    def test_selects_complete_geode_wasm_configuration(self) -> None:
        actual = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
        expected = """\
compilation_mode=opt
copt_pthread=True
copt_oz=False
cxxopt_constexpr=True
disable_perf_opt_transition=True
editor_wasm_enabled=True
geode_enabled=True
linkopt_pthread=True
linkopt_oz=False
renderer_backend=geode
renderer_wasm_enabled=True
text=True
text_full=False
"""
        self.assertEqual(actual, expected)

    def test_preserves_browser_contracts_and_reserves_a_proxy_worker(self) -> None:
        actual = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
        expected = """\
asyncify_common=False
asyncify_geode=True
closure=True
closure_simple=True
exports_ccall=False
initial_memory_64_common=False
initial_memory_64_geode=True
pthread_pool_size_one=False
pthread_pool_size_two=True
"""
        self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
