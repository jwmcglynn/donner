"""Check that the editor NOTICE attributes the full text stack.

The editor's About text embeds //third_party/licenses:notice_editor, which collects license keys
by walking the editor's C++ dependency graph. The text libraries reach HarfBuzz, FreeType,
SheenBidi and the WOFF2 decoder (with Brotli) through `implementation_deps`, so a walk that
follows only `deps` silently drops them. The test target runs in the full text configuration and
fails when any of them is missing from the manifest.
"""

from __future__ import annotations

import json
import os
from pathlib import Path
import unittest

_MANIFEST = "third_party/licenses/notice_editor.json"
_FULL_TEXT_COMPONENTS = ["brotli", "freetype", "harfbuzz", "sheenbidi", "woff2"]


def _runfile(path: str) -> Path:
    for root in (os.environ.get("RUNFILES_DIR"), os.environ.get("TEST_SRCDIR")):
        if root and (Path(root) / "_main" / path).is_file():
            return Path(root) / "_main" / path
    manifest = os.environ.get("RUNFILES_MANIFEST_FILE")
    if manifest:
        for line in Path(manifest).read_text(encoding="utf-8").splitlines():
            logical, _, physical = line.partition(" ")
            if logical == "_main/" + path:
                return Path(physical)
    raise FileNotFoundError(path)


class EditorNoticeTextTest(unittest.TestCase):
    def test_full_text_components_are_attributed(self):
        licenses = json.loads(_runfile(_MANIFEST).read_text(encoding="utf-8"))["licenses"]
        attributed = sorted(entry["label"].rpartition(":")[2] for entry in licenses)
        missing = [name for name in _FULL_TEXT_COMPONENTS if name not in attributed]
        self.assertEqual(missing, [], f"editor NOTICE attributes only {attributed}")


if __name__ == "__main__":
    unittest.main()
