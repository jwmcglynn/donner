#!/usr/bin/env python3
"""Integration tests for showcase output-path safety and packaged fonts."""

from __future__ import annotations

import json
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
        timeout=30,
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


class RelocatedShowcaseAssetCliTest(unittest.TestCase):
  def setUp(self) -> None:
    self.temporary = tempfile.TemporaryDirectory(dir=os.environ.get("TEST_TMPDIR"))
    self.addCleanup(self.temporary.cleanup)
    self.root = Path(self.temporary.name)
    self.install = self.root / "install"
    self.install.mkdir()
    self.cwd = self.root / "unrelated-cwd"
    self.cwd.mkdir()
    self.home = self.root / "empty-home"
    self.home.mkdir()
    self.generator = self.install / "generate_showcase_asset_impl"
    binary = runfile("_donner_transitioned/donner/editor/tools/generate_showcase_asset").resolve()
    shutil.copy2(binary, self.generator)
    self.assets = runfile("donner/editor/tools/generate_showcase_asset_impl.assets")
    self.input = self.cwd / "input.svg"
    shutil.copyfile(runfile("donner_splash.svg"), self.input)
    self.output = self.cwd / "output.svg"

  def install_assets(self, name: str = "generate_showcase_asset_impl.assets",
                     parent: Path | None = None) -> Path:
    destination = (parent or self.install) / name
    shutil.copytree(self.assets, destination, copy_function=shutil.copyfile)
    return destination

  def invoke(self) -> subprocess.CompletedProcess[str]:
    env = dict(os.environ)
    for name in ("BUILD_WORKING_DIRECTORY", "RUNFILES_DIR", "RUNFILES_MANIFEST_FILE",
                 "RUNFILES_MANIFEST_ONLY", "TEST_SRCDIR"):
      env.pop(name, None)
    env["HOME"] = str(self.home)
    return subprocess.run([str(self.generator), str(self.input), str(self.output)],
                          cwd=self.cwd, env=env, capture_output=True, text=True,
                          check=False, timeout=30)

  def assert_preflight_rejected(self) -> None:
    result = self.invoke()
    self.assertEqual(result.returncode, 1, result.stderr)
    self.assertIn("packaged resources unavailable:", result.stderr)
    self.assertFalse(self.output.exists())

  def test_offline_relocated_generator_outlines_text(self) -> None:
    self.install_assets()
    result = self.invoke()
    self.assertEqual(result.returncode, 0, result.stderr)
    output = self.output.read_text(encoding="utf-8")
    self.assertIn('id="showcase_svg_label_outlines"', output)
    self.assertIn('id="donner-editor-overlay"', output)

  def test_missing_companion_cannot_use_working_directory_assets(self) -> None:
    self.install_assets(name="donner-assets", parent=self.cwd)
    self.assert_preflight_rejected()

  def test_corrupt_named_payload_cannot_use_valid_generic_companion(self) -> None:
    named = self.install_assets()
    self.install_assets(name="donner-assets")
    manifest = json.loads((named / "manifest.json").read_text(encoding="utf-8"))
    payload = named / manifest["assets"][0]["path"]
    body = bytearray(payload.read_bytes())
    body[0] ^= 1
    payload.write_bytes(body)
    self.assert_preflight_rejected()

  def test_corrupt_named_manifest_cannot_use_valid_generic_companion(self) -> None:
    named = self.install_assets()
    self.install_assets(name="donner-assets")
    (named / "manifest.json").write_text("{}\n", encoding="utf-8")
    self.assert_preflight_rejected()


if __name__ == "__main__":
  unittest.main()
