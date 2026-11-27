#!/usr/bin/env python3
"""Prepare fresh native products and curate a separate diagnostic artifact inventory."""
from __future__ import annotations

import argparse
from hashlib import sha256
import json
import os
from pathlib import Path
import re
import shutil
import stat

from tools import native_delivery

PLATFORMS = {"linux-x86-64": "linux", "darwin-arm64": "macos"}
ARCHIVES = {"linux-x86-64": "donner-cli_linux_x86_64.tar.gz",
            "darwin-arm64": "donner-cli_darwin_arm64.zip"}
EDITOR_ARCHIVE = "donner-editor_darwin_arm64.zip"
MAX_METADATA_BYTES = 1024 * 1024
MAX_SYMBOL_BYTES = 512 * 1024 * 1024
MAX_APP_BYTES = 1024 * 1024 * 1024
MAX_APP_FILES = 2048
PRIVATE_PATH = re.compile(rb"/(?:Users|home|Volumes)/|/(?:private/)?var/(?:folders|tmp)/|/tmp/")
HEX = re.compile(r"[0-9a-f]{64}")
COMMIT = re.compile(r"[0-9a-f]{40}")
POSITIVE = re.compile(r"[1-9][0-9]*")
NATIVE_FIELDS = {"schema", "platform", "identity", "source_sha256", "binary_sha256",
                 "source_size", "binary_size", "symbol_file", "symbol_sha256",
                 "unwind_sections_sha256"}
FINAL_FIELDS = {"final_binary_sha256", "final_binary_size", "final_seal", "code_data_sha256"}


def _digest(path):
    h = sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def _private(path):
    if (path.is_symlink() or not path.is_dir() or path.stat().st_uid != os.geteuid()
            or stat.S_IMODE(path.stat().st_mode) & 0o077):
        raise ValueError("native producer requires an owned private staging directory")


def roots(temp, platform):
    if platform not in PLATFORMS:
        raise ValueError("unsupported native producer platform")
    return temp / "donner-native-delivery" / platform, temp / "donner-native-symbols" / platform


def check_workspace_overrides(root):
    """Do not let ignored workspace configuration add a second diagnostic destination."""
    for name in (".buildbuddy.bazelrc", ".bazelrc.user"):
        if os.path.lexists(root / name):
            raise ValueError("native producer refuses workspace override fragments")


def initialize(temp, platform):
    """Reserve fresh staging outside the ordinary product artifact directories."""
    if temp.is_symlink() or not temp.is_dir() or temp.stat().st_uid != os.geteuid():
        raise ValueError("native producer temporary root is not an owned directory")
    check_workspace_overrides(Path.cwd())
    work, symbols = roots(temp, platform)
    for parent in (work.parent, symbols.parent):
        if os.path.lexists(parent):
            raise ValueError("native producer staging already exists")
    for parent, child in ((work.parent, work), (symbols.parent, symbols)):
        parent.mkdir(mode=0o700)
        child.mkdir(mode=0o700)


def prepare_cli(temp, platform, binary, dsym=None):
    work, symbols = roots(temp, platform)
    _private(work)
    _private(symbols)
    before_code = _mach_code_data(binary.resolve()) if platform == "darwin-arm64" else None
    result = native_delivery.prepare(binary=binary.resolve(), output=work / "donner-cli",
                                     symbols=symbols / "cli", platform=platform,
                                     dsym=dsym.resolve() if dsym else None,
                                     sign=platform == "darwin-arm64")
    if before_code is not None:
        output = work / "donner-cli"
        if _mach_code_data(output) != before_code:
            raise ValueError("native CLI delivery changed mapped code or data")
        result.update(final_binary_sha256=_digest(output), final_binary_size=output.stat().st_size,
                      final_seal="ad-hoc", code_data_sha256=before_code)
        (symbols / "cli/delivery.json").write_text(json.dumps(result, sort_keys=True, indent=2) + "\n")
    return result


