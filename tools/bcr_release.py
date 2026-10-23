#!/usr/bin/env python3
"""Resolve verified CI artifacts and guard manual BCR fork preparation."""

from __future__ import annotations

import argparse
import base64
from datetime import timedelta
import json
from pathlib import Path
import re
import subprocess
import tempfile
from urllib.parse import urlencode

from tools import bcr_source
from tools import release_artifact_publisher
from tools import release_candidate


REPOSITORY = "jwmcglynn/donner"
REGISTRY_FORK = "jwmcglynn/bazel-central-registry"
PREFLIGHT_PATH = ".github/workflows/bcr_preflight.yml"
RELEASE_PATH = ".github/workflows/release.yml"
CANDIDATE_PATH = ".github/workflows/release_candidate.yml"
CANDIDATE_ARTIFACT_PREFIX = "donner-release-candidate-"
SHA256 = re.compile(r"[0-9a-f]{64}")
STABLE_TAG = re.compile(r"v([0-9]+\.[0-9]+\.[0-9]+)")


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


def candidate_ref(release_body: str) -> tuple[str, str, str]:
    """Return the assembly run, attempt and record digest named by the release body.

    The candidate record is the only release authority. A body that still names a preflight
    attempt directly is rejected instead of choosing between two selections.
    """
    if re.search(r"(?m)^Release-Candidate-Preflight:", release_body):
        raise ValueError("release body names a preflight attempt; name the candidate record instead")
    lines = re.findall(r"(?m)^Release-Candidate-Record:.*$", release_body)
    matches = re.findall(
        r"(?m)^Release-Candidate-Record: ([1-9][0-9]{0,15})/([1-9][0-9]{0,4}) "
        r"sha256:([0-9a-f]{64})\r?$",
        release_body,
    )
    if len(lines) != 1 or len(matches) != 1:
        raise ValueError("release body must name exactly one candidate record run, attempt and digest")
    return matches[0]


def select_record(commit: str, tag: str, run_id: str, attempt: str,
                  record_sha256: str) -> dict[str, object]:
    """Select the retained record of one successful assembly attempt on the release commit."""
    version = check_source(commit, tag)
    return {**_select_record_artifact(commit, run_id, attempt, record_sha256), "version": version}


def _select_record_artifact(commit: str, run_id: str, attempt: str,
                            record_sha256: str) -> dict[str, object]:
    """Select one successful retained assembly artifact without reading an ambient checkout."""
    if not bcr_source.COMMIT.fullmatch(commit):
        raise ValueError("release source must be a full Git commit ID")
    if not re.fullmatch(r"[0-9a-f]{64}", record_sha256):
        raise ValueError("candidate record digest must be a lowercase SHA-256")
    run = gh_json("api", f"repos/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}")
    check_run(run, commit, CANDIDATE_PATH, {"workflow_dispatch"})
    if run.get("head_branch") != "main":
        raise ValueError("candidate record must be assembled on main")
    if run.get("id") != int(run_id) or run["run_attempt"] != int(attempt):
        raise ValueError("assembly run does not match the selected immutable attempt")
    name = f"{CANDIDATE_ARTIFACT_PREFIX}{run['run_attempt']}"
    pages = gh_json("api", f"repos/{REPOSITORY}/actions/runs/{run_id}/artifacts?per_page=100",
                    "--paginate", "--slurp")
    artifacts = [artifact for page in pages for artifact in page.get("artifacts", [])]
    matching = [item for item in artifacts if item.get("name") == name]
    if (len(matching) != 1 or matching[0].get("expired") is not False
            or type(matching[0].get("id")) is not int or matching[0]["id"] < 1
            or not isinstance(matching[0].get("size_in_bytes"), int)
            or matching[0]["size_in_bytes"] <= 0):
        raise ValueError("candidate record artifact is missing, ambiguous, empty or expired")
    return {"run_id": str(run["id"]), "attempt": str(run["run_attempt"]),
            "artifact_id": str(matching[0]["id"]), "record_sha256": record_sha256}


