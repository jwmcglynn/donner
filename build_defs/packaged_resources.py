"""Generate authenticated metadata and content-addressed installation assets."""
import argparse
import hashlib
import json
from pathlib import Path
import re

MAX_ENCODED = 16 * 1024 * 1024
MEDIA = {"ttf": "font/ttf", "otf": "font/otf", "woff2": "font/woff2", "svg": "image/svg+xml"}


def resource_record(identity, payload, extension, group, decoded_size=None):
    """Describe exact delivered bytes, rejecting unsupported identities and sizes."""
    if not re.fullmatch(r"k[A-Za-z0-9]+", identity):
        raise ValueError("Invalid logical resource ID")
    if extension not in MEDIA or not 0 < len(payload) <= MAX_ENCODED:
        raise ValueError("Unsupported or oversized packaged resource")
    digest = hashlib.sha256(payload).hexdigest()
    return dict(id=identity, sha256=digest, encodedSize=len(payload),
                decodedSize=decoded_size or len(payload), mediaType=MEDIA[extension],
                group=group, path=f"assets/{digest}.{extension}")


def generate(request, header_path, output):
    """Create one metadata-only header and one complete resource tree."""
    output = Path(output)
    (output / "assets").mkdir(parents=True)
    records = []
    for entry in request["resources"]:
        source = Path(entry["source"])
        if source.stat().st_size > MAX_ENCODED:
            raise ValueError("Oversized resource source")
        payload = source.read_bytes()
        record = resource_record(entry["id"], payload, source.suffix[1:], request["group"])
        (output / record["path"]).write_bytes(payload)
        records.append(record)
    records.sort(key=lambda record: record["id"])
    if sum(record["encodedSize"] for record in records) > MAX_ENCODED:
        raise ValueError("Resource set exceeds encoded retention budget")
    manifest = dict(schemaVersion=1, assets=records)
    (output / "manifest.json").write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n")
    lines = ['#pragma once', '/// @file', '', '#include "donner/base/PackagedResource.h"', '',
             'namespace donner::embedded {']
    for record in records:
        fields = [json.dumps(record[key]) for key in
                  ("id", "sha256", "encodedSize", "decodedSize", "mediaType", "group", "path")]
        fields.append(json.dumps(request["runfilesRoot"]))
        lines.append(f'inline const PackagedResource {record["id"]}{{' + ', '.join(fields) + '};')
    lines.extend(['}  // namespace donner::embedded', ''])
    Path(header_path).write_text('\n'.join(lines))


def read_regular_bounded(path, limit):
    """Read one immutable-size regular file without following a final symlink."""
    import os
    import stat
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    try:
        metadata = os.fstat(descriptor)
        if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > limit:
            raise ValueError("Invalid or oversized packaged resource file")
        with os.fdopen(descriptor, "rb", closefd=False) as stream:
            payload = stream.read(limit + 1)
        if len(payload) != metadata.st_size or len(payload) > limit:
            raise ValueError("Packaged resource changed while reading")
        return payload
    finally:
        os.close(descriptor)


def validate_record_shape(record):
    """Require exact field names and primitive types before inspecting identity values."""
    fields = {"id", "sha256", "encodedSize", "decodedSize", "mediaType", "group", "path"}
    if not isinstance(record, dict) or record.keys() != fields:
        raise ValueError("Malformed packaged resource record")
    if any(type(record[key]) is not str for key in
           ("id", "sha256", "mediaType", "group", "path")):
        raise ValueError("Malformed packaged resource strings")
    if not re.fullmatch(r"k[A-Za-z0-9]+", record["id"]):
        raise ValueError("Invalid logical resource ID")
    if not re.fullmatch(r"[0-9a-f]{64}", record["sha256"]):
        raise ValueError("Invalid packaged resource digest")
    if any(type(record[key]) is not int for key in ("encodedSize", "decodedSize")):
        raise ValueError("Invalid packaged resource size type")
    if not 0 < record["encodedSize"] <= MAX_ENCODED or not 0 < record["decodedSize"] <= 64 * 1024 * 1024:
        raise ValueError("Invalid packaged resource size")


