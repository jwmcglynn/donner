#!/usr/bin/env python3
"""Integration tests for showcase-generator output-path safety."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def runfile(path: str) -> Path:
  """Resolve a Bazel runfile for local and sandboxed test execution."""
  workspace = os.environ.get("TEST_WORKSPACE", "_main")
  candidates: list[Path] = []
  for env_name in ("RUNFILES_DIR", "TEST_SRCDIR"):
    if root := os.environ.get(env_name):
      candidates.append(Path(root) / workspace / path)
      candidates.append(Path(root) / path)

  for candidate in candidates:
    if candidate.exists():
      return candidate
  raise FileNotFoundError(path)


class GenerateShowcaseAssetCliTest(unittest.TestCase):
  @classmethod
  def setUpClass(cls) -> None:
    cls.generator = runfile("donner/editor/tools/generate_showcase_asset")
    cls.canonical = runfile("donner_splash.svg")

  def invoke(self, input_path: Path, output_path: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(self.generator), str(input_path), str(output_path)],
        check=False,
        capture_output=True,
        text=True,
    )

  def assert_alias_rejected(self, alias_kind: str) -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
      input_path = Path(temp_dir) / "canonical.svg"
      shutil.copyfile(self.canonical, input_path)
      original = input_path.read_bytes()

      if alias_kind == "same":
        output_path = input_path
      else:
        output_path = Path(temp_dir) / "output.svg"
        if alias_kind == "symlink":
          output_path.symlink_to(input_path)
        elif alias_kind == "hardlink":
          os.link(input_path, output_path)
        else:
          self.fail(f"unknown alias kind: {alias_kind}")

      result = self.invoke(input_path, output_path)
      self.assertNotEqual(result.returncode, 0, result.stdout)
      self.assertIn("input and output must be different files", result.stderr)
      self.assertEqual(input_path.read_bytes(), original)

  def test_rejects_same_path(self) -> None:
    self.assert_alias_rejected("same")

  def test_rejects_symlink_alias(self) -> None:
    self.assert_alias_rejected("symlink")

  def test_rejects_hardlink_alias(self) -> None:
    self.assert_alias_rejected("hardlink")

  def test_writes_distinct_output(self) -> None:
    with tempfile.TemporaryDirectory() as temp_dir:
      output_path = Path(temp_dir) / "showcase.svg"
      result = self.invoke(self.canonical, output_path)
      self.assertEqual(result.returncode, 0, result.stderr)
      output = output_path.read_text(encoding="utf-8")
      self.assertIn('id="showcase_svg_label_outlines"', output)
      self.assertIn('id="donner-editor-overlay"', output)


if __name__ == "__main__":
  unittest.main()
