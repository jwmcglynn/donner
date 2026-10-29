#!/usr/bin/env python3
"""Run upstream BCR admission checks using a not-yet-published local archive."""

from __future__ import annotations

import argparse
from collections.abc import Callable, Mapping
import importlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
from urllib.parse import quote

from tools import bcr_source
from tools.ci import github_rate_limit


def validate(upstream: Path, registry: Path, artifacts: Path, commit: str, report: Path) -> int:
    receipt = bcr_source.verify_archive(artifacts, commit)
    version = receipt["version"]
    source = json.loads((registry / f"modules/donner/{version}/source.json").read_text())
    if source["url"] != bcr_source.source_url(version):
        raise ValueError("BCR entry must retain its canonical stable release asset URL")
    root = subprocess.check_output(
        ["git", "-C", str(upstream), "rev-parse", "--show-toplevel"], text=True).strip()
    if Path(root).resolve() != upstream.resolve():
        raise ValueError("BCR tools must come from their own repository checkout")
    tools = (upstream / "tools").resolve()
    sys.path.insert(0, str(tools))
    try:
        validation = importlib.import_module("bcr_validation")
        registry_api = importlib.import_module("registry")
        if Path(validation.__file__).resolve() != tools / "bcr_validation.py":
            raise ValueError("a different BCR validator is already loaded")
        return run_validator(validation, registry_api, upstream, registry,
                             artifacts / receipt["archive"], version, source["url"], report)
    finally:
        sys.path.remove(str(tools))


def prime_maintainer_ids(validation, registry: Path, environ: Mapping[str, str] = os.environ,
                         client=None, retry: Callable = github_rate_limit.retry_rate_limited) -> None:
    """Looks up each maintainer's GitHub user ID for the validator, waiting out the rate limit.

    The validator compares every maintainer's `github_user_id` with GitHub's user lookup for
    that login and reports a refused lookup as a bad username. The workflow token shares the
    repository's hourly budget with every other workflow, so the same lookups are made here
    first, with github_rate_limit's bounded wait, and stored in the validator's own cache; its
    comparison is unchanged. A lookup that fails for another reason is left to the validator,
    which reports it as before. An exhausted cap fails the step (RateLimitExhausted).
    """
    cache = getattr(validation, "GITHUB_USER_ID_CACHE", None)
    if not isinstance(cache, dict):
        print("The BCR validator has no GitHub user ID cache; it performs its own lookups.")
        return
    metadata = json.loads((registry / "modules/donner/metadata.json").read_text())
    api = environ.get("GITHUB_API_URL") or "https://api.github.com"
    client = client or github_rate_limit.ApiClient(environ.get("GITHUB_TOKEN", ""))
    for maintainer in metadata.get("maintainers", []):
        login = maintainer.get("github")
        if not isinstance(login, str) or not login or login in cache:
            continue
        url = "/".join((api, "users", quote(login, safe="")))
        try:
            user = retry(lambda: client.get_json(url), label=f"BCR maintainer lookup ({login})")
        except github_rate_limit.ApiError as error:
            print(f"Maintainer lookup for {login} failed ({error}); the validator repeats it.")
            continue
        if isinstance(user, dict) and isinstance(user.get("id"), int):
            cache[login] = user["id"]


def run_validator(validation, registry_api, upstream: Path, registry: Path, archive: Path,
                  version: str, source_url: str, report: Path,
                  prime: Callable = prime_maintainer_ids) -> int:
    prime(validation, registry)
    original_download = validation.download
    original_download_file = validation.download_file

    def download(url):
        return archive.read_bytes() if url == source_url else original_download(url)

    def download_file(url, destination):
        if url == source_url:
            shutil.copyfile(archive, destination)
        else:
            original_download_file(url, destination)

    validation.download = download
    validation.download_file = download_file
    try:
        validator = validation.BcrValidator(
            registry_api.RegistryClient(str(registry)),
            registry_api.UpstreamRegistry(modules_dir_url=validation.UPSTREAM_MODULES_DIR_URL),
            False,
        )
        validator.validate_module("donner", version, [])
        validator.validate_metadata(["donner"])
        validator.global_checks()
        status = validator.getValidationReturnCode()
        results = [{"result": kind.name, "message": message}
                   for kind, message in validator.validation_results]
    finally:
        validation.download = original_download
        validation.download_file = original_download_file
    revision = subprocess.check_output(["git", "-C", str(upstream), "rev-parse", "HEAD"], text=True).strip()
    summary = {
        "validator_revision": revision, "module": f"donner@{version}",
        "source_transport": "local candidate bytes; canonical release URL policy is checked",
        "published_url_availability": "verified by the release workflow before BCR submission",
        "upstream_exit_code": status, "results": results,
    }
    report.write_text(json.dumps(summary, indent=2) + "\n")
    if status == 42:
        print("BCR admission requires maintainer review before its build matrix can run.")
    return 0 if status in (0, 42) else 1


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--registry", type=Path, required=True)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--commit", required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    return validate(args.upstream, args.registry, args.artifacts, args.commit, args.report)


if __name__ == "__main__":
    raise SystemExit(main())
