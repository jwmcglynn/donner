"""Bounded extraction of the signed producer's test-only, uncompressed USTAR data."""

from pathlib import Path, PurePosixPath

from tools import release_macos_editor as native


_BLOCK = 512


def _octal(field):
    value = field.strip(b" \0")
    if any(byte not in b"01234567" for byte in value):
        raise ValueError("native test bundle has an invalid numeric header")
    return int(value or b"0", 8)


def _text(field):
    value, _, padding = field.partition(b"\0")
    if any(padding):
        raise ValueError("native test bundle has an invalid text header")
    return value.decode("ascii")


def _record(header, offset):
    if header[257:265] != b"ustar\x0000" or header[156:157] not in (b"0", b"\0", b"5"):
        raise ValueError("native test bundle requires regular USTAR members")
    if sum(header[:148] + b" " * 8 + header[156:]) != _octal(header[148:156]):
        raise ValueError("native test bundle has an invalid header checksum")
    name = _text(header[:100])
    prefix = _text(header[345:500])
    name = (prefix + "/" if prefix else "") + name
    directory = header[156:157] == b"5"
    name = name.removesuffix("/") if directory else name
    native._safe_name(native.BUNDLE + "/" + name)
    if PurePosixPath(name).parts[0] != "Contents" or any(header[157:257]):
        raise ValueError("native test bundle has an unexpected member")
    mode, size = _octal(header[100:108]), _octal(header[124:136])
    if mode not in ((0o755,) if directory else (0o644, 0o755)) or (directory and size):
        raise ValueError("native test bundle has an invalid mode or directory size")
    if size > native.MAX_MEMBER_BYTES:
        raise ValueError("native test bundle member is too large")
    if (_octal(header[108:116]) or _octal(header[116:124])
            or _text(header[265:297]) != "root" or _text(header[297:329]) != "wheel"):
        raise ValueError("native test bundle owner metadata is not normalized")
    return {"name": name, "directory": directory, "mode": mode, "size": size, "offset": offset}


def _finish(stream):
    if stream.read(_BLOCK) != bytes(_BLOCK):
        raise ValueError("native test bundle is missing its end marker")
    while block := stream.read(1024 * 1024):
        if any(block):
            raise ValueError("native test bundle has trailing data")


def _scan(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError("native test bundle requires a regular archive")
    size = path.stat().st_size
    if not 2 * _BLOCK <= size <= native.MAX_ARCHIVE_BYTES or size % _BLOCK:
        raise ValueError("native test bundle is linked, truncated or too large")
    records, names, total = [], set(), 0
    with path.open("rb") as stream:
        while True:
            header = stream.read(_BLOCK)
            if len(header) != _BLOCK:
                raise ValueError("native test bundle is truncated")
            if not any(header):
                _finish(stream)
                return records
            record = _record(header, stream.tell())
            name = record["name"].casefold()
            total += record["size"]
            if name in names or len(records) >= native.MAX_FILES or total > native.MAX_TOTAL_BYTES:
                raise ValueError("native test bundle has duplicate or excessive members")
            names.add(name)
            records.append(record)
            stream.seek((record["size"] + _BLOCK - 1) // _BLOCK * _BLOCK, 1)
            if stream.tell() > size:
                raise ValueError("native test bundle has a truncated member")


def _validate_structure(records):
    directories = {record["name"] for record in records if record["directory"]}
    files = {record["name"] for record in records if not record["directory"]}
    required = {name.split("/", 1)[1] for name in native.REQUIRED - {"LICENSE", "NOTICE.txt"}}
    if "Contents" not in directories or not required <= files:
        raise ValueError("native test bundle is missing required application members")
    for record in records:
        parents = PurePosixPath(record["name"]).parents
        if any(str(parent) != "." and str(parent) not in directories for parent in parents):
            raise ValueError("native test bundle has a missing or conflicting parent")


def _copy_member(stream, record, destination):
    stream.seek(record["offset"])
    remaining = record["size"]
    with destination.open("xb") as output:
        while remaining:
            block = stream.read(min(remaining, 1024 * 1024))
            if not block:
                raise ValueError("native test bundle changed during extraction")
            output.write(block)
            remaining -= len(block)
    destination.chmod(record["mode"])


def extract_bundle(archive, destination):
    """Validate all headers before creating a fresh tree, without following any archive links."""
    archive, destination = Path(archive), Path(destination)
    records = _scan(archive)
    _validate_structure(records)
    if destination.exists() or destination.is_symlink():
        raise ValueError("native test bundle extraction requires a fresh destination")
    destination.mkdir(mode=0o755)
    destination.chmod(0o755)
    directories = sorted((record for record in records if record["directory"]),
                         key=lambda record: len(PurePosixPath(record["name"]).parts))
    for record in directories:
        directory = destination / record["name"]
        directory.mkdir(mode=record["mode"])
        directory.chmod(record["mode"])
    with archive.open("rb") as stream:
        for record in records:
            if not record["directory"]:
                _copy_member(stream, record, destination / record["name"])
