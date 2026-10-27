"""Generate authenticated metadata and content-addressed installation assets."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil

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
    (output / "manifest.json").write_text(json.dumps(manifest, sort_keys=True, indent=2) + "\n")
    lines = ['#pragma once', '/// @file', '', '#include "donner/base/PackagedResource.h"', '',
             'namespace donner::embedded {']
    for record in records:
        fields = [json.dumps(record[key]) for key in
                  ("id", "sha256", "encodedSize", "decodedSize", "mediaType", "group", "path")]
        fields.append(json.dumps(request["runfilesRoot"]))
        lines.append(f'inline const PackagedResource {record["id"]}{{' + ', '.join(fields) + '};')
    lines.extend(['}  // namespace donner::embedded', ''])
    Path(header_path).write_text('\n'.join(lines))


def merge(inputs, output):
    """Merge generated sets, verifying their entire identity and inventory."""
    output = Path(output)
    (output / "assets").mkdir(parents=True)
    records = {}
    for directory in inputs:
        directory = Path(directory)
        manifest = json.loads((directory / "manifest.json").read_text())
        if manifest.get("schemaVersion") != 1:
            raise ValueError("Unsupported resource schema")
        for record in manifest["assets"]:
            source = directory / record["path"]
            payload = source.read_bytes()
            expected = resource_record(record["id"], payload, source.suffix[1:], record["group"],
                                       record["decodedSize"])
            if record != expected:
                raise ValueError("Resource identity does not match delivered bytes")
            if record["id"] in records and record != records[record["id"]]:
                raise ValueError("Conflicting logical resource IDs")
            records[record["id"]] = record
            shutil.copyfile(source, output / record["path"])
    assets = sorted(records.values(), key=lambda record: record["id"])
    if sum({record["sha256"]: record["encodedSize"] for record in assets}.values()) > MAX_ENCODED:
        raise ValueError("Installed resource set exceeds encoded retention budget")
    (output / "manifest.json").write_text(
        json.dumps(dict(schemaVersion=1, assets=assets), sort_keys=True, indent=2) + "\n")


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