def check_release(release: dict, tag: str) -> None:
    if (release.get("tagName") != tag or release.get("isDraft") is not False
            or type(release.get("isPrerelease")) is not bool):
        raise ValueError("expected an existing published release")


def verify_published_source(commit: str, tag: str, release: dict, run_id: str,
                            attempt: str, record: dict) -> dict:
    """Verify the released source bytes and that they came from the named preflight attempt.

    `run_id` and `attempt` are the record's qualifying BCR Preflight attempt. The archive's
    provenance must name exactly that attempt, which must be a successful run on `main`.
    """
    version = bcr_source.module_values(bcr_source.git("show", f"{commit}:MODULE.bazel"))["version"]
    prefix = f"donner-{version}"
    names = {f"{prefix}.tar.gz", f"{prefix}.tar.gz.sha256"}
    assets = {asset["name"]: asset for asset in release["assets"]}
    if not names.issubset(assets):
        raise ValueError("verified source release assets are missing")
    if any(sum(asset.get("name") == name for asset in release["assets"]) != 1 for name in names):
        raise ValueError("source release assets have duplicate names")
    with tempfile.TemporaryDirectory() as temporary:
        directory = Path(temporary)
        retained = release_candidate.fetch_role(record, "source", Path.cwd(),
                                                 directory / "source.zip", directory / "retained")
        receipt = bcr_source.verify_archive(retained, commit)
        published = directory / "published"
        published.mkdir()
        for name in sorted(names):
            subprocess.run(["gh", "release", "download", tag, "--repo", REPOSITORY,
                            "--pattern", name, "--dir", str(published)], check=True)
            path = published / name
            if path.is_symlink() or not path.is_file() or \
                    bcr_source.digest(path) != bcr_source.digest(retained / name):
                raise ValueError("published source bytes differ from the retained candidate")
        if assets[f"{prefix}.tar.gz"].get("digest") != "sha256:" + receipt["sha256"]:
            raise ValueError("published source digest does not match its verified bytes")
    check_preflight_attempt(receipt, commit, run_id, attempt)
    return receipt


def check_preflight_attempt(receipt: dict, commit: str, run_id: str, attempt: str) -> None:
    """Require the archive's qualifying preflight to be the named successful attempt on main."""
    verified = (receipt.get("verified_run_id", ""), receipt.get("verified_run_attempt", ""))
    if any(not isinstance(value, str) or not re.fullmatch(r"[1-9][0-9]*", value)
           for value in verified):
        raise ValueError("source provenance is missing its CI run and attempt")
    if verified != (run_id, attempt):
        raise ValueError("source provenance names another preflight attempt than the record")
    run = gh_json("api", f"repos/{REPOSITORY}/actions/runs/{run_id}/attempts/{attempt}")
    check_run(run, commit, PREFLIGHT_PATH, {"push", "workflow_dispatch"})
    if run.get("head_branch") != "main":
        raise ValueError("source preflight must have run on main")
    if run.get("id") != int(run_id) or run["run_attempt"] != int(attempt):
        raise ValueError("source CI attempt does not match provenance")


def fork_contents(path: str, commit: str) -> bytes:
    value = gh_json("api", f"repos/{REGISTRY_FORK}/contents/{path}?ref={commit}")
    return base64.b64decode(value["content"], validate=False)


def verify_submission_metadata(head: str, commit: str, version: str) -> None:
    metadata = json.loads(fork_contents("modules/donner/metadata.json", head))
    template = json.loads(subprocess.check_output(
        ["git", "show", f"{commit}:.bcr/metadata.template.json"]))
    check_metadata(metadata, template, version)


# Shared with the candidate verifier, which checks the preflight entry's metadata the same way.
check_metadata = bcr_source.check_metadata
verify_yanked_metadata = bcr_source.verify_yanked_metadata


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
    if source != bcr_source.expected_source_json(receipt["version"], receipt["sha256"]):
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