def validated_record(record):
    """Validate a canonical content address before it can select a disk read."""
    validate_record_shape(record)
    extension = record["path"].rsplit(".", 1)[-1]
    if extension not in MEDIA or record["path"] != f"assets/{record['sha256']}.{extension}":
        raise ValueError("Noncanonical packaged resource path")
    if record["mediaType"] != MEDIA[extension] or record["group"] not in {
            "ui-font", "fallback-font", "chrome", "sample", "catalog-font", "generic-font"}:
        raise ValueError("Invalid packaged resource media type or group")
    return extension


def unique_object(pairs):
    """Reject duplicate JSON members rather than choosing a parser-specific value."""
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON member")
        result[key] = value
    return result


def read_manifest(directory):
    """Read a small no-follow manifest and reject ambiguous or unknown schemas."""
    manifest = json.loads(read_regular_bounded(directory / "manifest.json", 128 * 1024),
                          object_pairs_hook=unique_object)
    if not isinstance(manifest, dict) or manifest.keys() != {"schemaVersion", "assets"}:
        raise ValueError("Malformed resource schema")
    if type(manifest["schemaVersion"]) is not int or manifest["schemaVersion"] != 1:
        raise ValueError("Unsupported resource schema")
    if not isinstance(manifest["assets"], list) or not 0 < len(manifest["assets"]) <= 128:
        raise ValueError("Invalid packaged resource record count")
    identities = set()
    for record in manifest["assets"]:
        validated_record(record)
        if record["id"] in identities:
            raise ValueError("Duplicate logical resource ID")
        identities.add(record["id"])
    return manifest


def verify_inventory(directory, records):
    """Reject missing, extra, special, and symlinked installation paths."""
    if directory.is_symlink() or (directory / "assets").is_symlink():
        raise ValueError("Packaged resource directory is a symlink")
    expected = {"manifest.json"} | {record["path"] for record in records}
    inventory = set()
    for path in directory.rglob("*"):
        if path.is_symlink():
            raise ValueError("Packaged resource inventory contains a symlink")
        if path.is_file():
            inventory.add(path.relative_to(directory).as_posix())
        elif path != directory / "assets":
            raise ValueError("Unexpected packaged resource directory")
    if inventory != expected:
        raise ValueError("Packaged resource inventory mismatch")


def verified_set(directory):
    """Validate schema and complete inventory before any resource content read."""
    directory = Path(directory)
    if directory.is_symlink():
        raise ValueError("Packaged resource directory is a symlink")
    manifest = read_manifest(directory)
    verify_inventory(directory, manifest["assets"])
    for record in manifest["assets"]:
        payload = read_regular_bounded(directory / record["path"], record["encodedSize"])
        if len(payload) != record["encodedSize"] or hashlib.sha256(payload).hexdigest() != record["sha256"]:
            raise ValueError("Resource identity does not match delivered bytes")
        yield record, payload


def merge(inputs, output):
    """Merge generated sets, verifying their entire identity and exact inventory."""
    output = Path(output)
    (output / "assets").mkdir(parents=True)
    records = {}
    retained = {}
    for directory in inputs:
        for record, payload in verified_set(directory):
            if record["id"] in records and record != records[record["id"]]:
                raise ValueError("Conflicting logical resource IDs")
            retained[record["sha256"]] = record["encodedSize"]
            if sum(retained.values()) > MAX_ENCODED:
                raise ValueError("Installed resource set exceeds encoded retention budget")
            records[record["id"]] = record
            (output / record["path"]).write_bytes(payload)
    assets = sorted(records.values(), key=lambda record: record["id"])
    if sum({record["sha256"]: record["encodedSize"] for record in assets}.values()) > MAX_ENCODED:
        raise ValueError("Installed resource set exceeds encoded retention budget")
    (output / "manifest.json").write_text(
        json.dumps(dict(schemaVersion=1, assets=assets), sort_keys=True, separators=(",", ":")) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--request")
    parser.add_argument("--header")
    parser.add_argument("--output", required=True)
    parser.add_argument("--merge", nargs="*")
    args = parser.parse_args()
    if args.merge is not None:
        merge(args.merge, args.output)
    else:
        generate(json.loads(Path(args.request).read_text()), args.header, args.output)


if __name__ == "__main__":
    main()
