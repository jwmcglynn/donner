#!/usr/bin/env python3
"""Create a notarized derivative of an exact published editor ZIP without publishing it."""

from __future__ import annotations

import argparse
import base64
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import secrets
import shutil
import subprocess
import zipfile

from tools import release_macos_editor as native

REPOSITORY = "jwmcglynn/donner"
ARCHIVE = native.ARCHIVE
BACKFILL = {
    "tag": "v0.8.0",
    "commit": "ebf35396d995d9df1d700ffa5913d876cc8dfdcf",
    "release_id": 421423815,
    "archive_id": 667858064,
    "checksum_id": 667858113,
    "archive_sha256": "fb9aff3d7656fd9362a7e74fe1855ede63ac283560c019b4c1fd4fbc31198e0c",
    "checksum_sha256": "557e995c9ac0d7f26332d8bdc6f3625f08e44ecc6b57abcd0ab2be3513ac9b9e",
    "candidate_record": "Release-Candidate-Record: 39882883140/1 "
                        "sha256:9efb9bc0829a392e8835766a6f24d3eb320989a055cc737735065d5d9d9ab1c1",
}
SECRET_NAMES = ("MACOS_CERTIFICATE_BASE64", "MACOS_CERTIFICATE_PASSWORD", "APPLE_ID",
                "APPLE_APP_SPECIFIC_PASSWORD", "APPLE_TEAM_ID")


def run(*args: str, timeout: int = 120) -> str:
    """Run a platform tool without echoing credential-bearing arguments on failure."""
    try:
        result = subprocess.run(args, check=True, capture_output=True, timeout=timeout)
    except (OSError, subprocess.SubprocessError):
        raise ValueError("signing platform command failed: " + Path(args[0]).name) from None
    if len(result.stdout) + len(result.stderr) > native.MAX_CONTROL_BYTES:
        raise ValueError("signing platform command output exceeds its limit")
    return (result.stdout + result.stderr).decode("utf-8")


def api(path: str) -> dict:
    value = json.loads(run("gh", "api", "repos/" + REPOSITORY + "/" + path))
    if not isinstance(value, dict):
        raise ValueError("GitHub metadata is not an object")
    return value


def check_inputs(tag: str, commit: str, digest: str) -> None:
    if not re.fullmatch(r"v[0-9]+\.[0-9]+\.[0-9]+", tag):
        raise ValueError("a stable release tag is required")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        raise ValueError("a full release source commit is required")
    if not re.fullmatch(r"[0-9a-f]{64}", digest):
        raise ValueError("the expected existing ZIP SHA-256 is required")


def release_assets(release: dict, tag: str, digest: str) -> dict:
    if release.get("tag_name") != tag or release.get("draft") or release.get("immutable"):
        raise ValueError("release identity or mutability differs from the requested release")
    selected = {}
    for name in (ARCHIVE, ARCHIVE + ".sha256"):
        matches = [a for a in release.get("assets", []) if a.get("name") == name]
        if len(matches) != 1:
            raise ValueError("release must contain exactly one editor ZIP and checksum")
        asset = matches[0]
        limit = native.MAX_ARCHIVE_BYTES if name == ARCHIVE else 1024
        if (type(asset.get("id")) is not int or asset["id"] <= 0
                or type(asset.get("size")) is not int or not 0 < asset["size"] <= limit
                or not re.fullmatch(r"sha256:[0-9a-f]{64}", asset.get("digest", ""))):
            raise ValueError("release asset has invalid identity, size or digest")
        selected[name] = {key: asset[key] for key in ("id", "name", "size", "digest")}
    if selected[ARCHIVE]["digest"] != "sha256:" + digest:
        raise ValueError("existing ZIP does not match the approved digest")
    return selected


