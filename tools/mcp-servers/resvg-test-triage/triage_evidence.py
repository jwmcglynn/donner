"""Read-only, bounded evidence handling shared by the optional triage tools.

PNG pixels are never used to reconstruct a pixelmatch mismatch count. The display
image includes unchanged pixels and anti-aliasing annotations, so only the runner
can supply the comparator's count and options.
"""

import os
import stat
import struct
import zlib
from dataclasses import dataclass
from pathlib import Path

MAX_IMAGE_BYTES = 16 * 1024 * 1024
MAX_IMAGE_DIMENSION = 4096
MAX_IMAGE_PIXELS = 8 * 1024 * 1024
MAX_TEXT_BYTES = 4 * 1024 * 1024


@dataclass(frozen=True)
class PngArtifact:
    """PNG bytes with validated container metadata, without decoding pixels."""

    data: bytes
    width: int
    height: int


def bounded_text(value: str) -> str:
    """Reject oversized inputs before XML or regular-expression processing."""
    if not isinstance(value, str) or len(value.encode("utf-8")) > MAX_TEXT_BYTES:
        raise ValueError("Text input exceeds the 4 MiB limit or is not a string")
    return value


def comparator_count(value: int | None) -> int | None:
    """Validate a supplied count without guessing one when evidence is absent."""
    if value is not None and (type(value) is not int or value < 0):
        raise ValueError("pixel_diff must be a non-negative integer or null")
    return value


def failure_assessment(pixel_diff: int | None) -> tuple[str, str]:
    """Counts alone cannot establish root cause, severity or acceptability."""
    comparator_count(pixel_diff)
    return "unresolved", "unknown"


