"""The producer archive is test data, not permission to extract arbitrary tar members."""

import io
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest import mock

from tools import macos_editor_test_bundle as bundle


class NativeTestBundleTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.archive = self.root / "producer.tar"
        self.destination = self.root / "Donner.app"

    def members(self):
        directories = ("Contents", "Contents/MacOS", "Contents/Resources", "Contents/_CodeSignature")
        result = []
        for name in directories:
            member = tarfile.TarInfo(name)
            member.type, member.mode = tarfile.DIRTYPE, 0o755
            result.append((member, b""))
        for name in sorted(bundle.native.REQUIRED - {"LICENSE", "NOTICE.txt"}):
            member = tarfile.TarInfo(name.split("/", 1)[1])
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
        members = self.members()
        members[-1][0].mode = 0o4755
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