def _mach_code_data(path):
    """Fingerprint mapped code/data sections; signing may change only link-edit metadata."""
    native_delivery.validate_image(path, "darwin-arm64")
    length = path.stat().st_size
    if not 0 < length <= MAX_APP_BYTES:
        raise ValueError("native Mach-O exceeds its inspection bound")
    commands = native_delivery._tool("/usr/bin/otool", "-l", str(path))
    if re.search(r"(?m)^\s*cmd LC_SEGMENT_64\s*$", commands) is None:
        raise ValueError("native Mach-O section commands are invalid")
    sections, inspected = {}, 0
    with path.open("rb") as image:
        for section in commands.split("Section\n")[1:]:
            fields = {}
            for name, pattern in (("name", r"sectname ([A-Za-z0-9_]{1,16})\s"),
                                  ("segment", r"segname ([A-Za-z0-9_]{1,16})\s"),
                                  ("address", r"\baddr (0x[0-9a-fA-F]+)"),
                                  ("size", r"\bsize (0x[0-9a-fA-F]+)"),
                                  ("offset", r"\boffset ([0-9]+)"),
                                  ("flags", r"\bflags (0x[0-9a-fA-F]+)")):
                match = re.search(pattern, section)
                if not match:
                    raise ValueError("native Mach-O section metadata is invalid")
                fields[name] = match[1]
            if fields["segment"] == "__LINKEDIT":
                continue
            key = fields["segment"] + "," + fields["name"]
            if key in sections or len(sections) >= 4096:
                raise ValueError("native Mach-O section inventory is ambiguous or too large")
            size, offset, flags = int(fields["size"], 16), int(fields["offset"]), int(fields["flags"], 16)
            record = {"address": int(fields["address"], 16), "size": size, "flags": flags}
            if flags & 0xff in (1, 12, 18):  # Mach-O zero-fill section kinds have no file payload.
                record["zero_fill"] = True
            else:
                inspected += size
                if offset + size > length or inspected > length:
                    raise ValueError("native Mach-O section exceeds its file byte bound")
                image.seek(offset)
                h, remaining = sha256(), size
                while remaining:
                    block = image.read(min(65536, remaining))
                    if not block:
                        raise ValueError("native Mach-O section was truncated")
                    h.update(block)
                    remaining -= len(block)
                record.update(offset=offset, sha256=h.hexdigest())
            sections[key] = record
    if "__TEXT,__text" not in sections or not sections["__TEXT,__text"]["size"]:
        raise ValueError("native Mach-O has no inspected code section")
    return sha256(json.dumps(sections, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def prepare_editor(app, work, symbols, dsym):
    """Strip only a fresh app copy, then seal that copy before any package hashing."""
    _private(work.parent)
    _private(symbols.parent)
    if os.path.lexists(work) or os.path.lexists(symbols):
        raise ValueError("native editor delivery needs fresh work and symbol paths")
    if app.is_symlink() or not app.is_dir():
        raise ValueError("native editor source bundle is linked or missing")
    count, total, pending = 0, 0, [app]
    while pending:
        with os.scandir(pending.pop()) as entries:
            for entry in entries:
                count += 1
                if count > MAX_APP_FILES or entry.is_symlink():
                    raise ValueError("native editor source bundle is linked or too large")
                if entry.is_dir(follow_symlinks=False):
                    pending.append(Path(entry.path))
                elif entry.is_file(follow_symlinks=False):
                    total += entry.stat(follow_symlinks=False).st_size
                    if total > MAX_APP_BYTES:
                        raise ValueError("native editor source bundle exceeds its byte budget")
                else:
                    raise ValueError("native editor source bundle has unsupported entries")
    work.mkdir(mode=0o700)
    copied = work / "Donner.app"
    shutil.copytree(app, copied, copy_function=shutil.copyfile)
    copied.chmod(0o755)
    for path in copied.rglob("*"):
        if path.is_dir():
            path.chmod(0o755)
        elif path.is_file():
            original = app / path.relative_to(copied)
            path.chmod(0o755 if original.stat().st_mode & 0o111 else 0o644)
    executable = copied / "Contents/MacOS/Donner"
    prepared = work / "Donner.prepared"
    before_code = _mach_code_data(executable)
    metadata = native_delivery.prepare(binary=executable, output=prepared, symbols=symbols,
                                       platform="darwin-arm64", dsym=dsym.resolve(), sign=False)
    if _mach_code_data(prepared) != before_code:
        raise ValueError("native editor stripping changed mapped code or data")
    os.replace(prepared, executable)  # Replaces only the executable in this fresh owned copy.
    native_delivery._tool("/usr/bin/codesign", "--force", "--sign", "-", "--timestamp=none",
                          str(copied))
    native_delivery._tool("/usr/bin/codesign", "--verify", "--deep", "--strict", str(copied))
    if (native_delivery._uuid(executable) != metadata["identity"]
            or native_delivery._mach_unwind(executable) != metadata["unwind_sections_sha256"]
            or _mach_code_data(executable) != before_code):
        raise ValueError("native editor final seal changed identity, mapped code/data or unwind")
    metadata.update(final_binary_sha256=_digest(executable),
                    final_binary_size=executable.stat().st_size, final_seal="ad-hoc",
                    code_data_sha256=before_code)
    (symbols / "delivery.json").write_text(json.dumps(metadata, sort_keys=True, indent=2) + "\n")
    return copied


def _json(path):
    if (path.is_symlink() or not path.is_file()
            or not 0 < path.stat().st_size <= MAX_METADATA_BYTES):
        raise ValueError("native diagnostic metadata is missing, linked or oversized")
    value = json.loads(path.read_text())
    if not isinstance(value, dict):
        raise ValueError("native diagnostic metadata must be an object")
    return value


def _symbol_name(platform, product):
    if platform == "linux-x86-64":
        return "binary.debug"
    return "binary.dSYM/Contents/Resources/DWARF/" + (
        "donner-cli" if product == "cli" else "editor_impl")


def _metadata(stage, platform, product):
    metadata = _json(stage / product / "delivery.json")
    keys = set(metadata)
    if keys not in (NATIVE_FIELDS, NATIVE_FIELDS | FINAL_FIELDS):
        raise ValueError("native diagnostic metadata contains unreviewed fields")
    identity = metadata.get("identity", "")
    identity_pattern = r"[0-9a-f]{40}" if platform == "linux-x86-64" else r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}"
    if (type(metadata.get("schema")) is not int or metadata["schema"] != 1
            or metadata.get("platform") != platform
            or not isinstance(identity, str) or not re.fullmatch(identity_pattern, identity)
            or metadata.get("symbol_file") != _symbol_name(platform, product)):
        raise ValueError("native diagnostic identity or symbol path is invalid")
    for name in ("source_sha256", "binary_sha256", "symbol_sha256"):
        if not isinstance(metadata.get(name), str) or not HEX.fullmatch(metadata[name]):
            raise ValueError("native diagnostic digest is invalid")
    for name in ("source_size", "binary_size"):
        if type(metadata.get(name)) is not int or not 0 < metadata[name] <= MAX_SYMBOL_BYTES:
            raise ValueError("native diagnostic size is invalid")
    permitted = ({".eh_frame", ".eh_frame_hdr", ".gcc_except_table"} if platform == "linux-x86-64"
                 else {"__eh_frame", "__unwind_info"})
    unwind = metadata.get("unwind_sections_sha256")
    if (not isinstance(unwind, dict) or not set(unwind) <= permitted
            or any(not isinstance(h, str) or not HEX.fullmatch(h) for h in unwind.values())
            or (platform == "linux-x86-64" and ".eh_frame" not in unwind)):
        raise ValueError("native diagnostic unwind inventory is invalid")
    if FINAL_FIELDS <= keys:
        if (metadata["final_seal"] != "ad-hoc" or platform != "darwin-arm64"
                or not isinstance(metadata["final_binary_sha256"], str)
                or not HEX.fullmatch(metadata["final_binary_sha256"])
                or not isinstance(metadata["code_data_sha256"], str)
                or not HEX.fullmatch(metadata["code_data_sha256"])
                or type(metadata["final_binary_size"]) is not int
                or not 0 < metadata["final_binary_size"] <= MAX_SYMBOL_BYTES):
            raise ValueError("native diagnostic final seal metadata is invalid")
    elif platform == "darwin-arm64":
        raise ValueError("native diagnostics are not bound to the final macOS seal")
    return metadata


def _expected_files(platform):
    names = {"custody.json"}
    for product in (("cli", "editor") if platform == "darwin-arm64" else ("cli",)):
        names.update((product + "/delivery.json", product + "/" + _symbol_name(platform, product)))
        if platform == "darwin-arm64":
            names.add(product + "/binary.dSYM/Contents/Info.plist")
    return names


def _has_private_path(path):
    tail = b""
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(65536), b""):
            if PRIVATE_PATH.search(tail + chunk):
                return True
            tail = chunk[-64:]
    return False