def read_bounded_artifact(image_path: str, artifact_root: str | None = None) -> bytes:
    """Read only regular files beneath an explicitly trusted root, with no symlinks.

    The server configures the root at startup; tool arguments cannot widen it.
    Directory descriptors keep traversal confined even if a path is renamed.
    """
    if not artifact_root:
        raise ValueError("Image access requires DONNER_TRIAGE_ARTIFACT_ROOT")
    root = Path(artifact_root)
    path = Path(image_path)
    if not root.is_absolute() or ".." in root.parts or ".." in path.parts:
        raise ValueError("Artifact root must be absolute and paths must not traverse parents")
    if path.is_absolute():
        try:
            path = path.relative_to(root)
        except ValueError as error:
            raise ValueError("Image is outside the configured artifact root") from error
    if not path.parts:
        raise ValueError("Image path must name a file")

    if not all(hasattr(os, flag) for flag in ("O_DIRECTORY", "O_NOFOLLOW", "O_NONBLOCK")):
        raise ValueError("Secure artifact reads require POSIX descriptor traversal")
    directory_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    descriptor = os.open(root.anchor, directory_flags)
    try:
        for part in (*root.parts[1:], *path.parts[:-1]):
            child = os.open(part, directory_flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        file_descriptor = os.open(
            path.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=descriptor
        )
        with os.fdopen(file_descriptor, "rb") as stream:
            metadata = os.fstat(stream.fileno())
            if not stat.S_ISREG(metadata.st_mode) or metadata.st_size > MAX_IMAGE_BYTES:
                raise ValueError("Image must be a regular file no larger than 16 MiB")
            data = stream.read(MAX_IMAGE_BYTES + 1)
            if len(data) > MAX_IMAGE_BYTES:
                raise ValueError("Image exceeds the 16 MiB limit")
            return data
    finally:
        os.close(descriptor)


def _png_chunks(data: bytes):
    """Yield checksum-validated PNG chunks without decoding compressed payloads."""
    if len(data) > MAX_IMAGE_BYTES or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("Expected a bounded PNG image")
    position = 8
    while position + 12 <= len(data):
        length = struct.unpack_from(">I", data, position)[0]
        end = position + 12 + length
        if end > len(data):
            raise ValueError("Truncated PNG chunk")
        kind = data[position + 4:position + 8]
        payload = data[position + 8:end - 4]
        checksum = struct.unpack_from(">I", data, end - 4)[0]
        if zlib.crc32(kind + payload) != checksum:
            raise ValueError("Invalid PNG chunk checksum")
        if kind == b"IEND" and end != len(data):
            raise ValueError("Trailing data after PNG end")
        yield kind, payload
        position = end
    if position != len(data):
        raise ValueError("Incomplete PNG container")


def _png_dimensions(payload: bytes) -> tuple[int, int]:
    """Validate encoding and bound decoded dimensions before forwarding a PNG."""
    if len(payload) != 13:
        raise ValueError("PNG must begin with a single IHDR")
    width, height, depth, color, compression, filtering, interlace = struct.unpack(
        ">IIBBBBB", payload
    )
    depths = {0: (1, 2, 4, 8, 16), 2: (8, 16), 3: (1, 2, 4, 8),
              4: (8, 16), 6: (8, 16)}
    if depth not in depths.get(color, ()) or compression or filtering or interlace > 1:
        raise ValueError("Invalid PNG encoding")
    if not (0 < width <= MAX_IMAGE_DIMENSION and 0 < height <= MAX_IMAGE_DIMENSION):
        raise ValueError("PNG dimensions exceed the 4096-per-axis limit")
    if width * height > MAX_IMAGE_PIXELS:
        raise ValueError("PNG decoded pixel count exceeds the 8 Mi-pixel limit")
    return width, height


class _PngStructure:
    """Track required PNG container structure without interpreting pixel data."""

    def __init__(self):
        self.width = 0
        self.height = 0
        self.saw_data = False
        self.ended = False

    def accept(self, index: int, kind: bytes, payload: bytes) -> None:
        if index == 0:
            if kind != b"IHDR":
                raise ValueError("PNG must begin with a single IHDR")
            self.width, self.height = _png_dimensions(payload)
            return
        if kind == b"IHDR":
            raise ValueError("Duplicate PNG header")
        if kind in (b"acTL", b"fcTL", b"fdAT"):
            raise ValueError("Animated PNG is not a static comparison artifact")
        if kind == b"IDAT":
            self.saw_data = True
        if kind == b"IEND":
            if payload or not self.saw_data:
                raise ValueError("Invalid PNG end or missing image data")
            self.ended = True


def inspect_png(data: bytes) -> PngArtifact:
    """Validate bounded static PNG container metadata, not pixels or visual equality."""
    structure = _PngStructure()
    for index, (kind, payload) in enumerate(_png_chunks(data)):
        structure.accept(index, kind, payload)
    if not structure.ended:
        raise ValueError("Incomplete PNG container")
    return PngArtifact(data, structure.width, structure.height)


def read_png(image_path: str, artifact_root: str | None = None) -> PngArtifact:
    """Read a confined static PNG and validate its metadata before forwarding it."""
    return inspect_png(read_bounded_artifact(image_path, artifact_root))


def visual_evidence(images: dict[str, PngArtifact], pixel_diff: int | None = None) -> dict:
    """Report container evidence and the caller's comparator metric separately."""
    pixel_diff = comparator_count(pixel_diff)
    dimensions = {(image.width, image.height) for image in images.values()}
    if len(dimensions) != 1:
        raise ValueError("Comparison artifacts must have matching dimensions")
    width, height = next(iter(dimensions))
    if pixel_diff is not None and pixel_diff > width * height:
        raise ValueError("Comparator count exceeds the image's pixel count")
    return {
        "difference_type": "unresolved",
        "visual_analysis": {
            "width": width,
            "height": height,
            "total_pixels": width * height,
            "diff_pixels": pixel_diff,
            "metric_source": "caller_supplied_comparator" if pixel_diff is not None else None,
            "image_bytes": {name: len(image.data) for name, image in images.items()},
        },
        "likely_cause": None,
        "confidence": "unknown",
        "hypotheses": [],
        "note": "Display-diff colors are not a mismatch mask. Inspect source, spec and images.",
    }
