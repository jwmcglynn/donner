#!/usr/bin/env python3
"""Publish verified release assets without replacing existing bytes."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
from urllib.parse import urlencode


def resolve_remote_tag(refs: str, tag: str) -> str | None:
    """Return the peeled commit for an annotated or lightweight tag."""
    exact_ref = f"refs/tags/{tag}"
    peeled_ref = f"{exact_ref}^{{}}"
    exact_commit = None
    peeled_commit = None
    for line in refs.splitlines():
        fields = line.split()
        if len(fields) != 2:
            continue
        commit, ref = fields
        if ref == peeled_ref:
            peeled_commit = commit
        elif ref == exact_ref:
            exact_commit = commit
    return peeled_commit or exact_commit


def sha256_digest(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return f"sha256:{digest.hexdigest()}"


class Publisher:
    def __init__(self, repository: str, release_id: str, upload_url: str):
        self.repository = repository
        self.release_id = release_id
        self.upload_base = upload_url.split("{", 1)[0]
        self.assets_endpoint = f"repos/{repository}/releases/{release_id}/assets?per_page=100"

    def list_assets(self) -> list[dict[str, object]]:
        result = subprocess.run(
            ["gh", "api", self.assets_endpoint],
            check=True,
            capture_output=True,
            text=True,
        )
        value = json.loads(result.stdout)
        if not isinstance(value, list):
            raise RuntimeError("GitHub release assets response is not an array")
        return value

    def existing_asset_matches(self, path: Path) -> bool:
        matches = [asset for asset in self.list_assets() if asset.get("name") == path.name]
        if not matches:
            return False
        if len(matches) != 1 or matches[0].get("digest") != sha256_digest(path):
            raise RuntimeError(f"release asset {path.name} exists with a different digest")
        return True

    def upload_once(self, path: Path) -> bool:
        endpoint = f"{self.upload_base}?{urlencode({'name': path.name})}"
        result = subprocess.run(
            [
                "gh",
                "api",
                "--method",
                "POST",
                "-H",
                "Content-Type: application/octet-stream",
                endpoint,
                "--input",
                str(path),
            ],
            check=False,
        )
        return result.returncode == 0

    def publish(self, paths: list[Path], maximum_attempts: int = 3) -> None:
        for path in paths:
            if self.existing_asset_matches(path):
                print(f"release asset {path.name} already has the expected digest")
                continue
            for attempt in range(1, maximum_attempts + 1):
                if self.upload_once(path) or self.existing_asset_matches(path):
                    break
                print(
                    f"release asset {path.name} upload attempt {attempt} failed",
                    file=sys.stderr,
                )
            else:
                raise RuntimeError(
                    f"release asset {path.name} upload failed after {maximum_attempts} attempts"
                )


def verify_remote_tag(tag: str, expected_commit: str) -> None:
    result = subprocess.run(
        [
            "git",
            "ls-remote",
            "--tags",
            "origin",
            f"refs/tags/{tag}",
            f"refs/tags/{tag}^{{}}",
        ],
        check=True,
        capture_output=True,
        text=True,
    )
    if resolve_remote_tag(result.stdout, tag) != expected_commit:
        raise RuntimeError("release tag does not resolve to GITHUB_SHA")


def main(argv: list[str]) -> int:
    if not argv:
        raise RuntimeError("at least one release asset path is required")
    tag = os.environ["RELEASE_TAG"]
    expected_commit = os.environ["GITHUB_SHA"]
    verify_remote_tag(tag, expected_commit)
    publisher = Publisher(
        os.environ["GITHUB_REPOSITORY"],
        os.environ["RELEASE_ID"],
        os.environ["RELEASE_UPLOAD_URL"],
    )
    paths = [Path(value) for value in argv]
    for path in paths:
        if not path.is_file():
            raise RuntimeError(f"release asset is missing: {path}")
    publisher.publish(paths)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main(sys.argv[1:]))
    except (KeyError, RuntimeError, subprocess.SubprocessError, json.JSONDecodeError) as error:
        print(f"release publication failed: {error}", file=sys.stderr)
        raise SystemExit(1)
