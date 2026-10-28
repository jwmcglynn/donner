"""The producer archive is test data, not permission to extract arbitrary tar members."""

import ast
import io
import os
from pathlib import Path, PurePosixPath
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock

from python.runfiles import runfiles
from tools import macos_editor_test_bundle as bundle


class NativeTestBundleTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.archive = self.root / "producer.tar"
        self.destination = self.root / "Donner.app"

    def members(self):
        files = sorted(name.split("/", 1)[1]
                       for name in bundle.native.REQUIRED - {"LICENSE", "NOTICE.txt"})
        directories = sorted({str(parent) for name in files
                              for parent in PurePosixPath(name).parents if str(parent) != "."},
                             key=lambda name: (name.count("/"), name))
        result = []
        for name in directories:
            member = tarfile.TarInfo(name)
            member.type, member.mode = tarfile.DIRTYPE, 0o755
            result.append((member, b""))
        for name in files:
            member = tarfile.TarInfo(name)
            member.mode = 0o755 if "/MacOS/" in name else 0o644
            result.append((member, b"signed producer bytes"))
        return result

    def write(self, members):
        with tarfile.open(self.archive, "w", format=tarfile.USTAR_FORMAT) as output:
            for member, data in members:
                member.uid = member.gid = 0
                member.uname, member.gname = "root", "wheel"
                member.size = len(data)
                output.addfile(member, io.BytesIO(data))

    def resource_staging_command(self):
        location = runfiles.Create().Rlocation("donner/build_defs/macos_app_bundle.bzl")
        self.assertIsNotNone(location)
        tree = ast.parse(Path(location).read_text())
        actions = [node for node in ast.walk(tree) if isinstance(node, ast.Call)
                   and isinstance(node.func, ast.Attribute) and node.func.attr == "run_shell"]
        self.assertEqual(len(actions), 1)
        commands = [keyword.value for keyword in actions[0].keywords if keyword.arg == "command"]
        self.assertEqual(len(commands), 1)
        command = ast.literal_eval(commands[0])
        start = 'mkdir -p "$app/Contents/Resources/donner-assets"'
        stop = 'chmod 644 "$app/Contents/Info.plist" "$app/Contents/Resources/Donner.icns"'
        self.assertEqual(command.count(start), 1)
        self.assertEqual(command.count(stop), 1)
        return command[command.index(start):command.index(stop)]

    def check_resource_staging(self, virtual_inputs):
        canonical, virtual = self.root / "canonical", self.root / "virtual-inputs"
        payloads = {f"assets/icon-{index}.svg": f"<svg id=\"{index}\"/>".encode()
                    for index in range(56)}
        payloads["manifest.json"] = b'{"assets": []}'
        for name, data in payloads.items():
            source, linked = canonical / name, virtual / name
            source.parent.mkdir(parents=True, exist_ok=True)
            linked.parent.mkdir(parents=True, exist_ok=True)
            source.write_bytes(data)
            source.chmod(0o444)
            linked.symlink_to(source)
        input_tree = virtual if virtual_inputs else canonical
        environment = dict(os.environ, app=str(self.destination), resources=str(input_tree))
        subprocess.run(["/bin/sh", "-eu", "-c", self.resource_staging_command()],
                       env=environment, check=True, capture_output=True)
        staged = self.destination / "Contents/Resources/donner-assets"
        for name, data in payloads.items():
            output, source = staged / name, canonical / name
            self.assertFalse(output.is_symlink(), name)
            self.assertEqual(output.read_bytes(), data, name)
            self.assertEqual(output.stat().st_mode & 0o777, 0o644, name)
            self.assertEqual(source.read_bytes(), data, name)
            self.assertEqual(source.stat().st_mode & 0o777, 0o444, name)
        for directory in (staged, staged / "assets", canonical, canonical / "assets"):
            self.assertEqual(directory.stat().st_mode & 0o777, 0o755)

    def test_production_resource_staging_copies_regular_input_bytes(self):
        self.check_resource_staging(virtual_inputs=False)

    def test_production_resource_staging_materializes_virtual_input_bytes(self):
        self.check_resource_staging(virtual_inputs=True)

    def test_exact_bytes_and_executable_modes_survive_extraction(self):
        members = self.members()
        self.write(members)
        bundle.extract_bundle(self.archive, self.destination)
        for member, data in members:
            path = self.destination / member.name
            self.assertFalse(path.is_symlink())
            self.assertEqual(path.stat().st_mode & 0o777, member.mode)
            if member.isfile():
                self.assertEqual(path.read_bytes(), data)

    def test_restrictive_umask_cannot_change_validated_modes(self):
        members = self.members()
        self.write(members)
        previous = os.umask(0o077)
        try:
            bundle.extract_bundle(self.archive, self.destination)
        finally:
            os.umask(previous)
        self.assertEqual(self.destination.stat().st_mode & 0o777, 0o755)
        for member, _ in members:
            self.assertEqual((self.destination / member.name).stat().st_mode & 0o777, member.mode)

    def test_missing_resource_parent_is_rejected_before_extraction(self):
        parent = str(PurePosixPath(bundle.native.RESOURCE_MANIFEST.split("/", 1)[1]).parent)
        self.write([(member, data) for member, data in self.members() if member.name != parent])
        with self.assertRaisesRegex(ValueError, "missing or conflicting parent"):
            bundle.extract_bundle(self.archive, self.destination)
        self.assertFalse(self.destination.exists())

    def test_links_special_members_traversal_and_duplicates_are_refused_before_extraction(self):
        for kind in (tarfile.SYMTYPE, tarfile.LNKTYPE, tarfile.FIFOTYPE, tarfile.CHRTYPE):
            member = tarfile.TarInfo("Contents/Resources/link")
            member.type, member.linkname = kind, "outside"
            self.write(self.members() + [(member, b"")])
            with self.assertRaises(ValueError):
                bundle.extract_bundle(self.archive, self.destination)
            self.assertFalse(self.destination.exists())
        for name in ("../escape", "/absolute", "Contents/../escape", "Contents/Info.plist",
                     "Contents/info.plist"):
            self.write(self.members() + [(tarfile.TarInfo(name), b"extra")])
            with self.assertRaises(ValueError):
                bundle.extract_bundle(self.archive, self.destination)
            self.assertFalse(self.destination.exists())

    def test_bounds_corrupt_headers_and_special_modes_are_refused(self):
        self.write(self.members())
        with mock.patch.object(bundle.native, "MAX_FILES", 1), self.assertRaises(ValueError):
            bundle.extract_bundle(self.archive, self.destination)
        with mock.patch.object(bundle.native, "MAX_TOTAL_BYTES", 1), self.assertRaises(ValueError):
            bundle.extract_bundle(self.archive, self.destination)
        with mock.patch.object(bundle.native, "MAX_MEMBER_BYTES", 1), self.assertRaises(ValueError):
            bundle.extract_bundle(self.archive, self.destination)
        contents = bytearray(self.archive.read_bytes())
        contents[0] ^= 1
        self.archive.write_bytes(contents)
        with self.assertRaises(ValueError):
            bundle.extract_bundle(self.archive, self.destination)
        for mode in (0o4755, 0o555):
            members = self.members()
            members[-1][0].mode = mode
            self.write(members)
            with self.assertRaises(ValueError):
                bundle.extract_bundle(self.archive, self.destination)
        self.assertFalse(self.destination.exists())

    def test_extended_headers_truncated_payload_and_nonfresh_destination_are_refused(self):
        member = tarfile.TarInfo("Contents/extended")
        member.type = tarfile.XHDTYPE
        self.write(self.members() + [(member, b"PAX payload")])
        with self.assertRaises(ValueError):
            bundle.extract_bundle(self.archive, self.destination)
        self.write(self.members())
        self.archive.write_bytes(self.archive.read_bytes()[:-1])
        with self.assertRaises(ValueError):
            bundle.extract_bundle(self.archive, self.destination)
        self.write(self.members())
        self.destination.mkdir()
        sentinel = self.destination / "existing"
        sentinel.write_bytes(b"keep")
        with self.assertRaises(ValueError):
            bundle.extract_bundle(self.archive, self.destination)
        self.assertEqual(sentinel.read_bytes(), b"keep")


if __name__ == "__main__":
    unittest.main()
