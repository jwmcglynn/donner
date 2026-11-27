"""Native producer custody and ordering controls; no compiler, native executable or upload runs."""
from contextlib import ExitStack
from hashlib import sha256
import json
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

from tools import bcr_preflight, native_delivery, native_producer as producer

COMMIT = "a" * 40
GUARD = (
    "${{ github.repository == 'jwmcglynn/donner' && github.ref == 'refs/heads/main' && "
    "(github.event_name == 'schedule' || github.event_name == 'workflow_dispatch') }}"
)
UNWIND = {".eh_frame": "a" * 64}
MACH_UNWIND = {"__unwind_info": "b" * 64}


def digest(data):
    return sha256(data).hexdigest()


def workflow():
    try:
        from python.runfiles import runfiles
        location = runfiles.Create().Rlocation("_main/.github/workflows/bcr_preflight.yml")
        if location:
            return Path(location).read_text()
    except ImportError:
        pass
    return (Path(__file__).resolve().parents[1] / ".github/workflows/bcr_preflight.yml").read_text()


def _upload_expectations(upload, platform):
    errors = []
    expected = {
        "name": "donner-native-symbols-" + platform + "-${{ needs.prepare.outputs.source_commit }}-${{ github.run_id }}-${{ github.run_attempt }}",
        "path": "${{ runner.temp }}/donner-native-symbols/" + platform + "/",
        "if-no-files-found": "error", "retention-days": "90",
        "compression-level": "0", "overwrite": "false",
    }
    for key, value in expected.items():
        if "          " + key + ": " + value + "\n" not in upload:
            errors.append("unreviewed diagnostic upload " + key)
    if "continue-on-error:" in upload or "always()" in upload:
        errors.append("upload failure may be ignored")
    return errors


def admission_errors(text):
    errors = []
    for job, platform in (("cli-linux", "linux-x86-64"), ("cli-macos", "darwin-arm64")):
        block = text.split("\n  " + job + ":\n", 1)[1]
        block = re.split(r"\n  [a-z][a-z-]+:\n", block, maxsplit=1)[0]
        pieces = re.split(r"(?m)^      - ", block)
        uploads = [piece for piece in pieces if "name: Retain paired native diagnostics" in piece]
        if len(uploads) != 1:
            errors.append("expected exactly one curated diagnostic upload")
            continue
        upload = uploads[0]
        if "        if: " + GUARD + "\n" not in upload:
            errors.append("not trusted main only")
        if "uses: actions/upload-artifact@v7" not in upload:
            errors.append("unreviewed diagnostic transfer")
        errors.extend(_upload_expectations(upload, platform))
    after = text.split("\n  attest-artifacts:\n", 1)[1]
    if "donner-native-symbols" in after:
        errors.append("diagnostics entered candidate inventory")
    return errors


