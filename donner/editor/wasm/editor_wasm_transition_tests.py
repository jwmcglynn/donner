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
text=True
text_full=False
"""
        self.assertEqual(actual, expected)

    def test_links_the_single_canvas_whole_app_worker_configuration(self) -> None:
        """The the single-canvas architecture wasm contract, asserted at the link line.

        Every value here is load-bearing:

        - ``proxy_to_pthread`` plus ``offscreencanvases_to_pthread=#canvas``
          is the whole-app-in-worker architecture. Without them ``main()``
          runs on the browser main thread and the app draws through the page,
          which is the CSS-seam architecture this design deletes.
        - ``memory_growth=0`` with ``initial_memory == maximum_memory`` is the
          fixed linear-memory invariant. Growth is fatal on all three engines,
          not slow, so a build that can grow is a build that can trap an
          unrelated thread.
        - ``pthread_pool_size=2`` pre-warms the app pthread and
          ``AsyncRenderer``'s raster ``std::thread``.
        """
        actual = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
        expected = """\
asyncify=True
closure=True
closure_simple=True
exports_ccall=False
proxy_to_pthread=True
offscreencanvases_to_pthread=#canvas
offscreencanvas_support=1
memory_growth=0
initial_memory=402653184
maximum_memory=402653184
memory_is_fixed=True
pthread_pool_size=2
"""
        self.assertEqual(actual, expected)


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
