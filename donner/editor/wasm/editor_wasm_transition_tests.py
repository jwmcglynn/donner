import pathlib
import sys
import unittest


class EditorWasmTransitionTest(unittest.TestCase):
    def test_selects_complete_geode_wasm_configuration(self) -> None:
        actual = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
        expected = """\
compilation_mode=opt
copt_pthread=True
copt_oz=True
cxxopt_constexpr=True
disable_perf_opt_transition=True
editor_wasm_enabled=True
geode_enabled=True
linkopt_pthread=True
linkopt_oz=True
renderer_backend=geode
renderer_wasm_enabled=True
text=False
text_full=False
"""
        self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
