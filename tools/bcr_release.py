#!/usr/bin/env python3
"""Resolve verified CI artifacts and guard automatic BCR submission."""

from __future__ import annotations

import argparse
import base64
import json
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import urlencode

from tools import bcr_source
from tools import release_artifact_publisher


REPOSITORY = "jwmcglynn/donner"
REGISTRY_FORK = "jwmcglynn/bazel-central-registry"
PREFLIGHT_PATH = ".github/workflows/bcr_preflight.yml"
RELEASE_PATH = ".github/workflows/release.yml"


def gh_json(*args: str):
    return json.loads(subprocess.check_output(["gh", *args], text=True))


def check_run(run: dict, commit: str, path: str, events: set[str]) -> None:
    if (run.get("head_sha") != commit or run.get("path") != path
            or run.get("event") not in events or run.get("status") != "completed"
            or run.get("conclusion") != "success"
            or run.get("repository", {}).get("full_name") != REPOSITORY):
        raise ValueError("workflow run is not a successful build of the approved source")
    if type(run.get("run_attempt")) is not int or run["run_attempt"] < 1:
        raise ValueError("workflow run has an invalid attempt")


def check_source(commit: str, tag: str) -> str:
    if not bcr_source.COMMIT.fullmatch(commit):
        raise ValueError("release source must be a full Git commit ID")
    version = bcr_source.module_values(bcr_source.git("show", f"{commit}:MODULE.bazel"))["version"]
    if tag != f"v{version}":
        raise ValueError("release tag does not match the module version")
    subprocess.run(["git", "merge-base", "--is-ancestor", commit, "origin/main"], check=True)
    release_artifact_publisher.verify_remote_tag(tag, commit)
    return version


def candidate_ref(release_body: str) -> tuple[str, str]:
    matches = re.findall(
        r"(?m)^Release-Candidate-Preflight: ([1-9][0-9]*)/([1-9][0-9]*)$",
        release_body,
    )
    if len(matches) != 1:
        raise ValueError("release body must name exactly one preflight run and attempt")
    return matches[0]


def select_preflight(commit: str, tag: str, run_id: str, attempt: str) -> dict[str, object]:
    version = check_source(commit, tag)
    if not re.fullmatch(r"[1-9][0-9]*", run_id) or not re.fullmatch(r"[1-9][0-9]*", attempt):
        raise ValueError("release candidate run and attempt must be numeric")
    run = gh_json("api", f"repos/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}")
    check_run(run, commit, PREFLIGHT_PATH, {"push", "workflow_dispatch"})
    if run.get("head_branch") != "main":
        raise ValueError("release preflight must run on main")
    if run.get("id") != int(run_id) or run["run_attempt"] != int(attempt):
        raise ValueError("preflight run does not match the selected immutable attempt")
    names = {
        "artifact": f"donner-bcr-qualified-{run['run_attempt']}",
        "linux_artifact": f"donner-svg-linux-x86-64-{commit}-{run['run_attempt']}",
        "macos_artifact": f"donner-svg-darwin-arm64-{commit}-{run['run_attempt']}",
    }
    pages = gh_json("api", f"repos/{REPOSITORY}/actions/runs/{run_id}/artifacts?per_page=100",
                    "--paginate", "--slurp")
    artifacts = [artifact for page in pages for artifact in page.get("artifacts", [])]
    for name in names.values():
        matching = [item for item in artifacts if item.get("name") == name]
        if (len(matching) != 1 or matching[0].get("expired") is not False
                or not isinstance(matching[0].get("size_in_bytes"), int)
                or matching[0]["size_in_bytes"] <= 0):
            raise ValueError("preflight release artifact is missing, ambiguous, empty or expired")
    return {"run_id": str(run["id"]), "attempt": str(run["run_attempt"]),
            "version": version, **names}


def check_release(release: dict, tag: str) -> None:
    if (release.get("tagName") != tag or release.get("isDraft") is not False
            or type(release.get("isPrerelease")) is not bool):
        raise ValueError("expected an existing published release")