def verify_metadata_history(registry: Path, metadata_file: Path, versions: list[str],
                            yanked: dict, template: dict, version: str) -> None:
    old_metadata = subprocess.check_output(
        ["git", "-C", str(registry), "ls-tree", "--name-only", "HEAD", "--",
         metadata_file.as_posix()])
    if old_metadata:
        previous = json.loads(subprocess.check_output(
            ["git", "-C", str(registry), "show", f"HEAD:{metadata_file.as_posix()}"]))
        if (versions != bcr_source.add_registry_version(previous["versions"], version)
                or yanked != previous["yanked_versions"]):
            raise ValueError("generated registry metadata changes existing versions, their order "
                             "or yanks")
    elif versions != [version] or yanked != template.get("yanked_versions", {}):
        raise ValueError("generated registry metadata has unexpected initial history")


METADATA_FILE = "modules/donner/metadata.json"


def submission_metadata(registry: Path, validated: Path, source: Path, version: str) -> bytes:
    """Derive the proposed `metadata.json` from the registry's HEAD and the maintainer template.

    The registry history may move after preflight: an earlier donner version can merge or be
    yanked. The proposal therefore adds this version to HEAD's history, in ascending version
    order, instead of reusing preflight's frozen copy, and preflight's copy must agree with it on
    every other field.
    """
    listed = subprocess.check_output(
        ["git", "-C", str(registry), "ls-tree", "--name-only", "HEAD", "--", METADATA_FILE])
    upstream = json.loads(subprocess.check_output(
        ["git", "-C", str(registry), "show", f"HEAD:{METADATA_FILE}"])) if listed else None
    template = json.loads((source / ".bcr/metadata.template.json").read_text(encoding="utf-8"))
    derived = bcr_source.derive_metadata(upstream, template, version)
    try:
        preflight = json.loads(bcr_source.entry_bytes(validated / "registry" / METADATA_FILE))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ValueError("validated registry entry has malformed JSON") from None
    bcr_source.check_validated_metadata(preflight, json.loads(derived), version)
    return derived


def check_proposed_files(proposed_bytes, registry: Path, validated: Path, source: Path,
                         version: str) -> None:
    """Require the validated entry's bytes and the derived metadata in the proposed change."""
    for path in bcr_source.entry_paths(version):
        if path.as_posix() == METADATA_FILE:
            expected = submission_metadata(registry, validated, source, version)
            message = "proposed registry metadata differs from the metadata derived for this " \
                      "submission"
        else:
            expected = bcr_source.entry_bytes(validated / "registry" / path)
            message = "proposed registry entry differs from the admission-validated entry"
        if proposed_bytes(Path(path)) != expected:
            raise ValueError(message)


