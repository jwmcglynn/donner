"""Keep the source archive NOTICE equal to the default renderer's license catalog."""

from __future__ import annotations

from hashlib import sha256
from pathlib import Path
import sys


def digest(path: Path) -> str:
    with path.open("rb") as stream:
        return sha256(stream.read()).hexdigest()


def main() -> int:
    if len(sys.argv) != 3:
        print("expected committed NOTICE and generated notice paths", file=sys.stderr)
        return 2
    committed, generated = map(Path, sys.argv[1:])
    if digest(committed) != digest(generated):
        print("NOTICE differs from //third_party/licenses:notice_default.txt", file=sys.stderr)
        return 1
    print("Committed default-renderer NOTICE matches the generated license catalog")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