def _inventory(stage, platform, include_custody):
    _private(stage)
    allowed = _expected_files(platform)
    if not include_custody:
        allowed.remove("custody.json")
    parents = {str(parent) for name in allowed for parent in Path(name).parents}
    names, pending = set(), [stage]
    while pending:
        with os.scandir(pending.pop()) as entries:
            for entry in entries:
                path = Path(entry.path)
                name = path.relative_to(stage).as_posix()
                if entry.is_symlink():
                    raise ValueError("native diagnostic inventory contains a link")
                if entry.is_dir(follow_symlinks=False):
                    if name not in parents:
                        raise ValueError("native diagnostic inventory contains an unreviewed directory")
                    pending.append(path)
                    continue
                if not entry.is_file(follow_symlinks=False) or name not in allowed:
                    raise ValueError("native diagnostic inventory contains an unreviewed file")
                if not 0 < entry.stat(follow_symlinks=False).st_size <= MAX_SYMBOL_BYTES:
                    raise ValueError("native diagnostic file exceeds its byte budget")
                if _has_private_path(path):
                    raise ValueError("native diagnostic inventory contains a private build path")
                names.add(name)
    if names != allowed:
        raise ValueError("native diagnostic inventory is incomplete")


def _custody(temp, platform, commit, run_id, attempt, include_custody):
    if not COMMIT.fullmatch(commit) or not POSITIVE.fullmatch(run_id) or not POSITIVE.fullmatch(attempt):
        raise ValueError("native custody requires a source commit, run and attempt")
    _, stage = roots(temp, platform)
    _inventory(stage, platform, include_custody)
    products, source_tree = {}, None
    for product in (("cli", "editor") if platform == "darwin-arm64" else ("cli",)):
        metadata = _metadata(stage, platform, product)
        symbol = stage / product / metadata["symbol_file"]
        if _digest(symbol) != metadata["symbol_sha256"]:
            raise ValueError("native diagnostic symbol digest differs from its pair")
        work, _ = roots(temp, platform)
        binary = (work / "donner-cli" if product == "cli"
                  else work / "editor/Donner.app/Contents/MacOS/Donner")
        final_hash = metadata.get("final_binary_sha256", metadata["binary_sha256"])
        final_size = metadata.get("final_binary_size", metadata["binary_size"])
        if (binary.is_symlink() or not binary.is_file() or binary.stat().st_size != final_size
                or _digest(binary) != final_hash):
            raise ValueError("native diagnostic final executable differs from its pair")
        native_delivery.validate_image(binary, platform)
        identity = native_delivery._uuid if platform == "darwin-arm64" else native_delivery._build_id
        unwind = native_delivery._mach_unwind if platform == "darwin-arm64" else native_delivery._elf_unwind
        if (identity(binary) != metadata["identity"] or identity(symbol) != metadata["identity"]
                or unwind(binary) != metadata["unwind_sections_sha256"]):
            raise ValueError("native diagnostic symbol identity or unwind differs from its executable")
        if "code_data_sha256" in metadata and _mach_code_data(binary) != metadata["code_data_sha256"]:
            raise ValueError("native diagnostic code/data differs from its final sealed product")
        if platform == "linux-x86-64":
            native_delivery._require_elf_source_lines(symbol)
        else:
            native_delivery._tool("/usr/bin/dwarfdump", "--verify", str(symbol))
            native_delivery._require_macho_source_lines(symbol)
        archive_name = ARCHIVES[platform] if product == "cli" else EDITOR_ARCHIVE
        directory = (temp / ("donner-cli-" + PLATFORMS[platform]) if product == "cli"
                     else temp / "donner-editor-macos")
        provenance = _json(directory / (archive_name + ".provenance"))
        final_hash = metadata.get("final_binary_sha256", metadata["binary_sha256"])
        final_size = metadata.get("final_binary_size", metadata["binary_size"])
        tree = provenance.get("source_tree")
        if not isinstance(tree, str) or not COMMIT.fullmatch(tree) or (source_tree is not None and tree != source_tree):
            raise ValueError("native diagnostic products differ in source tree")
        source_tree = tree
        if (provenance.get("source_commit") != commit or provenance.get("producer_run_id") != run_id
                or provenance.get("producer_attempt") != attempt or provenance.get("platform") != platform):
            raise ValueError("native diagnostic custody differs from the product producer")
        if product == "cli":
            product_hash, product_size = provenance.get("binary_sha256"), provenance.get("binary_size")
        else:
            record = provenance.get("files", {}).get("Donner SVG Editor.app/Contents/MacOS/Donner", {})
            product_hash, product_size = record.get("sha256"), record.get("size")
        archive = directory / archive_name
        if (product_hash != final_hash or product_size != final_size or archive.is_symlink()
                or not archive.is_file() or archive.stat().st_size != provenance.get("archive_size")
                or _digest(archive) != provenance.get("archive_sha256")):
            raise ValueError("native diagnostic custody differs from the final product bytes")
        products[product] = {"identity": metadata["identity"],
                             "source_sha256": metadata["source_sha256"],
                             "prepared_binary_sha256": metadata["binary_sha256"],
                             "final_binary_sha256": final_hash, "final_binary_size": final_size,
                             "symbol_file": product + "/" + metadata["symbol_file"],
                             "symbol_sha256": metadata["symbol_sha256"],
                             "archive": archive_name, "archive_sha256": provenance["archive_sha256"]}
        if "code_data_sha256" in metadata:
            products[product]["code_data_sha256"] = metadata["code_data_sha256"]
    return {"schema": 1, "source_commit": commit, "source_tree": source_tree, "producer_run_id": run_id,
            "producer_attempt": attempt, "platform": platform, "products": products}