def verify_generated_entry(registry: Path, source: Path, version: str, sha256: str,
                           validated: Path, proposed_commit: str | None = None) -> None:
    """Check the proposed registry change before any fork token is used.

    `MODULE.bazel`, `presubmit.yml` and `source.json` must be the bytes of `validated`, the entry
    BCR Preflight generated and admission-validated for the candidate record. `metadata.json`
    must be the file `submission_metadata` derives from the registry's HEAD. All of it must still
    match the approved archive, module files, maintainer metadata and existing registry history.
    """
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version) or not re.fullmatch(r"[0-9a-f]{64}", sha256):
        raise ValueError("generated entry needs an approved stable version and archive SHA-256")
    if proposed_commit is not None and not bcr_source.COMMIT.fullmatch(proposed_commit):
        raise ValueError("generated entry needs a full proposed commit SHA")
    root = Path("modules/donner")
    entry = root / version
    metadata_file = root / "metadata.json"
    required = {Path(path) for path in bcr_source.entry_paths(version)}
    diff_args = (["HEAD", proposed_commit] if proposed_commit else ["--cached"])
    changed_bytes = subprocess.check_output(
        ["git", "-C", str(registry), "diff", "--name-only", "-z", *diff_args])
    changed = {Path(name.decode()) for name in changed_bytes.split(b"\0") if name}
    if changed != required:
        raise ValueError("generated registry entry has missing or unexpected staged files")
    def proposed_bytes(path: Path) -> bytes:
        label = path.as_posix()
        mode_command = (["ls-tree", proposed_commit, "--", label] if proposed_commit else
                        ["ls-files", "--stage", "--", label])
        mode = subprocess.check_output(["git", "-C", str(registry), *mode_command], text=True)
        if not mode.startswith("100644 ") or not mode.endswith(f"\t{label}\n"):
            raise ValueError("generated registry entry contains a missing or linked file")
        ref = proposed_commit if proposed_commit else ""
        return subprocess.check_output(["git", "-C", str(registry), "show", f"{ref}:{label}"])

    check_proposed_files(proposed_bytes, registry, validated, source, version)
    if json.loads(proposed_bytes(entry / "source.json")) != \
            bcr_source.expected_source_json(version, sha256):
        raise ValueError("generated registry entry does not match the approved source archive")
    for generated, original in (("MODULE.bazel", "MODULE.bazel"),
                                ("presubmit.yml", ".bcr/presubmit.yml")):
        if proposed_bytes(entry / generated) != (source / original).read_bytes():
            raise ValueError("generated registry entry differs from the approved module files")
    metadata = json.loads(proposed_bytes(metadata_file))
    template = json.loads((source / ".bcr/metadata.template.json").read_text(encoding="utf-8"))
    versions, yanked = check_metadata(metadata, template, version)
    verify_metadata_history(registry, metadata_file, versions, yanked, template, version)


# The identity of the proposed registry commit. Hooks and signing stay off so runner
# configuration cannot change the committed bytes.
SUBMISSION_COMMIT_CONFIG = ("-c", "core.hooksPath=/dev/null", "-c", "commit.gpgsign=false",
                            "-c", "user.name=Donner release",
                            "-c", "user.email=donner-release@users.noreply.github.com")


def submission_branch(tag: str, version: str) -> str:
    """Return the registry branch `donner-<tag>` of a stable tag that names `version`."""
    match = STABLE_TAG.fullmatch(tag)
    if not match or match.group(1) != version:
        raise ValueError("submission branch needs the stable release tag of the version")
    return f"donner-{tag}"


def _registry_git(registry: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(registry), *args], check=True)


def _registry_value(registry: Path, *args: str) -> str:
    return subprocess.check_output(["git", "-C", str(registry), *args], text=True).strip()


def bundle_entry(registry: Path, source: Path, tag: str, version: str, approved_sha256: str,
                 validated: Path, bundle: Path) -> dict[str, str]:
    """Verify the staged entry, commit it on the submission branch and bundle that one commit.

    Runs without a fork token. The staged change must pass `verify_generated_entry` before
    anything is committed. The bundle holds only the submission branch's commit on top of the
    registry HEAD it was staged on; the returned base, commit, tree and bundle digest let later
    jobs check they received exactly this commit.
    """
    branch = submission_branch(tag, version)
    verify_generated_entry(registry, source, version, approved_sha256, validated)
    tree = _registry_value(registry, "write-tree")
    base = _registry_value(registry, "rev-parse", "HEAD")
    _registry_git(registry, "checkout", "-b", branch)
    _registry_git(registry, *SUBMISSION_COMMIT_CONFIG, "commit", "-m", f"Publish donner@{version}")
    commit = _registry_value(registry, "rev-parse", "HEAD")
    if _registry_value(registry, "rev-parse", "HEAD^{tree}") != tree:
        raise ValueError("proposed registry commit does not hold the verified tree")
    _registry_git(registry, "bundle", "create", str(bundle), f"refs/heads/{branch}", f"^{base}")
    _registry_git(registry, "bundle", "verify", str(bundle))
    return {"base": base, "commit": commit, "tree": tree,
            "bundle_sha256": bcr_source.digest(bundle)}