class NativeProducerPlanTest(unittest.TestCase):
    def test_native_policy_is_only_supported_main_events_in_release_repository(self):
        owner = {"GITHUB_REPOSITORY": "jwmcglynn/donner"}
        for event in ("schedule", "workflow_dispatch"):
            self.assertTrue(bcr_preflight.trusted_native_delivery({
                **owner, "GITHUB_EVENT_NAME": event, "GITHUB_REF": "refs/heads/main"}))
        for event, ref in (("pull_request", "refs/heads/main"), ("push", "refs/heads/main"),
                           ("workflow_dispatch", "refs/heads/other"), ("workflow_call", "refs/heads/main")):
            self.assertFalse(bcr_preflight.trusted_native_delivery({
                **owner, "GITHUB_EVENT_NAME": event, "GITHUB_REF": ref}))
        self.assertFalse(bcr_preflight.trusted_native_delivery({
            "GITHUB_REPOSITORY": "another/donner",
            "GITHUB_EVENT_NAME": "workflow_dispatch", "GITHUB_REF": "refs/heads/main"}))

    def test_default_pr_commands_remain_unstripped(self):
        for platform in producer.PLATFORMS:
            commands = bcr_preflight.cli_plan(bcr_preflight.Layout("/temp"), platform, COMMIT, "1", "1")
            self.assertFalse(any("tools.native_producer" in c.argv for c in commands))
            self.assertFalse(any("--copt=-gline-tables-only" in c.argv for c in commands))

    def _assert_release_debug_flags(self, command, platform):
        self.assertEqual(command[1:3], ("--nosystem_rc", "--nohome_rc"))
        self.assertIn("--copt=-Os", command)
        self.assertIn("--copt=-gline-tables-only", command)
        self.assertNotIn("--config=latest_llvm", command)
        self.assertFalse(any(flag.startswith("--xcode_version") for flag in command))
        for flag in ("--remote_cache=", "--remote_executor=", "--disk_cache=", "--remote_upload_local_results=false", "--bes_backend=", "--bes_results_url="):
            self.assertIn(flag, command)
        if platform == "linux-x86-64":
            self.assertIn("--linkopt=-Wl,--build-id=sha1", command)
        if platform == "darwin-arm64":
            self.assertIn("--output_groups=+dsyms", command)
            self.assertIn("--apple_generate_dsym", command)

    def test_native_both_lock_passes_pair_symbols_and_use_fresh_cli(self):
        for platform in producer.PLATFORMS:
            commands = bcr_preflight.cli_plan(bcr_preflight.Layout("/temp"), platform, COMMIT, "1", "1", native=True)
            builds = [c.argv for c in commands if c.argv[0] == "bazelisk" and "build" in c.argv[:4]]
            for command in builds:
                self._assert_release_debug_flags(command, platform)
            package = next(c.argv for c in commands if "tools.release_cli" in c.argv)
            self.assertEqual(package[package.index("--binary") + 1], "/temp/donner-native-delivery/" + platform + "/donner-cli")
            self.assertEqual(commands[-1].argv[4], "finalize")

    def test_darwin_editor_requests_the_native_pair_and_uses_its_symbols(self):
        commands = bcr_preflight.cli_plan(
            bcr_preflight.Layout("/temp"), "darwin-arm64", COMMIT, "1", "1", native=True,
        )
        self.assertTrue(any("//donner/editor:editor_impl" in command.argv for command in commands))
        editor = next(c.argv for c in commands if "tools.release_macos_editor" in c.argv)
        self.assertIn("--delivery-symbols", editor)
        self.assertIn("bazel-bin/donner/editor/editor_impl.dSYM", editor)

    def test_strip_failure_stops_before_packaging_or_finalization(self):
        commands = bcr_preflight.cli_plan(bcr_preflight.Layout("/temp"), "linux-x86-64", COMMIT, "1", "1", native=True)
        called = []
        def execute(command):
            called.append(command.phase)
            return 23 if command.phase == "Prepare the paired fresh CLI delivery copy" else 0
        with patch.object(bcr_preflight, "_execute", side_effect=execute):
            result = bcr_preflight.run_lane("native producer", commands, {})
        self.assertEqual(result, 23)
        self.assertNotIn("Package the CLI", called)
        self.assertNotIn("Bind the curated diagnostic inventory to final products", called)

    def test_ignored_workspace_fragments_cannot_add_an_export(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            for name in (".buildbuddy.bazelrc", ".bazelrc.user"):
                path = root / name;path.write_text("an ignored override")
                with self.assertRaisesRegex(ValueError, "override"):
                    producer.check_workspace_overrides(root)
                path.unlink()

    def test_source_rc_import_inventory_cannot_expand_silently(self):
        try:
            from python.runfiles import runfiles
            path = runfiles.Create().Rlocation("_main/.bazelrc")
        except ImportError:
            path = Path(__file__).resolve().parents[1] / ".bazelrc"
        imports = [line for line in Path(path).read_text().splitlines()
                   if line.startswith(("import ", "try-import "))]
        self.assertEqual(imports, ["try-import %workspace%/.buildbuddy.bazelrc",
                                   "try-import %workspace%/.bazelrc.user"])

    def test_workflow_has_exact_main_only_retention_and_negative_controls(self):
        text = workflow()
        self.assertEqual(admission_errors(text), [])
        for bad in (text.replace(GUARD, "${{ always() }}", 1),
                    text.replace("retention-days: 90", "retention-days: 1"),
                    text.replace("donner-native-symbols/linux-x86-64/", "donner-cli-linux/", 1),
                    text.replace("if-no-files-found: error\n          retention-days: 90",
                                      "if-no-files-found: ignore\n          retention-days: 90"),
                    text.replace("          overwrite: false", "          overwrite: true", 1)):
            self.assertTrue(admission_errors(bad))

class NativeCustodyTest(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.temp = Path(self.directory.name)
        self.temp.chmod(0o700)
        self.mocks = ExitStack()
        self.addCleanup(self.mocks.close)
        self.mocks.enter_context(patch.object(native_delivery, "validate_image"))
        self.mocks.enter_context(patch.object(native_delivery, "_build_id", return_value="1" * 40))
        self.mocks.enter_context(patch.object(native_delivery, "_uuid", return_value="11111111-1111-1111-1111-111111111111"))
        self.mocks.enter_context(patch.object(native_delivery, "_elf_unwind", return_value=UNWIND))
        self.mocks.enter_context(patch.object(native_delivery, "_mach_unwind", return_value=MACH_UNWIND))
        self.mocks.enter_context(patch.object(native_delivery, "_require_elf_source_lines"))
        self.mocks.enter_context(patch.object(native_delivery, "_require_macho_source_lines"))
        self.mocks.enter_context(patch.object(native_delivery, "_tool", return_value=""))
        self.mocks.enter_context(patch.object(producer, "_mach_code_data", return_value="c" * 64))

    def _write_archive_fixture(self, platform, product, binary):
        archive_name = producer.ARCHIVES[platform] if product == "cli" else producer.EDITOR_ARCHIVE
        output = self.temp / ("donner-cli-" + producer.PLATFORMS[platform] if product == "cli" else "donner-editor-macos")
        output.mkdir()
        archive = output / archive_name
        archive.write_bytes(b"verified product archive")
        provenance = {"source_commit": COMMIT, "source_tree": "b" * 40, "producer_run_id": "1", "producer_attempt": "1",
                      "platform": platform, "archive_sha256": digest(archive.read_bytes()),
                      "archive_size": archive.stat().st_size}
        if product == "cli":
            provenance.update(binary_sha256=digest(binary.read_bytes()), binary_size=binary.stat().st_size)
        else:
            provenance["files"] = {"Donner SVG Editor.app/Contents/MacOS/Donner":
                                   {"sha256": digest(binary.read_bytes()), "size": binary.stat().st_size}}
        (output / (archive_name + ".provenance")).write_text(json.dumps(provenance))

    def _write_pair_fixture(self, work, stage, platform, product):
        parent = stage / product
        parent.mkdir(mode=0o700)
        symbol = parent / producer._symbol_name(platform, product)
        symbol.parent.mkdir(parents=True, exist_ok=True)
        symbol.write_bytes(b"paired public source lines")
        if platform == "darwin-arm64":
            (parent / "binary.dSYM/Contents/Info.plist").write_bytes(b"<plist>shipping product</plist>")
        binary = work / ("donner-cli" if product == "cli" else "editor/Donner.app/Contents/MacOS/Donner")
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(b"fresh stripped product with final seal")
        metadata = {"schema": 1, "platform": platform,
                    "identity": "1" * 40 if platform == "linux-x86-64" else "11111111-1111-1111-1111-111111111111",
                    "source_sha256": digest(b"unstripped input"), "source_size": 100,
                    "binary_sha256": digest(binary.read_bytes()), "binary_size": binary.stat().st_size,
                    "symbol_file": producer._symbol_name(platform, product),
                    "symbol_sha256": digest(symbol.read_bytes()),
                    "unwind_sections_sha256": UNWIND if platform == "linux-x86-64" else MACH_UNWIND}
        if platform == "darwin-arm64":
            metadata.update(final_binary_sha256=digest(binary.read_bytes()),
                            final_binary_size=binary.stat().st_size, final_seal="ad-hoc", code_data_sha256="c" * 64)
        if product == "editor":
            metadata.update(binary_sha256=digest(b"prepared before app seal"), binary_size=24)
        (parent / "delivery.json").write_text(json.dumps(metadata))
        return binary

    def fixture(self, platform="linux-x86-64"):
        producer.initialize(self.temp, platform)
        work, stage = producer.roots(self.temp, platform)
        for product in (("cli", "editor") if platform == "darwin-arm64" else ("cli",)):
            binary = self._write_pair_fixture(work, stage, platform, product)
            self._write_archive_fixture(platform, product, binary)
        return work, stage

    def test_mac_cli_compares_code_before_and_after_delivery(self):
        producer.initialize(self.temp, "darwin-arm64")
        source = self.temp / "input";source.write_bytes(b"unstripped executable")
        def prepare(**kwargs):
            kwargs["output"].write_bytes(b"stripped and signed executable")
            kwargs["symbols"].mkdir(mode=0o700)
            return {"binary_sha256": digest(kwargs["output"].read_bytes())}
        with patch.object(native_delivery, "prepare", side_effect=prepare), patch.object(producer, "_mach_code_data", return_value="c" * 64) as code:
            result = producer.prepare_cli(self.temp, "darwin-arm64", source, self.temp / "matching.dSYM")
        self.assertEqual(code.call_count, 2)
        self.assertEqual(result["code_data_sha256"], "c" * 64)
        self.assertEqual(source.read_bytes(), b"unstripped executable")

    def test_mac_cli_code_change_cannot_fall_back_to_unstripped_input(self):
        producer.initialize(self.temp, "darwin-arm64")
        source = self.temp / "input";source.write_bytes(b"unstripped executable")
        def prepare(**kwargs):
            kwargs["output"].write_bytes(b"stripped executable")
            return {}
        with patch.object(native_delivery, "prepare", side_effect=prepare), patch.object(producer, "_mach_code_data", side_effect=["a" * 64, "b" * 64]):
            with self.assertRaisesRegex(ValueError, "code or data"):
                producer.prepare_cli(self.temp, "darwin-arm64", source, self.temp / "matching.dSYM")

    def test_linux_complete_inventory_passes_without_copying_raw_proof(self):
        _, stage = self.fixture()
        producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")
        producer.validate(self.temp, "linux-x86-64", COMMIT, "1", "1")
        self.assertEqual({p.relative_to(stage).as_posix() for p in stage.rglob("*") if p.is_file()},
                         {"custody.json", "cli/delivery.json", "cli/binary.debug"})

    def test_macos_complete_inventory_binds_prepared_and_final_seal(self):
        _, stage = self.fixture("darwin-arm64")
        producer.finalize(self.temp, "darwin-arm64", COMMIT, "1", "1")
        custody = json.loads((stage / "custody.json").read_text())
        self.assertNotEqual(custody["products"]["editor"]["prepared_binary_sha256"],
                            custody["products"]["editor"]["final_binary_sha256"])
        self.assertEqual(len([p for p in stage.rglob("*") if p.is_file()]), 7)

    def test_macos_cli_diagnostics_require_the_final_seal(self):
        _, stage = self.fixture("darwin-arm64")
        path = stage / "cli/delivery.json"
        metadata = json.loads(path.read_text())
        for field in producer.FINAL_FIELDS:
            del metadata[field]
        path.write_text(json.dumps(metadata))
        with self.assertRaisesRegex(ValueError, "final macOS seal"):
            producer.finalize(self.temp, "darwin-arm64", COMMIT, "1", "1")

    def test_macos_uuid_match_without_decoded_lines_refuses_custody(self):
        self.fixture("darwin-arm64")
        with patch.object(native_delivery, "_require_macho_source_lines",
                          side_effect=ValueError("no usable DWARF source lines")):
            with self.assertRaisesRegex(ValueError, "DWARF source lines"):
                producer.finalize(self.temp, "darwin-arm64", COMMIT, "1", "1")

    def test_repeated_or_linked_staging_is_refused(self):
        self.fixture()
        with self.assertRaises(ValueError):
            producer.initialize(self.temp, "linux-x86-64")
        with self.assertRaises(ValueError):
            producer.initialize(self.temp / "missing", "linux-x86-64")

    def test_missing_or_extra_raw_crash_file_is_refused(self):
        _, stage = self.fixture()
        (stage / "crash.log").write_text("not approved for diagnostic retention")
        with self.assertRaisesRegex(ValueError, "unreviewed"):
            producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")

    def test_symbols_cannot_be_replaced_by_links(self):
        _, stage = self.fixture()
        symbol = stage / "cli/binary.debug"
        symbol.unlink()
        symbol.symlink_to(stage / "cli/delivery.json")
        with self.assertRaisesRegex(ValueError, "link"):
            producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")

    def test_file_byte_limit_is_enforced_with_a_small_fixture(self):
        _, stage = self.fixture()
        (stage / "cli/binary.debug").write_bytes(b"x" * 4096)
        with patch.object(producer, "MAX_SYMBOL_BYTES", 2048):
            with self.assertRaisesRegex(ValueError, "budget"):
                producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")

    def test_private_paths_cannot_be_uploaded(self):
        _, stage = self.fixture()
        (stage / "cli/binary.debug").write_bytes(b"/" + b"home/" + b"builder/work/source.cc")
        with self.assertRaisesRegex(ValueError, "private"):
            producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")

    def test_private_path_split_across_read_chunks_is_refused(self):
        _, stage = self.fixture()
        symbol = stage / "cli/binary.debug"
        symbol.write_bytes(b"x" * 65533 + b"/" + b"home/" + b"builder/work")
        with self.assertRaisesRegex(ValueError, "private"):
            producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")

    def test_wrong_symbol_identity_or_unwind_is_refused(self):
        self.fixture()
        with patch.object(native_delivery, "_build_id", return_value="2" * 40):
            with self.assertRaisesRegex(ValueError, "identity"):
                producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")
        with patch.object(native_delivery, "_elf_unwind", return_value={}):
            with self.assertRaisesRegex(ValueError, "unwind"):
                producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")

    def test_final_executable_wrong_architecture_is_refused(self):
        self.fixture()
        with patch.object(native_delivery, "validate_image", side_effect=ValueError("wrong architecture")):
            with self.assertRaisesRegex(ValueError, "architecture"):
                producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")

    def test_empty_source_lines_cannot_qualify(self):
        self.fixture()
        with patch.object(native_delivery, "_require_elf_source_lines", side_effect=ValueError("source lines")):
            with self.assertRaisesRegex(ValueError, "source lines"):
                producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")

    def test_unstripped_or_changed_product_cannot_be_bound(self):
        work, _ = self.fixture()
        (work / "donner-cli").write_bytes(b"original unstripped executable")
        with self.assertRaisesRegex(ValueError, "executable"):
            producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")

    def test_foreign_source_run_or_attempt_cannot_be_bound(self):
        self.fixture()
        for commit, run, attempt in (("b" * 40, "1", "1"), (COMMIT, "2", "1"), (COMMIT, "1", "2")):
            with self.assertRaisesRegex(ValueError, "producer"):
                producer.finalize(self.temp, "linux-x86-64", commit, run, attempt)

    def test_modified_custody_or_missing_file_fails_revalidation(self):
        _, stage = self.fixture()
        producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")
        manifest = stage / "custody.json"
        data = json.loads(manifest.read_text());data["producer_run_id"] = "3";manifest.write_text(json.dumps(data))
        with self.assertRaisesRegex(ValueError, "changed"):
            producer.validate(self.temp, "linux-x86-64", COMMIT, "1", "1")
        (stage / "cli/binary.debug").unlink()
        with self.assertRaisesRegex(ValueError, "incomplete"):
            producer.validate(self.temp, "linux-x86-64", COMMIT, "1", "1")

    def test_unknown_metadata_fields_are_refused(self):
        _, stage = self.fixture()
        path = stage / "cli/delivery.json";m = json.loads(path.read_text());m["environment"] = "unreviewed";path.write_text(json.dumps(m))
        with self.assertRaisesRegex(ValueError, "fields"):
            producer.finalize(self.temp, "linux-x86-64", COMMIT, "1", "1")


class MappedCodeFingerprintTest(unittest.TestCase):
    COMMANDS = """cmd LC_SEGMENT_64
Section
 sectname __text
 segname __TEXT
 addr 0x100000040
 size 0x4
 offset 64
 flags 0x80000400
Section
 sectname __bss
 segname __DATA
 addr 0x100010000
 size 0x1000
 offset 0
 flags 0x1
"""

    def test_signature_bytes_do_not_change_code_fingerprint_but_code_bytes_do(self):
        import struct
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "executable"
            header = struct.pack("<8I", 0xfeedfacf, 0x0100000c, 0, 2, 1, 8, 0, 0).ljust(64, b"\0")
            path.write_bytes(header + b"CODE" + b"old signature")
            with patch.object(native_delivery, "_tool", return_value=self.COMMANDS):
                before = producer._mach_code_data(path)
                path.write_bytes(header + b"CODE" + b"a different final signature")
                self.assertEqual(producer._mach_code_data(path), before)
                path.write_bytes(header + b"FAIL" + b"a different final signature")
                self.assertNotEqual(producer._mach_code_data(path), before)

    def test_out_of_bounds_section_cannot_be_fingerprinted(self):
        import struct
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "executable"
            path.write_bytes(struct.pack("<8I", 0xfeedfacf, 0x0100000c, 0, 2, 1, 8, 0, 0).ljust(64, b"\0") + b"CODE")
            with patch.object(native_delivery, "_tool", return_value=self.COMMANDS.replace("offset 64", "offset 999999")):
                with self.assertRaisesRegex(ValueError, "bound"):
                    producer._mach_code_data(path)


class EditorStripOrderTest(unittest.TestCase):
    def test_app_entry_limit_precedes_copy_or_strip(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);root.chmod(0o700)
            app = root / "input.app";app.mkdir()
            (app / "one").write_bytes(b"a");(app / "two").write_bytes(b"b")
            with patch.object(producer, "MAX_APP_FILES", 1), patch.object(native_delivery, "prepare") as prepare:
                with self.assertRaisesRegex(ValueError, "large"):
                    producer.prepare_editor(app, root / "fresh-work", root / "fresh-symbols", root / "matched.dSYM")
                prepare.assert_not_called()
            self.assertFalse((root / "fresh-work").exists())

    def test_editor_code_mutation_during_strip_or_seal_stops_delivery(self):
        for fingerprints in (("a" * 64, "b" * 64), ("a" * 64, "a" * 64, "b" * 64)):
            with self.subTest(fingerprints=len(fingerprints)), tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary);root.chmod(0o700)
                app = root / "input.app";executable = app / "Contents/MacOS/Donner"
                executable.parent.mkdir(parents=True);executable.write_bytes(b"unstripped input")
                def prepare(**kwargs):
                    kwargs["output"].write_bytes(b"prepared output")
                    kwargs["symbols"].mkdir(mode=0o700)
                    return {"identity": "paired", "unwind_sections_sha256": {}}
                with patch.object(native_delivery, "prepare", side_effect=prepare), patch.object(native_delivery, "_tool"), patch.object(native_delivery, "_uuid", return_value="paired"), patch.object(native_delivery, "_mach_unwind", return_value={}), patch.object(producer, "_mach_code_data", side_effect=fingerprints):
                    with self.assertRaisesRegex(ValueError, "code"):
                        producer.prepare_editor(app, root / "work", root / "symbols", root / "matching.dSYM")
                self.assertEqual(executable.read_bytes(), b"unstripped input")
                self.assertFalse((root / "symbols/delivery.json").exists())

    def test_copy_strip_then_final_seal_preserves_input_and_resources(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary);root.chmod(0o700)
            app = root / "original.app";(app / "Contents/MacOS").mkdir(parents=True)
            executable = app / "Contents/MacOS/Donner";executable.write_bytes(b"original unstripped app");executable.chmod(0o555)
            resource = app / "Contents/Resources/manifest.json";resource.parent.mkdir();resource.write_bytes(b"immutable resource");resource.chmod(0o444)
            work = root / "work";symbols = root / "symbols";work.mkdir(mode=0o700);symbols.mkdir(mode=0o700)
            events = []
            def prepare(**kwargs):
                events.append("strip-fresh-copy")
                self.assertNotEqual(kwargs["binary"], executable)
                self.assertFalse(kwargs["sign"])
                kwargs["output"].write_bytes(b"stripped app")
                kwargs["symbols"].mkdir(mode=0o700)
                return {"identity": "paired", "unwind_sections_sha256": {}, "binary_sha256": digest(b"stripped app")}
            def tool(*args):
                events.append("seal" if "--force" in args else "verify-seal")
                if "--force" in args:
                    target = Path(args[-1]) / "Contents/MacOS/Donner";target.write_bytes(b"stripped and sealed app")
                return ""
            executable.parent.chmod(0o555)
            try:
                with patch.object(native_delivery, "prepare", side_effect=prepare), patch.object(native_delivery, "_tool", side_effect=tool), patch.object(native_delivery, "_uuid", return_value="paired"), patch.object(native_delivery, "_mach_unwind", return_value={}), patch.object(producer, "_mach_code_data", return_value="c" * 64):
                    result = producer.prepare_editor(app, work / "editor", symbols / "editor", root / "matched.dSYM")
                self.assertEqual(executable.parent.stat().st_mode & 0o777, 0o555)
            finally:
                executable.parent.chmod(0o755)
            self.assertEqual(events, ["strip-fresh-copy", "seal", "verify-seal"])
            self.assertEqual(executable.read_bytes(), b"original unstripped app")
            self.assertEqual(resource.read_bytes(), b"immutable resource")
            self.assertEqual((result / "Contents/Resources/manifest.json").read_bytes(), resource.read_bytes())
            metadata = json.loads((symbols / "editor/delivery.json").read_text())
            self.assertNotEqual(metadata["binary_sha256"], metadata["final_binary_sha256"])


if __name__ == "__main__":
    unittest.main()
