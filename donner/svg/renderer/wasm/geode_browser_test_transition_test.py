"""Assert the actual private renderer fixture configuration after its Bazel transition."""

import json
from pathlib import Path
import sys
import unittest


class GeodeBrowserTransitionTest(unittest.TestCase):
    def test_selects_basic_text_independently_of_the_ambient_tier(self):
        self.assertEqual(json.loads(Path(sys.argv[1]).read_text()), {
            "renderer_backend": "geode", "text": True, "text_full": False,
            "geode_enabled": True, "browser_backend": True, "renderer_wasm_enabled": True,
            "compilation_mode": "opt",
        })


if __name__ == "__main__":
    unittest.main(argv=[sys.argv[0]])