def verify_bundle(registry: Path, source: Path, tag: str, version: str, approved_sha256: str,
                  validated: Path, bundle: Path, bundle_sha256: str, base: str, commit: str,
                  tree: str) -> None:
    """Check a downloaded entry bundle is the promoted commit before any fork token is used.

    `registry` is a checkout of the promoted base. The bundle must have the promotion job's
    digest, and its submission branch must be exactly `commit`, whose parent is `base` and
    whose tree is `tree`. That commit must then pass `verify_generated_entry` again.
    """
    branch = submission_branch(tag, version)
    if bcr_source.digest(bundle) != bundle_sha256:
        raise ValueError("proposed entry bundle differs from the bundle the promotion retained")
    print(f"{bundle}: OK")
    if _registry_value(registry, "rev-parse", "HEAD") != base:
        raise ValueError("registry checkout is not the promoted base")
    _registry_git(registry, "bundle", "verify", str(bundle))
    _registry_git(registry, "fetch", "--no-tags", str(bundle), f"refs/heads/{branch}")
    for revision, expected, label in (("FETCH_HEAD", commit, "commit"),
                                      ("FETCH_HEAD^", base, "base"),
                                      ("FETCH_HEAD^{tree}", tree, "tree")):
        if _registry_value(registry, "rev-parse", revision) != expected:
            raise ValueError(f"proposed entry bundle has another {label} than the promotion")
    verify_generated_entry(registry, source, version, approved_sha256, validated, commit)


def published_record(tag: str, version: str, record_sha256: str, release: dict,
                     directory: Path, commit: str) -> tuple[Path, dict]:
    """Fetch the retained assembly record named by the published release body."""
    if tag != f"v{version}":
        raise ValueError("release tag does not match the module version")
    run_id, attempt, digest = candidate_ref(release.get("body", ""))
    if digest != record_sha256:
        raise ValueError("release body names a different candidate record than the approved one")
    selected = _select_record_artifact(commit, run_id, attempt, digest)
    release_candidate._empty_directory(directory, "candidate record work")
    _, record, record_dir = release_candidate._fetch_record(
        int(selected["artifact_id"]), int(run_id), int(attempt), commit, digest,
        "release body", directory, release_candidate._github_json,
        release_candidate._download_artifact, minimum=timedelta(0))
    if record["source"].get("version") != version or record["source"].get("commit") != commit:
        raise ValueError("candidate record names another version or commit than the release")
    return record_dir / "candidate.json", record


def verify_record_attestation(path: Path, commit: str) -> None:
    """Require the record to be the one `Assemble release candidate` attested for this commit."""
    subprocess.run(["gh", "attestation", "verify", str(path), "--repo", REPOSITORY,
                    "--signer-workflow", f"{REPOSITORY}/{CANDIDATE_PATH}",
                    "--source-digest", commit, "--source-ref", "refs/heads/main"],
                   check=True, stdout=subprocess.DEVNULL)


def record_source(record: dict, commit: str, version: str) -> tuple[str, str, str]:
    """Return the record's approved archive SHA-256 and its preflight run and attempt."""
    if record["source"].get("commit") != commit:
        raise ValueError("candidate record names another commit than the release")
    preflight = {(record["artifacts"][role].get("run_id"), record["artifacts"][role].get("attempt"))
                 for role in release_candidate.PREFLIGHT_ROLES}
    if len(preflight) != 1:
        raise ValueError("candidate record source and registry entry come from different "
                         "preflight attempts")
    source = record["artifacts"]["source"]
    archive = source["files"].get(f"donner-{version}.tar.gz")
    if not isinstance(archive, str) or not SHA256.fullmatch(archive):
        raise ValueError("candidate record has no approved source archive digest")
    return archive, str(source["run_id"]), str(source["attempt"])


