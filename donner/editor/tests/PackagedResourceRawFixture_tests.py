#!/usr/bin/env python3
"""Real raw-fuzzer fixture reach and fail-closed resource initialization."""

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


def runfile(path: str) -> Path:
  root = Path(os.environ["TEST_SRCDIR"])
  candidate = root / os.environ.get("TEST_WORKSPACE", "_main") / path
  if not candidate.exists():
    raise FileNotFoundError(path)
  return candidate


def stage_public_wrapper(directory: Path) -> Path:
  wrapper = directory / Path(OPTIONS.fuzzer).name
  shutil.copyfile(runfile(OPTIONS.fuzzer), wrapper)
  wrapper.chmod(0o755)
  fixtures = Path(str(wrapper) + ".runfiles")
  logical_impl = Path("_donner_transitioned") / OPTIONS.fuzzer
  implementation = fixtures / logical_impl
  implementation.parent.mkdir(parents=True)
  shutil.copyfile(Path(os.environ["TEST_SRCDIR"]) / logical_impl, implementation)
  implementation.chmod(0o755)
  for relative in [OPTIONS.font_tree, OPTIONS.icon_tree]:
    shutil.copytree(runfile(relative), fixtures / "_main" / relative,
                    copy_function=shutil.copyfile)
  mapping = Path(os.environ["TEST_SRCDIR"]) / "_repo_mapping"
  if mapping.is_file():
    shutil.copyfile(mapping, fixtures / "_repo_mapping")
  else:
    (fixtures / "_repo_mapping").write_text("")
  return wrapper


class PackagedResourceRawFixtureTest(unittest.TestCase):
  def invoke(self, fixture_root: Path | str | None) -> subprocess.CompletedProcess[str]:
    environment = dict(os.environ)
    if fixture_root is None:
      environment.pop("DONNER_TEST_RESOURCE_ROOT", None)
      environment.pop("TEST_SRCDIR", None)
      environment["RUNFILES_DIR"] = os.environ["TEST_SRCDIR"]
    else:
      environment["DONNER_TEST_RESOURCE_ROOT"] = str(fixture_root)
    return subprocess.run(
        [str(runfile(OPTIONS.fuzzer)), "-runs=1",
         "-artifact_prefix=" + os.environ["TEST_TMPDIR"] + "/"],
        cwd=os.environ["TEST_TMPDIR"],
        env=environment,
        check=False,
        capture_output=True,
        text=True,
        timeout=30,
    )

  def test_direct_raw_fuzzer_rasterizes_fallback_glyphs_and_icon(self):
    for root in [Path(os.environ["TEST_SRCDIR"]), None]:
      with self.subTest(explicit_root=root is not None):
        result = self.invoke(root)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn("Failed to load packaged fallback", result.stderr)

  def test_relocated_public_wrapper_discovers_fixtures_with_no_runfiles_environment(self):
    with tempfile.TemporaryDirectory() as temporary:
      root = Path(temporary)
      wrapper = stage_public_wrapper(root)
      unrelated = root / "unrelated"
      unrelated.mkdir()
      findings = root / "findings"
      findings.mkdir()
      environment = {
          key: value for key, value in os.environ.items()
          if "RUNFILES" not in key and key not in (
              "TEST_SRCDIR", "DONNER_TEST_RESOURCE_ROOT", "LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH")
      }
      result = subprocess.run(
          [str(wrapper), "-runs=1", "-artifact_prefix=" + str(findings) + "/"],
          cwd=unrelated,
          env=environment,
          check=False,
          capture_output=True,
          text=True,
          timeout=30,
      )
    self.assertEqual(result.returncode, 0, result.stderr)
    self.assertNotIn("Failed to load packaged fallback", result.stderr)

  def test_missing_fixture_is_rejected_before_fuzz_input(self):
    with tempfile.TemporaryDirectory() as temporary:
      for root in [Path(temporary), ""]:
        with self.subTest(root=str(root)):
          result = self.invoke(root)
          self.assertNotEqual(result.returncode, 0)
          self.assertIn("Cannot prepare packaged test resources", result.stderr)
          self.assertNotIn("Packaged fixture probe:", result.stderr)

  def test_corrupt_fixture_is_rejected_before_fuzz_input(self):
    with tempfile.TemporaryDirectory() as temporary:
      root = Path(temporary)
      for relative in [OPTIONS.font_tree, OPTIONS.icon_tree]:
        shutil.copytree(runfile(relative), root / "_main" / relative,
                        copy_function=shutil.copyfile)
      font_tree = root / "_main" / OPTIONS.font_tree
      manifest = json.loads((font_tree / "manifest.json").read_text())
      font = font_tree / manifest["assets"][0]["path"]
      data = bytearray(font.read_bytes())
      data[0] ^= 1
      font.write_bytes(data)
      result = self.invoke(root)
    self.assertNotEqual(result.returncode, 0)
    self.assertIn("Cannot prepare packaged test resources", result.stderr)
    self.assertNotIn("Packaged fixture probe:", result.stderr)


if __name__ == "__main__":
  parser = argparse.ArgumentParser()
  parser.add_argument("--fuzzer", required=True)
  parser.add_argument("--font-tree", required=True)
  parser.add_argument("--icon-tree", required=True)
  OPTIONS, remaining = parser.parse_known_args()
  unittest.main(argv=[__file__, *remaining])
