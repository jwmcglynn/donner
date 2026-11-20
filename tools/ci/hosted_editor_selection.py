"""Select a hosted Editor Wasm PR graph from the complete affected rule set."""

import os
import sys
from pathlib import Path

from editor_wasm_evidence import HOSTED, selected_browsers

PACKAGE_ROOTS = {"//tools/ci:hosted_editor_wasm_package", "//tools/ci:editor_wasm_package"}


def select(affected, roots):
    if not (HOSTED | PACKAGE_ROOTS) <= roots:
        return "full", ()
    relevant = affected & roots
    if not relevant:
        return "unaffected", ()
    if relevant <= HOSTED:
        return "browser_subset", tuple(sorted(relevant))
    return "full", ()


def main():
    if sys.argv[1:] == ["--validate-browser-targets"]:
        selected_browsers(os.environ["HOSTED_EDITOR_BROWSER_TARGETS"])
        return

    affected_path, roots_path = map(Path, sys.argv[1:])
    mode, targets = select(set(affected_path.read_text().splitlines()),
                           set(roots_path.read_text().splitlines()))
    print(f"hosted_editor_mode={mode}")
    if targets:
        print("hosted_editor_browser_targets=" + " ".join(targets))


if __name__ == "__main__":
    main()