def verify_published_source(commit: str, tag: str, release: dict) -> dict:
    version = bcr_source.module_values(bcr_source.git("show", f"{commit}:MODULE.bazel"))["version"]
    prefix = f"donner-{version}"
    names = {f"{prefix}.tar.gz", f"{prefix}.tar.gz.sha256", f"{prefix}.provenance.json"}
    assets = {asset["name"]: asset for asset in release["assets"]}
    if not names.issubset(assets):
        raise ValueError("verified source release assets are missing")
    if any(sum(asset.get("name") == name for asset in release["assets"]) != 1 for name in names):
        raise ValueError("source release assets have duplicate names")
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        for name in sorted(names):
            subprocess.run(["gh", "release", "download", tag, "--repo", REPOSITORY,
                            "--pattern", name, "--dir", str(directory)], check=True)
        receipt = bcr_source.verify_archive(directory, commit)
        if assets[f"{prefix}.tar.gz"].get("digest") != "sha256:" + receipt["sha256"]:
            raise ValueError("published source digest does not match its verified bytes")
    run_id = receipt.get("verified_run_id", "")
    attempt = receipt.get("verified_run_attempt", "")
    if not re.fullmatch(r"[1-9][0-9]*", str(run_id)) or not re.fullmatch(r"[1-9][0-9]*", str(attempt)):
        raise ValueError("source provenance is missing its CI run and attempt")
    run = gh_json("api", f"repos/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}")
    check_run(run, commit, PREFLIGHT_PATH, {"push", "workflow_dispatch"})
    if run["run_attempt"] != int(attempt):
        raise ValueError("source CI attempt does not match provenance")
    return receipt


def fork_contents(path: str, commit: str) -> bytes:
    value = gh_json("api", f"repos/{REGISTRY_FORK}/contents/{path}?ref={commit}")
    return base64.b64decode(value["content"], validate=False)


def verify_submission_metadata(head: str, commit: str, version: str) -> None:
    metadata = json.loads(fork_contents("modules/donner/metadata.json", head))
    template = json.loads(subprocess.check_output(
        ["git", "show", f"{commit}:.bcr/metadata.template.json"]))
    versions = metadata.pop("versions", None)
    yanked = metadata.pop("yanked_versions", None)
    template.pop("versions", None)
    expected_yanked = template.pop("yanked_versions", {})
    if metadata != template:
        raise ValueError("existing registry metadata has different project or maintainer fields")
    if (not isinstance(versions, list) or not all(isinstance(item, str) for item in versions)
            or len(set(versions)) != len(versions) or versions.count(version) != 1):
        raise ValueError("existing registry metadata has a missing or invalid version list")
    verify_yanked_metadata(yanked, expected_yanked, version)


def verify_yanked_metadata(yanked, expected_yanked: dict, version: str) -> None:
    if (not isinstance(yanked, dict)
            or any(not isinstance(value, str) for value in yanked.values())
            or any(yanked.get(key) != value for key, value in expected_yanked.items())
            or yanked.get(version) != expected_yanked.get(version)):
        raise ValueError("existing registry metadata has different yanked versions")