def finalize(temp, platform, commit, run_id, attempt):
    custody = _custody(temp, platform, commit, run_id, attempt, False)
    _, stage = roots(temp, platform)
    with (stage / "custody.json").open("x") as stream:
        stream.write(json.dumps(custody, sort_keys=True, indent=2) + "\n")
    validate(temp, platform, commit, run_id, attempt)


def validate(temp, platform, commit, run_id, attempt):
    expected = _custody(temp, platform, commit, run_id, attempt, True)
    _, stage = roots(temp, platform)
    if _json(stage / "custody.json") != expected:
        raise ValueError("native diagnostic custody manifest was changed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    for name in ("initialize", "prepare-cli", "finalize", "validate"):
        command = commands.add_parser(name)
        command.add_argument("--temp", type=Path, required=True)
        command.add_argument("--platform", choices=sorted(PLATFORMS), required=True)
        if name == "prepare-cli":
            command.add_argument("--binary", type=Path, required=True)
            command.add_argument("--dsym", type=Path)
        if name in ("finalize", "validate"):
            for field in ("commit", "run-id", "attempt"):
                command.add_argument("--" + field, required=True)
    args = vars(parser.parse_args())
    name = args.pop("command")
    {"initialize": initialize, "prepare-cli": prepare_cli, "finalize": finalize,
     "validate": validate}[name](**args)


if __name__ == "__main__":
    main()