def plan_submission(release_run_id: str, record_sha256: str, work: Path) -> dict[str, object]:
    """Check a published release against its candidate record before BCR fork preparation.

    The maintainer approves a Release run and the record digest. The release body must name that
    record, its retained artifact must match, and it must be attested by the assembly
    workflow. The approved source digest and the preflight attempt come from the record.
    """
    if not re.fullmatch(r"[1-9][0-9]*", release_run_id):
        raise ValueError("release workflow run ID must be numeric")
    if not SHA256.fullmatch(record_sha256):
        raise ValueError("BCR submission needs the approved candidate record SHA-256")
    run = gh_json("api", f"repos/{REPOSITORY}/actions/runs/{release_run_id}")
    commit = run.get("head_sha", "")
    check_run(run, commit, RELEASE_PATH, {"release"})
    if not bcr_source.COMMIT.fullmatch(commit):
        raise ValueError("Release run has no full source commit")
    version = bcr_source.module_values(bcr_source.git("show", f"{commit}:MODULE.bazel"))["version"]
    tag = f"v{version}"
    release = gh_json("release", "view", tag, "--repo", REPOSITORY,
                      "--json", "tagName,isDraft,isPrerelease,assets,body")
    check_release(release, tag)
    if release.get("isPrerelease") or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        return {"prepare": "false", "reason": "BCR submission is limited to stable releases"}
    check_source(commit, tag)
    body = release.get("body")
    if not isinstance(body, str) or candidate_ref(body)[2] != record_sha256:
        raise ValueError("release body names a different candidate record than the approved one")
    path, record = published_record(tag, version, record_sha256, release, work, commit)
    verify_record_attestation(path, commit)
    approved_sha256, run_id, attempt = record_source(record, commit, version)
    receipt = verify_published_source(commit, tag, release, run_id, attempt, record)
    if receipt.get("sha256") != approved_sha256:
        raise ValueError("published source digest differs from the candidate record")
    existing = existing_submission(tag, commit, receipt)
    return {"prepare": "false" if existing else "true", "tag": tag,
            "version": version, "existing_pr": existing, "source_commit": commit,
            "source_sha256": approved_sha256, "record_sha256": record_sha256}


def fetch_entry(tag: str, record_sha256: str, source: Path, work: Path) -> Path:
    """Download and verify the admission-validated registry entry the release's record names.

    `source` is a checkout of the record's commit. The entry is the record's `bcr-entry` role,
    downloaded by artifact ID and checked against its recorded digests, the approved archive and
    the source's module files. It is returned as `work/entry`.
    """
    match = STABLE_TAG.fullmatch(tag)
    if not match or not SHA256.fullmatch(record_sha256):
        raise ValueError("entry selection needs a stable release tag and the record SHA-256")
    version = match.group(1)
    commit = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"],
                                     text=True).strip()
    release = gh_json("release", "view", tag, "--repo", REPOSITORY,
                      "--json", "tagName,isDraft,isPrerelease,assets,body")
    check_release(release, tag)
    path, record = published_record(tag, version, record_sha256, release, work / "record", commit)
    verify_record_attestation(path, commit)
    record_source(record, commit, version)
    return release_candidate.fetch_role(record, "bcr-entry", source, work / "bcr-entry.zip",
                                        work / "entry")


def stage_entry(entry: Path, registry: Path, source: Path, version: str) -> None:
    """Stage the validated entry's files and the derived metadata in a registry checkout."""
    target_version = registry / "modules/donner" / version
    if target_version.exists() or target_version.is_symlink():
        raise ValueError("the registry already has this donner version")
    paths = bcr_source.entry_paths(version)
    metadata = submission_metadata(registry, entry, source, version)
    for path in paths:
        data = (metadata if path.as_posix() == METADATA_FILE else
                bcr_source.entry_bytes(entry / "registry" / path))
        target = registry / path
        if target.is_symlink() or (target.exists() and not target.is_file()):
            raise ValueError("registry entry target is not a regular file")
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
    subprocess.run(["git", "-C", str(registry), "add", "--", *(path.as_posix() for path in paths)],
                   check=True)