def existing_submission(tag: str, commit: str, receipt: dict) -> str | None:
    branch = f"donner-{tag}"
    refs = gh_json("api", f"repos/{REGISTRY_FORK}/git/matching-refs/heads/{branch}")
    matches = [ref for ref in refs if ref["ref"] == f"refs/heads/{branch}"]
    if not matches:
        return None
    if len(matches) != 1:
        raise ValueError("ambiguous registry submission branch")
    head = matches[0]["object"]["sha"]
    if not bcr_source.COMMIT.fullmatch(head):
        raise ValueError("invalid registry submission commit")
    root = f"modules/donner/{receipt['version']}"
    source = json.loads(fork_contents(f"{root}/source.json", head))
    expected = {
        "integrity": "sha256-" + base64.b64encode(bytes.fromhex(receipt["sha256"])).decode(),
        "strip_prefix": f"donner-{receipt['version']}",
        "url": bcr_source.source_url(receipt["version"]),
    }
    if source != expected:
        raise ValueError("existing registry branch has different source metadata; inspect it manually")
    for file, original in [("MODULE.bazel", "MODULE.bazel"), ("presubmit.yml", ".bcr/presubmit.yml")]:
        expected_bytes = subprocess.check_output(["git", "show", f"{commit}:{original}"])
        if fork_contents(f"{root}/{file}", head) != expected_bytes:
            raise ValueError("existing registry branch has different module or test files")
    verify_submission_metadata(head, commit, receipt["version"])
    query = urlencode({"head": f"jwmcglynn:{branch}", "state": "all"})
    prs = gh_json("api", f"repos/bazelbuild/bazel-central-registry/pulls?{query}")
    matching = [pr for pr in prs if pr["head"]["sha"] == head
                and pr["head"].get("repo", {}).get("full_name") == REGISTRY_FORK
                and (pr["state"] == "open" or pr.get("merged_at"))]
    if len(matching) != 1:
        raise ValueError("existing registry branch needs manual PR recovery; it will not be overwritten")
    return matching[0]["html_url"]


def require_non_force_rule(branch: str) -> None:
    rules = gh_json("api", f"repos/{REGISTRY_FORK}/rules/branches/{branch}?per_page=100")
    if not isinstance(rules, list) or not any(
        isinstance(rule, dict) and rule.get("type") == "non_fast_forward" for rule in rules
    ):
        raise ValueError("BCR fork release branches need active non-fast-forward protection")


def plan_submission(release_run_id: str, approved_commit: str,
                    approved_sha256: str) -> dict[str, object]:
    if not re.fullmatch(r"[1-9][0-9]*", release_run_id):
        raise ValueError("release workflow run ID must be numeric")
    if not bcr_source.COMMIT.fullmatch(approved_commit) or not re.fullmatch(r"[0-9a-f]{64}", approved_sha256):
        raise ValueError("BCR submission needs an approved source commit and archive SHA-256")
    run = gh_json("api", f"repos/{REPOSITORY}/actions/runs/{release_run_id}")
    commit = run.get("head_sha", "")
    check_run(run, commit, RELEASE_PATH, {"release"})
    if commit != approved_commit:
        raise ValueError("Release source commit differs from the approved BCR submission")
    version = bcr_source.module_values(bcr_source.git("show", f"{commit}:MODULE.bazel"))["version"]
    tag = f"v{version}"
    release = gh_json("release", "view", tag, "--repo", REPOSITORY,
                      "--json", "tagName,isDraft,isPrerelease,assets")
    check_release(release, tag)
    if release.get("isPrerelease") or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        return {"prepare": "false", "reason": "BCR submission is limited to stable releases"}
    check_source(commit, tag)
    receipt = verify_published_source(commit, tag, release)
    if receipt.get("sha256") != approved_sha256:
        raise ValueError("published source digest differs from the approved BCR submission")
    existing = existing_submission(tag, commit, receipt)
    if not existing:
        require_non_force_rule(f"donner-{tag}")
    return {"prepare": "false" if existing else "true", "tag": tag,
            "existing_pr": existing, "source_commit": commit}


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    select = commands.add_parser("select-preflight")
    select.add_argument("--commit", required=True)
    select.add_argument("--tag", required=True)
    select.add_argument("--release-body-file", type=Path, required=True)
    select.add_argument("--github-output")
    plan = commands.add_parser("plan-submission")
    plan.add_argument("--release-run-id", required=True)
    plan.add_argument("--approved-commit", required=True)
    plan.add_argument("--approved-sha256", required=True)
    plan.add_argument("--github-output")
    args = parser.parse_args()
    if args.command == "select-preflight":
        run_id, attempt = candidate_ref(args.release_body_file.read_text(encoding="utf-8"))
        result = select_preflight(args.commit, args.tag, run_id, attempt)
    else:
        result = plan_submission(args.release_run_id, args.approved_commit, args.approved_sha256)
    bcr_source.outputs(result, args.github_output)


if __name__ == "__main__":
    main()