def check_backfill(release: dict, assets: dict, tag: str, commit: str, digest: str) -> None:
    expected = (BACKFILL["tag"], BACKFILL["commit"], BACKFILL["archive_sha256"])
    if (tag, commit, digest) != expected or release.get("id") != BACKFILL["release_id"]:
        raise ValueError("only the source-pinned release backfill is authorized")
    if (assets[ARCHIVE]["id"] != BACKFILL["archive_id"]
            or assets[ARCHIVE + ".sha256"]["id"] != BACKFILL["checksum_id"]
            or assets[ARCHIVE + ".sha256"]["digest"] != "sha256:" + BACKFILL["checksum_sha256"]):
        raise ValueError("published asset identities differ from the reviewed backfill")
    records = re.findall(r"(?m)^Release-Candidate-Record:.*$", release.get("body", ""))
    if records != [BACKFILL["candidate_record"]]:
        raise ValueError("release candidate record selection changed")


def download(asset: dict, destination: Path) -> None:
    endpoint = f"repos/{REPOSITORY}/releases/assets/{asset['id']}"
    with destination.open("xb") as output:
        subprocess.run(["gh", "api", endpoint, "-H", "Accept: application/octet-stream"],
                       stdout=output, check=True, timeout=120)
    if (destination.stat().st_size != asset["size"]
            or "sha256:" + native.release_cli.digest(destination) != asset["digest"]):
        raise ValueError("downloaded release bytes differ from the selected asset")


def extract(archive: Path, destination: Path) -> dict:
    files = native.inspect_archive(archive)
    destination.mkdir()
    with zipfile.ZipFile(archive) as source:
        for name, record in files.items():
            target = destination / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with source.open(name) as reader, target.open("xb") as writer:
                shutil.copyfileobj(reader, writer, length=1024 * 1024)
            target.chmod(record["mode"])
    return files