def _entry_check_arguments(command: argparse.ArgumentParser) -> None:
    """The arguments of every command that checks a proposed entry with `verify_generated_entry`."""
    command.add_argument("--registry", type=Path, required=True)
    command.add_argument("--source", type=Path, required=True)
    command.add_argument("--version", required=True)
    command.add_argument("--approved-sha256", required=True)
    command.add_argument("--validated-entry", type=Path, required=True)


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    select = commands.add_parser("select-record")
    select.add_argument("--commit", required=True)
    select.add_argument("--tag", required=True)
    select.add_argument("--release-body-file", type=Path, required=True)
    select.add_argument("--github-output")
    plan = commands.add_parser("plan-submission")
    plan.add_argument("--release-run-id", required=True)
    plan.add_argument("--record-sha256", required=True)
    plan.add_argument("--work", type=Path, required=True)
    plan.add_argument("--github-output")
    fetch = commands.add_parser("fetch-entry")
    fetch.add_argument("--tag", required=True)
    fetch.add_argument("--record-sha256", required=True)
    fetch.add_argument("--source", type=Path, required=True)
    fetch.add_argument("--work", type=Path, required=True)
    stage = commands.add_parser("stage-entry")
    stage.add_argument("--entry", type=Path, required=True)
    stage.add_argument("--registry", type=Path, required=True)
    stage.add_argument("--source", type=Path, required=True)
    stage.add_argument("--version", required=True)
    generated = commands.add_parser("verify-generated")
    _entry_check_arguments(generated)
    generated.add_argument("--proposed-commit")
    bundled = commands.add_parser(
        "bundle-entry", help="check, commit and bundle the staged entry without a fork token")
    _entry_check_arguments(bundled)
    bundled.add_argument("--tag", required=True)
    bundled.add_argument("--bundle", type=Path, required=True)
    bundled.add_argument("--github-output")
    rechecked = commands.add_parser(
        "verify-bundle", help="check a downloaded entry bundle is the promoted commit")
    _entry_check_arguments(rechecked)
    rechecked.add_argument("--tag", required=True)
    rechecked.add_argument("--bundle", type=Path, required=True)
    rechecked.add_argument("--bundle-sha256", required=True)
    rechecked.add_argument("--base", required=True)
    rechecked.add_argument("--tree", required=True)
    rechecked.add_argument("--proposed-commit", required=True)
    return parser


def _select_record_command(args: argparse.Namespace) -> dict[str, object]:
    run_id, attempt, record_sha256 = candidate_ref(
        args.release_body_file.read_text(encoding="utf-8"))
    return select_record(args.commit, args.tag, run_id, attempt, record_sha256)


def _fetch_entry_command(args: argparse.Namespace) -> None:
    print(f"Verified the admission-validated registry entry in "
          f"{fetch_entry(args.tag, args.record_sha256, args.source, args.work)}")


def _verify_bundle_command(args: argparse.Namespace) -> None:
    verify_bundle(args.registry, args.source, args.tag, args.version, args.approved_sha256,
                  args.validated_entry, args.bundle, args.bundle_sha256, args.base,
                  args.proposed_commit, args.tree)


# Each command returns its workflow outputs, or None when it has none.
COMMANDS = {
    "select-record": _select_record_command,
    "plan-submission": lambda args: plan_submission(args.release_run_id, args.record_sha256,
                                                    args.work),
    "fetch-entry": _fetch_entry_command,
    "stage-entry": lambda args: stage_entry(args.entry, args.registry, args.source,
                                            args.version),
    "verify-generated": lambda args: verify_generated_entry(
        args.registry, args.source, args.version, args.approved_sha256, args.validated_entry,
        args.proposed_commit),
    "bundle-entry": lambda args: bundle_entry(args.registry, args.source, args.tag, args.version,
                                              args.approved_sha256, args.validated_entry,
                                              args.bundle),
    "verify-bundle": _verify_bundle_command,
}


def main() -> None:
    args = _parser().parse_args()
    result = COMMANDS[args.command](args)
    if result is not None:
        bcr_source.outputs(result, args.github_output)


if __name__ == "__main__":
    main()