def prepare(work: Path, tag: str, commit: str, digest: str) -> None:
    check_inputs(tag, commit, digest)
    if work.exists():
        raise ValueError("signing work directory must be new")
    work.mkdir(parents=True)
    original = work / "original"
    original.mkdir()
    release = api("releases/tags/" + tag)
    assets = release_assets(release, tag, digest)
    check_backfill(release, assets, tag, commit, digest)
    if api("commits/" + tag).get("sha") != commit:
        raise ValueError("release tag does not resolve to the requested source")
    for name, asset in assets.items():
        download(asset, original / name)
    if (original / (ARCHIVE + ".sha256")).read_text() != f"{digest}  {ARCHIVE}\n":
        raise ValueError("published checksum does not match the original ZIP")
    attestation = run("gh", "attestation", "verify", str(original / ARCHIVE),
                      "--repo", REPOSITORY, "--signer-workflow",
                      REPOSITORY + "/.github/workflows/bcr_preflight.yml",
                      "--source-digest", commit, "--source-ref", "refs/heads/main",
                      "--deny-self-hosted-runners", "--format", "json")
    (original / "attestation.json").write_text(attestation)
    files = extract(original / ARCHIVE, work / "bundle")
    native._check_embedded_build_info(original / ARCHIVE, f"{tag[1:]}\n{commit}\n".encode())
    native.validate_native(work / "bundle" / native.BUNDLE)
    receipt = {"schema": 1, "repository": REPOSITORY, "tag": tag, "source_commit": commit,
               "release_id": release["id"], "candidate_record": BACKFILL["candidate_record"],
               "original_assets": assets,
               "original_files": files, "workflow_commit": os.environ["GITHUB_SHA"],
               "run_id": os.environ["GITHUB_RUN_ID"],
               "run_attempt": os.environ["GITHUB_RUN_ATTEMPT"]}
    (original / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")


def check_signature(text: str, team: str) -> None:
    lines = text.splitlines()
    if ("TeamIdentifier=" + team not in lines
            or "Authority=Developer ID Certification Authority" not in lines
            or "Authority=Apple Root CA" not in lines
            or not any(line.startswith("Authority=Developer ID Application:") for line in lines)
            or not any("flags=" in line and "runtime" in line for line in lines)
            or not any(line.startswith("Timestamp=") for line in lines)
            or "Signature=adhoc" in lines):
        raise ValueError("signed image lacks the expected Developer ID, timestamp or hardened runtime")


def verify_signed(app: Path, team: str, *, notarized: bool) -> None:
    run("/usr/bin/codesign", "--verify", "--deep", "--strict", str(app))
    images = [app, *sorted((app / "Contents/Frameworks").glob("*.dylib"))]
    for image in images:
        check_signature(run("/usr/bin/codesign", "--display", "--verbose=4", str(image)), team)
    if notarized:
        run("/usr/bin/xcrun", "stapler", "validate", str(app))
        assessment = run("/usr/sbin/spctl", "--assess", "--type", "execute", "--verbose=4", str(app))
        if "source=Notarized Developer ID" not in assessment:
            raise ValueError("Gatekeeper did not accept a notarized Developer ID app")


def signing_identity(text: str, team: str) -> str:
    pattern = r'\s*\d+\) ([0-9A-F]{40}) "Developer ID Application: [^"\n]+ \(' + team + r'\)"'
    matches = re.findall(pattern, text)
    if len(matches) != 1:
        raise ValueError("temporary keychain must contain exactly one valid Developer ID identity")
    return matches[0]


@contextmanager
def signing_keychain(work: Path):
    private = work / "private"
    private.mkdir(mode=0o700)
    keychain = private / "signing.keychain-db"
    certificate = private / "identity.p12"
    password = secrets.token_hex(32)
    try:
        certificate.write_bytes(base64.b64decode("".join(os.environ[SECRET_NAMES[0]].split()), validate=True))
        certificate.chmod(0o600)
        run("/usr/bin/security", "create-keychain", "-p", password, str(keychain))
        run("/usr/bin/security", "set-keychain-settings", "-lut", "3600", str(keychain))
        run("/usr/bin/security", "unlock-keychain", "-p", password, str(keychain))
        run("/usr/bin/security", "import", str(certificate), "-k", str(keychain),
            "-P", os.environ[SECRET_NAMES[1]], "-T", "/usr/bin/codesign")
        run("/usr/bin/security", "set-key-partition-list", "-S", "apple-tool:,apple:",
            "-s", "-k", password, str(keychain))
        yield keychain
    finally:
        try:
            if keychain.exists():
                run("/usr/bin/security", "delete-keychain", str(keychain))
        finally:
            if certificate.exists():
                certificate.unlink()
            for name in SECRET_NAMES:
                os.environ.pop(name, None)


def notary_log_summary(raw: str) -> dict:
    value = json.loads(raw)
    if not isinstance(value, dict):
        raise ValueError("invalid Apple notarization log")
    issues = value.get("issues") or []
    if not isinstance(issues, list) or len(issues) > 100:
        raise ValueError("Apple notarization issues exceed the bounded log contract")
    records = []
    for issue in issues:
        if not isinstance(issue, dict):
            raise ValueError("invalid Apple notarization issue")
        message = str(issue.get("message", ""))[:2048]
        for name in SECRET_NAMES:
            secret = os.environ.get(name)
            if secret:
                message = message.replace(secret, "[redacted]")
        records.append({"severity": str(issue.get("severity", ""))[:32], "message": message})
    return {"issues": records}


def wait_notarization(work: Path, keychain: Path, request_id: str) -> dict:
    args = ["/usr/bin/xcrun", "notarytool", "wait", request_id,
            "--keychain-profile", "donner-notarization", "--keychain", str(keychain),
            "--timeout", "20m", "--output-format", "json"]
    status = {"id": request_id, "status": "Unknown"}
    try:
        result = subprocess.run(args, check=False, capture_output=True, timeout=1260)
        if len(result.stdout) <= native.MAX_CONTROL_BYTES:
            value = json.loads(result.stdout)
            if isinstance(value, dict) and value.get("status") in ("Accepted", "Invalid", "In Progress", "Rejected"):
                status["status"] = value["status"]
    except (OSError, subprocess.SubprocessError, ValueError):
        pass
    (work / "notary-status.json").write_text(json.dumps(status) + "\n")
    if status["status"] in ("Accepted", "Invalid", "Rejected"):
        log = run("/usr/bin/xcrun", "notarytool", "log", request_id,
                  "--keychain-profile", "donner-notarization", "--keychain", str(keychain))
        (work / "notary-log.json").write_text(json.dumps(notary_log_summary(log), indent=2) + "\n")
    if status["status"] != "Accepted":
        raise ValueError("Apple did not accept the submission; retained request ID permits recovery")
    return status


def notarize(work: Path, app: Path, keychain: Path) -> dict:
    submission = work / "submission.zip"
    native._write_zip(app, work / "bundle/LICENSE", work / "bundle/NOTICE.txt", submission)
    run("/usr/bin/xcrun", "notarytool", "store-credentials", "donner-notarization",
        "--keychain", str(keychain), "--apple-id", os.environ["APPLE_ID"],
        "--team-id", os.environ["APPLE_TEAM_ID"],
        "--password", os.environ["APPLE_APP_SPECIFIC_PASSWORD"])
    result = json.loads(run("/usr/bin/xcrun", "notarytool", "submit", str(submission),
                            "--keychain-profile", "donner-notarization", "--keychain", str(keychain),
                            "--output-format", "json", timeout=300))
    request_id = result.get("id", "")
    if not re.fullmatch(r"[0-9a-fA-F]{8}(?:-[0-9a-fA-F]{4}){3}-[0-9a-fA-F]{12}", request_id):
        raise ValueError("Apple returned an invalid notarization request identity")
    status = {"id": request_id, "status": "Submitted",
              "submission_sha256": native.release_cli.digest(submission)}
    (work / "notary-submission.json").write_text(json.dumps(status) + "\n")
    status.update(wait_notarization(work, keychain, request_id))
    run("/usr/bin/xcrun", "stapler", "staple", str(app), timeout=180)
    return status


def check_resources(original: dict, final: dict) -> None:
    prefix = native.BUNDLE + "/Contents/"
    mutable = {prefix + "MacOS/Donner", prefix + "_CodeSignature/CodeResources",
               prefix + "CodeResources"}
    mutable.update(name for name in original if name.startswith(prefix + "Frameworks/")
                   and name.endswith(".dylib"))
    if set(final) - set(original) - {prefix + "CodeResources"} or set(original) - set(final):
        raise ValueError("signing changed the bundle file inventory")
    for name, record in original.items():
        if final[name]["mode"] != record["mode"]:
            raise ValueError("signing changed a retained executable mode")
        if name not in mutable and final.get(name) != record:
            raise ValueError("signing changed an application resource or license")


def sign(work: Path) -> None:
    if any(not os.environ.get(name) for name in SECRET_NAMES):
        raise ValueError("all signing environment secrets must be configured")
    team = os.environ["APPLE_TEAM_ID"]
    if not re.fullmatch(r"[A-Z0-9]{10}", team):
        raise ValueError("invalid Apple team identifier")
    receipt = json.loads((work / "original/receipt.json").read_text())
    app = work / "bundle" / native.BUNDLE
    with signing_keychain(work) as keychain:
        identity = signing_identity(run("/usr/bin/security", "find-identity", "-v", "-p",
                                        "codesigning", str(keychain)), team)
        for image in [*sorted((app / "Contents/Frameworks").glob("*.dylib")), app]:
            run("/usr/bin/codesign", "--force", "--sign", identity, "--keychain", str(keychain),
                "--options", "runtime", "--timestamp", str(image))
        verify_signed(app, team, notarized=False)
        receipt["notarization"] = notarize(work, app, keychain)
    finish(work, receipt, team, identity)


def finish(work: Path, receipt: dict, team: str, identity: str) -> None:
    output = work / "signed"
    output.mkdir()
    app = work / "bundle" / native.BUNDLE
    native._write_zip(app, work / "bundle/LICENSE", work / "bundle/NOTICE.txt", output / ARCHIVE)
    files = extract(output / ARCHIVE, work / "roundtrip")
    check_resources(receipt["original_files"], files)
    restored = work / "roundtrip" / native.BUNDLE
    verify_signed(restored, team, notarized=True)
    checksum = native.release_cli.digest(output / ARCHIVE)
    (output / (ARCHIVE + ".sha256")).write_text(f"{checksum}  {ARCHIVE}\n")
    receipt.update({"team_id": team, "signing_identity_sha1": identity, "files": files,
                    "archive_sha256": checksum, "archive_size": (output / ARCHIVE).stat().st_size,
                    "checks": {"signature": "Developer ID", "hardened_runtime": True,
                               "timestamp": True, "stapled_roundtrip": True,
                               "gatekeeper": "Notarized Developer ID"}})
    (output / "receipt.json").write_text(json.dumps(receipt, indent=2) + "\n")


def verify_artifact(work: Path) -> None:
    """Exercise signed bytes only on a separate runner without signing or attestation access."""
    archive, receipt_path = work / ARCHIVE, work / "receipt.json"
    receipt_digest = native.release_cli.digest(receipt_path)
    receipt = json.loads(receipt_path.read_text())
    digest = native.release_cli.digest(archive)
    if digest != receipt["archive_sha256"] or receipt["source_commit"] != BACKFILL["commit"]:
        raise ValueError("signed artifact does not match its derivation receipt")
    if receipt["candidate_record"] != BACKFILL["candidate_record"]:
        raise ValueError("signed artifact names a different source candidate")
    if (work / (ARCHIVE + ".sha256")).read_text() != f"{digest}  {ARCHIVE}\n":
        raise ValueError("signed checksum file differs from the verified ZIP")
    files = extract(archive, work / "verified-bundle")
    if files != receipt["files"]:
        raise ValueError("signed archive inventory differs from receipt")
    check_resources(receipt["original_files"], files)
    app = work / "verified-bundle" / native.BUNDLE
    verify_signed(app, receipt["team_id"], notarized=True)
    native._check_embedded_build_info(archive, (BACKFILL["tag"][1:] + "\n" + BACKFILL["commit"] + "\n").encode())
    native._check_entrypoint(app, environment={"PATH": "/usr/bin:/bin:/usr/sbin:/sbin",
                                             "HOME": str(work / "verified-bundle"),
                                             "TMPDIR": str(work / "verified-bundle")})
    if native.release_cli.digest(archive) != digest or native.release_cli.digest(receipt_path) != receipt_digest:
        raise ValueError("artifact changed during executable verification")
    proof = {"archive_sha256": digest, "receipt_sha256": receipt_digest, "entrypoint": True}
    (work / "verification.json").write_text(json.dumps(proof, indent=2) + "\n")


def check_verification(work: Path) -> None:
    proof = json.loads((work / "verification.json").read_text())
    expected = {"archive_sha256": native.release_cli.digest(work / ARCHIVE),
                "receipt_sha256": native.release_cli.digest(work / "receipt.json"), "entrypoint": True}
    if proof != expected:
        raise ValueError("verification evidence does not bind the artifacts being attested")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("prepare", "sign", "verify", "check-verification"))
    parser.add_argument("--work", type=Path, required=True)
    args = parser.parse_args()
    work = args.work.resolve()
    if args.operation == "prepare":
        prepare(work, BACKFILL["tag"], BACKFILL["commit"], BACKFILL["archive_sha256"])
    elif args.operation == "sign":
        sign(work)
    elif args.operation == "verify":
        verify_artifact(work)
    else:
        check_verification(work)


if __name__ == "__main__":
    try:
        main()
    except (ValueError, KeyError, OSError, subprocess.SubprocessError):
        raise SystemExit("Editor signing failed; inspect retained validation and notarization evidence.") from None
