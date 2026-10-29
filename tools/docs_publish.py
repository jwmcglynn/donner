"""Select, retain and promote an approved complete documentation composition."""
from __future__ import annotations

import argparse
import base64
import json
import os
from pathlib import Path
import shutil
import subprocess
import time
import urllib.error
import urllib.parse
import urllib.request

from tools import bcr_source
from tools import docs_site as site
from tools import docs_store as store
from tools import pages_artifact
from tools import release_candidate as candidate

WORKFLOW = ".github/workflows/deploy_docs.yaml"
SITE_URL = "https://jwmcglynn.github.io/donner/"


def github_json(*args: str) -> dict:
    result = subprocess.run(["gh", "api", *args], capture_output=True, text=True,
                            timeout=180, check=True)
    site.require(len(result.stdout) <= store.MAX_CONTROL, "GitHub JSON is oversized")
    return json.loads(result.stdout)


def download(artifact_id: int, output: Path) -> None:
    with output.open("wb") as stream:
        subprocess.run(["gh", "api", f"{store.REPO}/actions/artifacts/{artifact_id}/zip"],
                       stdout=stream, stderr=subprocess.PIPE, timeout=180, check=True)
    site.require(output.stat().st_size <= site.MAX_SITE, "artifact is oversized")


def _attestation_command(args: list[str], stdout) -> None:
    try:
        subprocess.run(args, stdout=stdout, stderr=subprocess.PIPE, timeout=180, check=True)
    except subprocess.CalledProcessError as error:
        diagnostic = error.stderr or b""
        tokens = {os.environ.get(name, "") for name in
                  ("GH_TOKEN", "GITHUB_TOKEN", "ACTIONS_ID_TOKEN_REQUEST_TOKEN")}
        for token in sorted(tokens, key=len, reverse=True):
            if token:
                diagnostic = diagnostic.replace(token.encode(), b"[REDACTED]")
        detail = json.dumps(diagnostic[:4096].decode("utf-8", errors="replace"))
        detail = detail.replace("#", "\\u0023")
        raise ValueError(f"GitHub attestation verification failed (exit {error.returncode}): {detail}") from None


def attestation(path: Path, workflow: str, commit: str, output: Path) -> None:
    with output.open("wb") as stream:
        _attestation_command(["gh", "attestation", "verify", str(path), "--repo", "jwmcglynn/donner",
                        "--signer-workflow", "jwmcglynn/donner/" + workflow,
                        "--source-digest", commit, "--source-ref", "refs/heads/main",
                        "--deny-self-hosted-runners", "--format", "json"], stdout=stream)


def source_file(commit: str, name: str) -> str:
    site.require(site.SHA.fullmatch(commit), "invalid source commit")
    value = github_json(f"{store.REPO}/contents/{name}?ref={commit}")
    return base64.b64decode(value["content"], validate=False).decode("utf-8")


def identity(commit: str, run: int, attempt: int, archive: Path) -> dict:
    return {"version": bcr_source.module_values(source_file(commit, "MODULE.bazel"))["version"],
            "source_commit": commit, "run_id": run, "attempt": attempt,
            "archive_sha256": bcr_source.digest(archive)}


def release_source(reference: str, expected_digest: str, work: Path) -> tuple[Path, dict]:
    run, attempt = candidate._run_reference(reference, "release record")
    selected = candidate.docs_record(record_run_id=run, record_attempt=attempt,
                                     record_sha256=expected_digest, work=work / "record")
    record_path = Path(selected["record_path"])
    attestation(record_path, ".github/workflows/release_candidate.yml", selected["record_commit"],
                work / "record-attestation.json")
    shutil.copyfile(record_path, work / "release-candidate.json")
    record = candidate.load_record(record_path.read_bytes(), expected_digest)
    candidate.docs_selection(record_path=record_path, record_commit=selected["record_commit"],
                             record_sha256=expected_digest, work=work / "record-selection")
    version = record["source"]["version"]
    site.require(site.VERSION.fullmatch(version), "release needs a stable module version")
    release = github_json(f"{store.REPO}/releases/tags/v{version}")
    site.require(release.get("tag_name") == "v" + version and release.get("draft") is False
                 and release.get("prerelease") is False, "release docs need a published stable release")
    tag = github_json(f"{store.REPO}/git/ref/tags/v{version}")["object"]
    if tag["type"] == "tag":
        tag = github_json(f"{store.REPO}/git/tags/{tag['sha']}")["object"]
    site.require(tag["type"] == "commit" and tag["sha"] == record["source"]["commit"],
                 "release tag differs from candidate source")
    candidate.fetch_role(record, "docs", candidate.NO_SOURCE_CHECKOUT, work / "release.zip",
                         work / "release")
    archive = work / "release/artifact.tar"
    entry = record["artifacts"]["docs"]
    value = identity(record["source"]["commit"], entry["run_id"], entry["attempt"], archive)
    site.require(value["version"] == version, "tag and module version differ")
    return archive, value


def main_source(reference: str, work: Path) -> tuple[Path, dict]:
    run_id, attempt = candidate._run_reference(reference, "main docs run")
    run = github_json(f"{store.REPO}/actions/runs/{run_id}/attempts/{attempt}")
    commit = run["head_sha"]
    pages_artifact.verify_source_run(run, {"run_id": run_id, "attempt": attempt, "source_commit": commit})
    roles = {}
    for role in ("docs-provenance", "docs"):
        path = work / (role + ".zip")
        entry = candidate._collect_artifact(role, commit, run_id, attempt, path, github_json, download)
        candidate._extract_verified(path, entry["files"], work / role)
        roles[role] = entry
    archive = work / "docs/artifact.tar"
    value = identity(commit, run_id, attempt, archive)
    pages_artifact.verify_provenance(work / "docs-provenance", {
        "run_id": run_id, "attempt": attempt, "source_commit": commit,
        "archive_sha256": value["archive_sha256"]}, source_file(commit, ".bazelversion").strip())
    pages_artifact.verify_metadata(github_json(f"{store.REPO}/actions/artifacts/{roles['docs']['artifact_id']}"),
                                   run, {**value, "artifact_id": roles["docs"]["artifact_id"],
                                         "artifact_digest": "sha256:" + roles["docs"]["zip_sha256"]})
    (work / "main-selection.json").write_bytes(site.canonical({"identity": value, "artifacts": roles}))
    return archive, value


def release_identity(record: dict, version: str) -> dict:
    source = record["source"]
    docs = record["artifacts"]["docs"]
    site.require(source["version"] == version, "signed record names another release version")
    value = {"version": version, "source_commit": source["commit"], "run_id": docs["run_id"],
             "attempt": docs["attempt"], "archive_sha256": docs["files"]["artifact.tar"]}
    site.validate_identity(value)
    return value


def require_release_binding(record: dict, version: str, value: dict, archive_digest: str) -> None:
    expected = release_identity(record, version)
    site.require(value == expected and archive_digest == expected["archive_sha256"],
                 "retained release differs from its independently signed source")


def _verify_retained_release(api, snapshot, version, entry, work):
    prefix = f"releases/{version}/"
    record_bytes = api.file(snapshot, prefix + "candidate.json")
    site.require(len(record_bytes) <= candidate.MAX_RECORD_BYTES, "retained candidate record is oversized")
    record = candidate.load_record(record_bytes, site.digest(record_bytes))
    proof = work / ("proof-" + version)
    proof.mkdir()
    path = proof / "candidate.json"
    path.write_bytes(record_bytes)
    _offline_attestation(path, api.file(snapshot, prefix + "record-attestation.json"),
                         record["source"]["commit"], proof, workflow=".github/workflows/release_candidate.yml")
    require_release_binding(record, version, entry["identity"], entry["archive"]["sha256"])
    release = github_json(f"{store.REPO}/releases/tags/v{version}")
    site.require(release.get("tag_name") == "v" + version and release.get("draft") is False
                 and release.get("prerelease") is False, "retained release is not a published stable release")
    tag = github_json(f"{store.REPO}/git/ref/tags/v{version}")["object"]
    if tag["type"] == "tag":
        tag = github_json(f"{store.REPO}/git/tags/{tag['sha']}")["object"]
    site.require(tag["type"] == "commit" and tag["sha"] == record["source"]["commit"],
                 "retained release no longer matches its canonical tag")


def require_preserved_releases(manifest: dict, releases: dict) -> None:
    previous = {key: value["identity"] for key, value in manifest["versions"].items() if key != "main"}
    site.require(set(previous) <= set(releases), "snapshot index removed an existing release")
    for version, identity in previous.items():
        site.require(releases[version]["identity"] == identity,
                     "snapshot index changed an existing release")


def _verify_current_roster(api, snapshot, state, work):
    composition_id = state["current"]
    if composition_id is None:
        return
    _restore_composition(api, snapshot, composition_id, work / "current-roster")
    manifest = site.read_json(work / "current-roster/site/_docs/versions.json")
    require_preserved_releases(manifest, state["releases"])


def prepare(work: Path, main_run: str, release_run: str, record_sha256: str,
            snapshot: str | None, api=None, bootstrap: dict | None = None,
            pinned_snapshot: bool = False) -> None:
    api = api or store.GitHub()
    site.empty(work)
    if pinned_snapshot:
        store.automatic_context()
        state = store.read_index(api, snapshot)
    else:
        state = store.index(api, snapshot)
    _verify_current_roster(api, snapshot, state, work)
    if bootstrap is not None:
        _bootstrap_recovery(api, snapshot, state, bootstrap, work / "bootstrap")
    else:
        site.require(state["current"] is not None or not state["releases"],
                     "unactivated seed requires its approved bootstrap selection")
    payload = work / "payload"
    payload.mkdir()
    sources = {}
    for version, entry in state["releases"].items():
        site.require(site.VERSION.fullmatch(version) and set(entry) == {"identity", "archive"},
                     "invalid retained release")
        _verify_retained_release(api, snapshot, version, entry, work)
        archive = work / (version + ".tar")
        store.restore(entry["archive"], archive, lambda path: api.file(snapshot, path))
        sources[version] = (archive, entry["identity"])
    if release_run:
        archive, value = release_source(release_run, record_sha256, work)
        version = value["version"]
        if version in sources:
            site.require(sources[version][1] == value, "retained release is immutable")
        else:
            state["releases"][version] = {"identity": value,
                                           "archive": store.split_archive(archive, payload)}
            proof = payload / "releases" / version
            proof.mkdir(parents=True)
            shutil.copyfile(work / "release-candidate.json", proof / "candidate.json")
            shutil.copyfile(work / "record-attestation.json", proof / "record-attestation.json")
            sources[version] = (archive, value)
    else:
        site.require(not record_sha256, "unexpected release record digest")
    site.require(state["releases"], "first composition needs a verified release record")
    sources["main"] = main_source(main_run, work)
    stable = max(state["releases"], key=lambda value: tuple(map(int, value.split("."))))
    site.compose(sources, stable, work / "site")
    (payload / "selection.json").write_bytes(site.canonical({"base_snapshot_commit": snapshot,
        "previous_composition": state["current"], "releases": state["releases"],
        "source_inputs": {k: v[1] for k, v in sources.items()}, "bootstrap": bootstrap}))
    # Selection and validation evidence stay in the retained payload, never the public site.
    evidence = payload / "evidence"
    evidence.mkdir()
    for path in work.rglob("provenance.json"):
        shutil.copyfile(path, evidence / (str(len(list(evidence.iterdir()))) + "-provenance.json"))
    if (work / "main-selection.json").exists():
        shutil.copyfile(work / "main-selection.json", evidence / "main-selection.json")
    if (work / "record-attestation.json").exists():
        shutil.copyfile(work / "record-attestation.json", evidence / "release-attestation.json")
        shutil.copyfile(work / "release-candidate.json", evidence / "candidate.json")


def finalize(work: Path, archive: Path, artifact_id: int) -> None:
    payload = work / "payload"
    selected = site.read_json(payload / "selection.json")
    site.unpack(archive, work / "uploaded-site")
    marker = site.verify_site(work / "uploaded-site")
    metadata = github_json(f"{store.REPO}/actions/artifacts/{artifact_id}")
    record = {"schema": 1, "archive_sha256": bcr_source.digest(archive), "artifact_id": artifact_id,
        "artifact_digest": metadata["digest"], "run_id": int(os.environ["GITHUB_RUN_ID"]),
        "attempt": int(os.environ["GITHUB_RUN_ATTEMPT"]), "workflow_commit": os.environ["GITHUB_SHA"],
        "base_snapshot_commit": selected["base_snapshot_commit"],
        "previous_composition": selected["previous_composition"], "bootstrap": selected["bootstrap"],
        "manifest_sha256": marker["manifest_sha256"], "payload_files": site.inventory(payload, store.CHUNK)}
    (payload / "composition.json").write_bytes(site.canonical(record))


def fetch_composition(reference: str, expected: str, work: Path) -> dict:
    run_id, attempt = candidate._run_reference(reference, "composition run")
    site.require(site.DIGEST.fullmatch(expected), "invalid composition digest")
    run = github_json(f"{store.REPO}/actions/runs/{run_id}/attempts/{attempt}")
    site.require(run.get("id") == run_id and run.get("run_attempt") == attempt
                 and run.get("repository", {}).get("full_name") == "jwmcglynn/donner"
                 and run.get("path") == WORKFLOW and run.get("event") == "workflow_dispatch"
                 and run.get("head_branch") == "main" and site.SHA.fullmatch(run.get("head_sha", ""))
                 and run.get("status") == "completed" and run.get("conclusion") == "success",
                 "composition is not a successful trusted-main manual attempt")
    artifacts = github_json(f"{store.REPO}/actions/runs/{run_id}/artifacts?per_page=100")["artifacts"]
    matches = [a for a in artifacts if a["name"] == f"docs-site-payload-{attempt}" and not a["expired"]]
    site.require(len(matches) == 1, "composition needs one retained payload")
    path = work / "payload.zip"
    candidate._download_checked(matches[0]["id"], matches[0]["digest"].removeprefix("sha256:"),
                                path, download, "composition payload")
    _extract_payload(path, work / "payload")
    record_path = work / "payload/composition.json"
    site.require(bcr_source.digest(record_path) == expected, "approved composition record changed")
    attestation(record_path, WORKFLOW, run["head_sha"], work / "composition-attestation.json")
    record = site.read_json(record_path)
    site.require(record["run_id"] == run_id and record["attempt"] == attempt
                 and record["workflow_commit"] == run["head_sha"], "composition run differs")
    data = site.inventory(work / "payload", store.CHUNK)
    data.pop("composition.json")
    site.require(data == record["payload_files"], "composition payload changed")
    _fetch_pages(record, work)
    return record


def _extract_payload(archive: Path, output: Path) -> None:
    import zipfile
    site.empty(output)
    total = 0
    with zipfile.ZipFile(archive) as zipped:
        seen = set()
        for info in zipped.infolist():
            name = site.safe_path(info.filename)
            total += info.file_size
            site.require(name not in seen and info.file_size <= store.CHUNK and total <= site.MAX_SITE
                         and len(seen) < site.MAX_FILES and not info.is_dir()
                         and ((info.external_attr >> 16) & 0o170000) in (0, 0o100000),
                         "unsafe composition payload")
            seen.add(name)
            target = output / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with zipped.open(info) as source, target.open("xb") as destination:
                shutil.copyfileobj(source, destination, 1024 * 1024)


def _fetch_pages(record: dict, work: Path, bundle: Path | None = None) -> None:
    meta = github_json(f"{store.REPO}/actions/artifacts/{record['artifact_id']}")
    site.require(meta["name"] == "github-pages" and not meta["expired"]
                 and meta["digest"] == record["artifact_digest"]
                 and meta["workflow_run"]["id"] == record["run_id"]
                 and meta["workflow_run"]["head_sha"] == record["workflow_commit"],
                 "Pages artifact differs from composition")
    candidate._download_checked(record["artifact_id"], record["artifact_digest"].removeprefix("sha256:"),
                                work / "pages.zip", download, "composed Pages artifact")
    candidate._extract_verified(work / "pages.zip", {"artifact.tar": record["archive_sha256"]}, work / "pages")
    archive = work / "pages/artifact.tar"
    if bundle is None:
        attestation(archive, WORKFLOW, record["workflow_commit"], work / "pages-attestation.json")
    else:
        _bundle_attestation(archive, bundle, record["workflow_commit"], work / "pages-attestation.json")
    site.unpack(archive, work / "site")
    site.verify_site(work / "site", record["manifest_sha256"])


class _NoRedirects(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        raise ValueError("HTTPS redirects are not allowed")


def _https_response(url: str, headers: dict, timeout: float = 30):
    parsed = urllib.parse.urlsplit(url)
    site.require(parsed.scheme == "https" and bool(parsed.hostname)
                 and not parsed.username and not parsed.password and not parsed.fragment
                 and parsed.port in (None, 443), "request requires a direct HTTPS URL")
    request = urllib.request.Request(url, headers=headers)
    return urllib.request.build_opener(_NoRedirects()).open(request, timeout=timeout)


def live_marker(url: str = SITE_URL, *, deadline: float | None = None) -> dict | None:
    try:
        with _https_response(url + "docs-site.json", {"Cache-Control": "no-cache"},
                             timeout=_request_timeout(deadline)) as response:
            data = _read_body(response, site.MAX_FILE, deadline)
        site.require(len(data) <= site.MAX_FILE, "live marker is oversized")
        return json.loads(data)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            return None
        raise


def deploy(artifact_id: int, commit: str, api, wait=time.sleep, receipt: Path | None = None) -> str:
    site.require(isinstance(commit, str) and site.SHA.fullmatch(commit), "invalid Pages source commit")
    api.require_protection()
    token = _oidc_token()
    value = api.call(store.REPO + "/pages/deployments", "POST", {
        "artifact_id": artifact_id, "pages_build_version": commit,
        "oidc_token": token, "environment": store.deployment_environment()})
    site.require(isinstance(value, dict), "Pages returned no deployment identity")
    deployment_id = str(value.get("id") or str(value.get("status_url", "")).rsplit("/", 1)[-1])
    site.require(bool(__import__("re").fullmatch(r"[A-Za-z0-9-]{1,128}", deployment_id)),
                 "Pages returned an invalid deployment identity")
    if receipt is not None:
        receipt.write_bytes(site.canonical({"deployment_id": deployment_id, "artifact_id": artifact_id}))
    page_url = value.get("page_url", "")
    if page_url and not page_url.startswith("https://"):
        page_url = "https://" + page_url
    site.require(page_url.rstrip("/") == SITE_URL.rstrip("/"), "Pages returned an unexpected site URL")
    deadline = time.monotonic() + 600
    while time.monotonic() < deadline:
        status = api.call(store.REPO + "/pages/deployments/" + deployment_id)["status"]
        if status == "succeed":
            return deployment_id
        site.require(status not in {"deployment_failed", "deployment_content_failed", "deployment_cancelled",
                                    "deployment_lost"}, "Pages deployment failed: " + deployment_id)
        wait(5)
    raise ValueError("Pages deployment timeout: " + deployment_id)


def _oidc_token() -> str:
    # The token travels only to GitHub Pages; it is never persisted or printed.
    url = os.environ["ACTIONS_ID_TOKEN_REQUEST_URL"]
    headers = {"Authorization": "Bearer " + os.environ["ACTIONS_ID_TOKEN_REQUEST_TOKEN"]}
    with _https_response(url, headers) as response:
        return json.load(response)["value"]


def _restore_composition(api, snapshot: str, target: str, work: Path) -> tuple[dict, dict]:
    site.require(bool(site.DIGEST.fullmatch(target)), "invalid composition content address")
    site.empty(work)
    prefix = f"compositions/{target}/"
    record_bytes = api.file(snapshot, prefix + "composition.json")
    record = json.loads(record_bytes)
    site.require(record["archive_sha256"] == target, "stored composition content address differs")
    path = work / "composition.json"
    path.write_bytes(record_bytes)
    _offline_attestation(path, api.file(snapshot, prefix + "composition-attestation.json"),
                         record["workflow_commit"], work)
    selected_bytes = api.file(snapshot, prefix + "selection.json")
    site.require(site.digest(selected_bytes) == record["payload_files"]["selection.json"],
                 "stored signed selection changed")
    descriptor = json.loads(api.file(snapshot, prefix + "archive.json"))
    site.require(descriptor["sha256"] == target, "prior archive differs from approved content address")
    pages = work / "pages"
    pages.mkdir()
    store.restore(descriptor, pages / "artifact.tar", lambda name: api.file(snapshot, name))
    _offline_attestation(pages / "artifact.tar", api.file(snapshot, prefix + "pages-attestation.json"),
                         record["workflow_commit"], work)
    site.unpack(pages / "artifact.tar", work / "site")
    site.verify_site(work / "site", record["manifest_sha256"])
    return record, json.loads(selected_bytes)


def _previous(api, snapshot: str, prior: str, work: Path) -> tuple[dict, dict]:
    _restore_composition(api, snapshot, prior, work / "prior-proof")
    shutil.copytree(work / "prior-proof/pages", work / "previous-pages")
    marker = site.verify_site(work / "prior-proof/site")
    return marker, {"kind": "composition", "marker": marker}


def validate_bootstrap(value: dict | None) -> None:
    if value is None:
        return
    site.require(isinstance(value, dict) and set(value) == {
        "recovery_archive_sha256", "observed_index_sha256", "approve_initial_release_recovery"}
        and value["approve_initial_release_recovery"] is True,
        "bootstrap needs an explicit initial release recovery approval")
    for key in ("recovery_archive_sha256", "observed_index_sha256"):
        site.require(isinstance(value[key], str) and bool(site.DIGEST.fullmatch(value[key])),
                     "bootstrap needs exact recovery and observed index digests")


def _recovery_sources(record, selected, work, api, snapshot) -> None:
    site.require(record.get("bootstrap") is None and selected.get("bootstrap") is None,
                 "recovery composition cannot itself depend on bootstrap")
    releases = selected["releases"]
    site.require(len(releases) == 1, "initial recovery needs exactly one stable release")
    version = next(iter(releases))
    site.require(bool(site.VERSION.fullmatch(version)), "initial recovery needs a stable release")
    value = releases[version]["identity"]
    site.require(selected["source_inputs"] == {version: value, "main": value},
                 "both recovery routes must use the same signed release source")
    manifest = site.read_json(work / "site/_docs/versions.json")
    site.require(set(manifest["versions"]) == {version, "main"} and manifest["stable"] == version
                 and manifest.get("recovery_release") == version
                 and all(entry["identity"] == value for entry in manifest["versions"].values()),
                 "recovery site must label and bind both routes to its release snapshot")
    _verify_retained_release(api, snapshot, version, releases[version], work)
    source = work / "release-source.tar"
    store.restore(releases[version]["archive"], source, lambda name: api.file(snapshot, name))
    site.read_source(source, value, work / "release-source")


def _bootstrap_recovery(api, snapshot, current, approved, work):
    validate_bootstrap(approved)
    site.require(approved is not None and snapshot is not None
                 and current["current"] is None and current["previous"] is None,
                 "bootstrap is allowed only before a composition is activated")
    target = approved["recovery_archive_sha256"]
    metadata = json.loads(api.file(snapshot, f"compositions/{target}/bootstrap.json"))
    site.require(metadata == approved, "bootstrap selection differs from the immutable seed")
    record, selected = _restore_composition(api, snapshot, target, work)
    _recovery_sources(record, selected, work, api, snapshot)
    site.require(current["releases"] == selected["releases"], "bootstrap release roster changed")
    return {"kind": "composition", "marker": site.verify_site(work / "site")}


def _live_identity(expected: dict, *, deadline: float | None = None) -> bool:
    deadline = deadline if deadline is not None else time.monotonic() + 120
    if expected["kind"] == "composition":
        try:
            _verify_live(expected["marker"]["manifest_sha256"], expected["marker"], deadline=deadline)
            return True
        except ValueError:
            return False
    data = _live_file("index.html", deadline=deadline)
    return site.digest(data) == expected["index_sha256"] and live_marker(deadline=deadline) is None


def _wait_for_live(expected: dict, wait=None) -> None:
    pause = wait or time.sleep
    deadline = time.monotonic() + 120
    for attempt in range(24):
        if time.monotonic() >= deadline:
            break
        try:
            if _live_identity(expected, deadline=deadline) and time.monotonic() < deadline:
                return
        except (ValueError, OSError, urllib.error.URLError):
            pass
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        if attempt < 23:
            pause(min(5, remaining))
    raise ValueError("public site did not converge within the bounded propagation window")


def preflight(reference: str, expected: str, prior: str | None, work: Path, api=None,
              bootstrap: dict | None = None) -> None:
    api = api or store.GitHub()
    site.empty(work)
    record = fetch_composition(reference, expected, work)
    _preflight_record(record, prior, work, api, bootstrap)


def _preflight_record(record, prior, work, api, bootstrap=None):
    current = store.index(api, record["base_snapshot_commit"])
    site.require(prior == current["current"] and record["previous_composition"] == prior,
                 "approved prior composition differs")
    site.require(record.get("bootstrap") == bootstrap, "publication differs from signed bootstrap selection")
    if bootstrap is None:
        site.require(prior is not None, "bootstrap needs an explicitly approved recovery seed")
        _, prior_identity = _previous(api, record["base_snapshot_commit"], prior, work)
        recovery_identity = prior_identity
    else:
        recovery_identity = _bootstrap_recovery(api, record["base_snapshot_commit"], current,
                                                bootstrap, work / "bootstrap")
        site.require(record["archive_sha256"] != bootstrap["recovery_archive_sha256"],
                     "initial candidate must differ from its recovery selection")
        shutil.copytree(work / "bootstrap/pages", work / "previous-pages")
        prior_identity = {"kind": "legacy", "index_sha256": bootstrap["observed_index_sha256"]}
    site.require(_live_identity(prior_identity), "live site differs from the approved prior")
    plan = {"record": record, "current": current, "prior": prior,
            "prior_identity": prior_identity, "recovery_identity": recovery_identity,
            "previous_archive_sha256": bcr_source.digest(work / "previous-pages/artifact.tar")}
    (work / "preflight.json").write_bytes(site.canonical(plan))


def _verified_previous(artifact_id: int, plan: dict, work: Path) -> None:
    meta = github_json(f"{store.REPO}/actions/artifacts/{artifact_id}")
    site.require(not meta["expired"] and meta["name"] == "docs-rollback-" + os.environ["GITHUB_RUN_ATTEMPT"]
                 and meta["workflow_run"]["id"] == int(os.environ["GITHUB_RUN_ID"])
                 and meta["workflow_run"]["head_sha"] == os.environ["GITHUB_SHA"],
                 "rollback upload is not from this trusted run")
    candidate._download_checked(artifact_id, meta["digest"].removeprefix("sha256:"),
                                work / "rollback.zip", download, "rollback upload")
    candidate._extract_verified(work / "rollback.zip", {"artifact.tar": plan["previous_archive_sha256"]},
                                work / "rollback-verified")


def promote(work: Path, previous_id: int, api=None) -> dict:
    api = api or store.GitHub()
    plan = site.read_json(work / "preflight.json")
    record = plan["record"]
    site.require(store.index(api, plan.get("snapshot", record["base_snapshot_commit"])) == plan["current"],
                 "snapshot index changed after preflight")
    site.require(_live_identity(plan["prior_identity"]), "live site changed after preflight")
    _verified_previous(previous_id, plan, work)
    site.verify_site(work / "site", record["manifest_sha256"])
    site.require(bcr_source.digest(work / "pages/artifact.tar") == record["archive_sha256"],
                 "selected archive changed after preflight")
    return _retain_and_deploy(record, plan, previous_id, work, api)


def _retain_composition(record, work):
    payload = work / "payload"
    data = site.inventory(payload, store.CHUNK)
    data.pop("composition.json")
    site.require(data == record["payload_files"], "selected payload changed after preflight")
    composition_id = record["archive_sha256"]
    durable = work / "durable"
    durable.mkdir()
    for directory in ("objects", "releases"):
        if (payload / directory).exists():
            shutil.copytree(payload / directory, durable / directory)
    archive_record = store.split_archive(work / "pages/artifact.tar", durable)
    prefix = durable / "compositions" / composition_id
    prefix.mkdir(parents=True)
    (prefix / "archive.json").write_bytes(site.canonical(archive_record))
    shutil.copyfile(payload / "composition.json", prefix / "composition.json")
    shutil.copyfile(payload / "selection.json", prefix / "selection.json")
    for name in ("composition-attestation.json", "pages-attestation.json"):
        shutil.copyfile(work / name, prefix / name)
    if (payload / "evidence").exists():
        shutil.copytree(payload / "evidence", prefix / "evidence")
    return durable, archive_record


def _retain_and_deploy(record, plan, previous_id, work, api):
    selected = site.read_json(work / "payload/selection.json")
    composition_id = record["archive_sha256"]
    if os.environ.get("GITHUB_EVENT_NAME") == "workflow_run":
        store.automatic_context()
        site.require(plan.get("automatic") is True, "automatic publication plan missing")
        manifest = site.read_json(work / "prior-proof/site/_docs/versions.json")
        _automatic_scope(record, selected, plan["current"], manifest)
        _verify_auto_source(selected["source_inputs"]["main"])
    durable, archive_record = _retain_composition(record, work)
    new_index = {**plan["current"], "releases": (plan["current"]["releases"]
                 if record.get("bootstrap") is not None else selected["releases"])}
    tip = store.commit(api, plan.get("snapshot", record["base_snapshot_commit"]), durable, new_index)
    store.restore(archive_record, work / "retained.tar", lambda path: api.file(tip, path))
    result = {"candidate": composition_id, "prior": plan["prior"], "snapshot_commit": tip}
    try:
        result["deployment_id"] = deploy(record["artifact_id"], record["workflow_commit"], api,
                                         receipt=work / "candidate-deployment.json")
        _wait_for_live({"kind": "composition", "marker": site.read_json(work / "site/docs-site.json")})
    except (ValueError, OSError, urllib.error.URLError, subprocess.SubprocessError) as error:
        result.update(_recover(record, plan["prior_identity"], previous_id, api, str(error),
                               tip, _deployment_id(work), recovery_identity=plan.get("recovery_identity")))
        if result["status"] == "restored" and record.get("bootstrap") is not None:
            activation = work / "recovery-activation"
            activation.mkdir()
            new_index.update(current=record["bootstrap"]["recovery_archive_sha256"], previous=None)
            try:
                result["snapshot_commit"] = store.commit(api, tip, activation, new_index)
                result["status"] = "recovered-to-bootstrap"
            except (ValueError, OSError, subprocess.SubprocessError) as failure:
                result.update(status="recovery-required", recovery_error=
                              "Healthy bootstrap recovery; snapshot activation unconfirmed: " + str(failure))
    else:
        activation = work / "activation"
        activation.mkdir()
        new_index.update(current=composition_id, previous=plan["prior"], releases=selected["releases"])
        try:
            result["snapshot_commit"] = store.commit(api, tip, activation, new_index)
            result["status"] = "published"
        except (ValueError, OSError, subprocess.SubprocessError) as error:
            result.update(status="recovery-required", error="Healthy candidate; snapshot activation unconfirmed: " + str(error))
    (work / "result.json").write_bytes(site.canonical(result))
    site.require(result["status"] == "published", "promotion did not publish; inspect result.json")
    return result


def _request_timeout(deadline: float | None) -> float:
    if deadline is None:
        return 30
    remaining = deadline - time.monotonic()
    site.require(remaining > 0, "live health deadline expired")
    return min(5, remaining)


def _read_body(response, limit: int, deadline: float | None) -> bytes:
    if deadline is None:
        return response.read(limit + 1)
    data = bytearray()
    while len(data) <= limit:
        _request_timeout(deadline)
        part = response.read1(min(65536, limit + 1 - len(data)))
        _request_timeout(deadline)
        if not part:
            break
        data.extend(part)
    return bytes(data)


def _live_file(name: str, limit=site.MAX_FILE, *, deadline: float | None = None) -> bytes:
    site.safe_path(name)
    with _https_response(SITE_URL + name, {"Cache-Control": "no-cache"},
                         timeout=_request_timeout(deadline)) as response:
        site.require(response.geturl().startswith(SITE_URL), "live file redirected outside docs")
        data = _read_body(response, limit, deadline)
    site.require(len(data) <= limit, "live file is oversized")
    return data


def _verify_live(manifest: str, expected_marker: dict, *, deadline: float | None = None) -> None:
    marker = live_marker(deadline=deadline)
    site.require(marker == expected_marker and marker["manifest_sha256"] == manifest, "live composition differs")
    raw = _live_file("_docs/versions.json", 65536, deadline=deadline)
    site.require(site.digest(raw) == manifest, "live version manifest differs")
    versions = json.loads(raw)["versions"]
    names = ["index.html", "_docs/selector.js", "_docs/selector.css"]
    for version in versions:
        site.require(version == "main" or site.VERSION.fullmatch(version), "invalid live version")
        names += [version + "/index.html", version + "/docs-build.json"]
    for name in names:
        site.require(site.digest(_live_file(name, deadline=deadline)) == marker["files"][name], "live content differs: " + name)


def _offline_attestation(path: Path, raw: bytes, commit: str, work: Path, workflow: str = WORKFLOW) -> None:
    site.require(isinstance(commit, str) and site.SHA.fullmatch(commit), "invalid attested source commit")
    site.require(len(raw) <= site.MAX_FILE, "stored attestation is oversized")
    values = json.loads(raw)
    site.require(isinstance(values, list) and bool(values), "stored attestation is missing")
    bundle = work / (path.name + ".bundle.json")
    bundle.write_bytes(site.canonical(values[0]["attestation"]["bundle"]))
    _attestation_command(["gh", "attestation", "verify", str(path), "--bundle", str(bundle),
                    "--repo", "jwmcglynn/donner", "--signer-workflow", "jwmcglynn/donner/" + workflow,
                    "--source-digest", commit, "--source-ref", "refs/heads/main",
                    "--deny-self-hosted-runners"], stdout=subprocess.DEVNULL)


def rollback_preflight(target: str, snapshot: str, prior: str, work: Path, api=None) -> None:
    api = api or store.GitHub()
    site.require(site.DIGEST.fullmatch(target) and target != prior, "invalid rollback target")
    site.empty(work)
    current = store.index(api, snapshot)
    site.require(current["current"] == prior, "prior composition differs")
    record, _ = _restore_composition(api, snapshot, target, work / "target")
    shutil.copytree(work / "target/pages", work / "pages")
    shutil.copytree(work / "target/site", work / "site")
    _, prior_identity = _previous(api, snapshot, prior, work)
    site.require(_live_identity(prior_identity), "live site differs from prior")
    plan = {"record": record, "current": current, "snapshot": snapshot, "prior": prior,
            "prior_identity": prior_identity, "rollback": True,
            "previous_archive_sha256": bcr_source.digest(work / "previous-pages/artifact.tar")}
    (work / "preflight.json").write_bytes(site.canonical(plan))


def rollback(work: Path, candidate_id: int, previous_id: int, api=None) -> dict:
    api = api or store.GitHub()
    plan = site.read_json(work / "preflight.json")
    site.require(plan.get("rollback") is True and store.index(api, plan["snapshot"]) == plan["current"],
                 "rollback selection changed")
    record = plan["record"]
    _verified_previous(previous_id, plan, work)
    _verified_candidate(candidate_id, record["archive_sha256"], work)
    site.require(_live_identity(plan["prior_identity"]), "live site changed after preflight")
    result = {"candidate": record["archive_sha256"], "prior": plan["prior"]}
    try:
        result["deployment_id"] = deploy(candidate_id, os.environ["GITHUB_SHA"], api,
                                         receipt=work / "candidate-deployment.json")
        _wait_for_live({"kind": "composition", "marker": site.read_json(work / "site/docs-site.json")})
    except (ValueError, OSError, urllib.error.URLError, subprocess.SubprocessError) as error:
        result.update(_recover(record, plan["prior_identity"], previous_id, api, str(error),
                               plan["snapshot"], _deployment_id(work)))
    else:
        payload = work / "activation"
        payload.mkdir()
        state = {**plan["current"], "current": record["archive_sha256"], "previous": plan["prior"]}
        try:
            result["snapshot_commit"] = store.commit(api, plan["snapshot"], payload, state)
            result["status"] = "published"
        except (ValueError, OSError, subprocess.SubprocessError) as error:
            result.update(status="recovery-required", error="Healthy candidate; snapshot activation unconfirmed: " + str(error))
    (work / "result.json").write_bytes(site.canonical(result))
    site.require(result["status"] == "published", "rollback did not publish; inspect result.json")
    return result


def _verified_candidate(artifact_id: int, expected: str, work: Path) -> None:
    meta = github_json(f"{store.REPO}/actions/artifacts/{artifact_id}")
    site.require(not meta["expired"] and meta["name"] == "docs-candidate-" + os.environ["GITHUB_RUN_ATTEMPT"]
                 and meta["workflow_run"]["id"] == int(os.environ["GITHUB_RUN_ID"])
                 and meta["workflow_run"]["head_sha"] == os.environ["GITHUB_SHA"],
                 "candidate upload is not from this trusted run")
    candidate._download_checked(artifact_id, meta["digest"].removeprefix("sha256:"),
                                work / "candidate.zip", download, "rollback candidate upload")
    candidate._extract_verified(work / "candidate.zip", {"artifact.tar": expected}, work / "candidate-verified")


def _deployment_id(work: Path) -> str | None:
    path = work / "candidate-deployment.json"
    return site.read_json(path)["deployment_id"] if path.exists() else None


def _recover(record, prior_identity, previous_id, api, error, expected_tip=None, deployment_id=None,
             recovery_identity=None):
    try:
        site.require(expected_tip is not None and api.tip() == expected_tip,
                     "snapshot branch advanced or recovery scope is unknown")
        site.require(deployment_id is not None, "candidate deployment identity is unknown")
        # SHA-based Pages status can describe an earlier artifact at the same commit.
        site.require(not site.SHA.fullmatch(str(deployment_id)),
                     "SHA deployment status is not bound to the current artifact request")
        candidate_state = api.call(store.REPO + "/pages/deployments/" + str(deployment_id))["status"]
        site.require(candidate_state in {"succeed", "deployment_failed", "deployment_content_failed",
                                         "deployment_cancelled", "deployment_lost"},
                     "candidate Pages request is still in flight")
        if _live_identity(prior_identity):
            site.require(candidate_state != "succeed", "successful deployment has not shown a stable live identity")
            return {"status": "unchanged", "error": error}
        observed = live_marker()
        if observed is None or observed.get("manifest_sha256") != record["manifest_sha256"]:
            return {"status": "recovery-required", "error": error, "observed": observed}
        site.require(api.tip() == expected_tip, "snapshot branch advanced before recovery")
        deployment_id = deploy(previous_id, record["workflow_commit"], api)
        _wait_for_live(recovery_identity or prior_identity)
        return {"status": "restored", "error": error, "recovery_deployment_id": deployment_id}
    except (ValueError, OSError, urllib.error.URLError, subprocess.SubprocessError) as failure:
        return {"status": "recovery-required", "error": error, "recovery_error": str(failure)}


def seed_recovery(reference: str, expected: str, approved: dict, work: Path, api=None) -> str:
    api = api or store.GitHub()
    validate_bootstrap(approved)
    site.require(approved is not None, "seed needs explicit initial release recovery approval")
    site.require(api.tip() is None, "recovery seed is only allowed before the snapshot branch exists")
    site.empty(work)
    record = fetch_composition(reference, expected, work)
    site.require(record["base_snapshot_commit"] is None and record["previous_composition"] is None
                 and record["archive_sha256"] == approved["recovery_archive_sha256"],
                 "seed differs from approved standalone recovery composition")
    selected = site.read_json(work / "payload/selection.json")
    class Payload:
        def file(self, commit, name):
            return (work / "payload" / site.safe_path(name)).read_bytes()
    _recovery_sources(record, selected, work, Payload(), None)
    before = {"kind": "legacy", "index_sha256": approved["observed_index_sha256"]}
    site.require(_live_identity(before), "observed pre-marker site has changed")
    durable, archive = _retain_composition(record, work)
    prefix = durable / "compositions" / record["archive_sha256"]
    (prefix / "bootstrap.json").write_bytes(site.canonical(approved))
    state = {"schema": 1, "releases": selected["releases"], "current": None, "previous": None}
    tip = store.commit(api, None, durable, state)
    store.restore(archive, work / "retained.tar", lambda name: api.file(tip, name))
    return tip


def _automatic_trigger() -> dict:
    store.automatic_context()
    event = site.read_json(Path(os.environ["GITHUB_EVENT_PATH"]))
    trigger = event.get("workflow_run") or {}
    run_id, attempt = trigger.get("id"), trigger.get("run_attempt")
    site.require(type(run_id) is int and 0 < run_id <= 9_007_199_254_740_991
                 and type(attempt) is int and 0 < attempt <= 9_007_199_254_740_991,
                 "automatic docs need a specific source attempt")
    run = github_json(f"{store.REPO}/actions/runs/{run_id}/attempts/{attempt}")
    source = run.get("head_sha", "")
    site.require(bool(site.SHA.fullmatch(source))
                 and run.get("head_repository", {}).get("full_name") == "jwmcglynn/donner",
                 "automatic docs source repository differs")
    pages_artifact.verify_source_run(run, {"run_id": run_id, "attempt": attempt,
                                         "source_commit": source})
    return {"source_commit": source, "run_id": run_id, "attempt": attempt}


def _source_order(previous: str, incoming: str) -> bool:
    site.require(bool(site.SHA.fullmatch(previous)) and bool(site.SHA.fullmatch(incoming)),
                 "invalid automatic source commit")
    if incoming == previous:
        return False
    compared = github_json(f"{store.REPO}/compare/{previous}...{incoming}")
    if compared.get("status") == "behind":
        return False
    site.require(compared.get("status") == "ahead"
                 and compared.get("merge_base_commit", {}).get("sha") == previous,
                 "automatic main source does not descend from the published source")
    return True


def _verify_auto_source(value: dict) -> None:
    selected = _automatic_trigger()
    site.require(all(value[key] == selected[key] for key in selected),
                 "automatic candidate differs from the successful triggering source")
    head = github_json(f"{store.REPO}/git/ref/heads/main")["object"]["sha"]
    if head != selected["source_commit"]:
        site.require(_source_order(selected["source_commit"], head),
                     "automatic source is no longer on main")


def _automatic_scope(record: dict, selected: dict, current: dict, manifest: dict) -> None:
    published = {key: value["identity"] for key, value in manifest["versions"].items() if key != "main"}
    site.require(published == {key: value["identity"] for key, value in current["releases"].items()},
                 "automatic publication cannot activate a pending manual release")
    site.require(current["current"] is not None and record.get("bootstrap") is None
                 and selected.get("bootstrap") is None and selected["releases"] == current["releases"],
                 "automatic publication cannot bootstrap or change the release roster")
    site.require(set(selected["source_inputs"]) == set(current["releases"]) | {"main"}
                 and all(selected["source_inputs"][version] == entry["identity"]
                         for version, entry in current["releases"].items()),
                 "automatic release source differs from the current signed roster")
    site.require(record["base_snapshot_commit"] == selected["base_snapshot_commit"]
                 and record["previous_composition"] == selected["previous_composition"],
                 "automatic assembly provenance differs")


def _automatic_ready(ready: bool, reason: str) -> None:
    output = os.environ.get("GITHUB_OUTPUT")
    if output:
        with open(output, "a", encoding="utf-8") as stream:
            stream.write("ready=" + str(ready).lower() + "\n")
    print(reason)


def prepare_auto(work: Path, api=None) -> None:
    api = api or store.GitHub()
    source = _automatic_trigger()
    snapshot = api.tip()
    state = store.read_index(api, snapshot) if snapshot is not None else {"current": None}
    if state["current"] is None:
        _automatic_ready(False, "Initial versioned publication has not been activated; skipping.")
        return
    _restore_composition(api, snapshot, state["current"], work / "current")
    manifest = site.read_json(work / "current/site/_docs/versions.json")
    if not _source_order(manifest["versions"]["main"]["identity"]["source_commit"], source["source_commit"]):
        _automatic_ready(False, "The triggering source is already published or superseded; skipping.")
        return
    prepare(work / "candidate", f"{source['run_id']}/{source['attempt']}", "", "", snapshot, api,
            pinned_snapshot=True)
    _automatic_ready(True, "Prepared a complete site with unchanged release snapshots.")


def _bundle_attestation(path: Path, bundle: Path, commit: str, output: Path) -> None:
    site.require(bundle.is_file() and not bundle.is_symlink() and bundle.stat().st_size <= site.MAX_FILE,
                 "automatic attestation bundle is missing or oversized")
    with output.open("wb") as stream:
        _attestation_command(["gh", "attestation", "verify", str(path), "--bundle", str(bundle),
                        "--repo", "jwmcglynn/donner", "--signer-workflow", "jwmcglynn/donner/" + WORKFLOW,
                        "--source-digest", commit, "--source-ref", "refs/heads/main",
                        "--deny-self-hosted-runners", "--format", "json"], stdout=stream)


def _auto_artifact(name: str, work: Path, output: Path) -> None:
    run_id = int(os.environ["GITHUB_RUN_ID"])
    artifacts = github_json(f"{store.REPO}/actions/runs/{run_id}/artifacts?per_page=100")["artifacts"]
    selected = [value for value in artifacts if value["name"] == name and value["expired"] is False]
    site.require(len(selected) == 1, "automatic handoff artifact is missing or ambiguous")
    archive = work / (name + ".zip")
    candidate._download_checked(selected[0]["id"], selected[0]["digest"].removeprefix("sha256:"),
                                archive, download, "automatic handoff")
    _extract_payload(archive, output)


def _fetch_auto_composition(expected: str, work: Path) -> dict:
    store.automatic_context()
    site.require(bool(site.DIGEST.fullmatch(expected)), "invalid automatic composition digest")
    run_id, attempt = int(os.environ["GITHUB_RUN_ID"]), int(os.environ["GITHUB_RUN_ATTEMPT"])
    commit = os.environ["GITHUB_SHA"]
    run = github_json(f"{store.REPO}/actions/runs/{run_id}/attempts/{attempt}")
    site.require(run.get("id") == run_id and run.get("run_attempt") == attempt
                 and run.get("path") == WORKFLOW and run.get("event") == "workflow_run"
                 and run.get("head_branch") == "main" and run.get("head_sha") == commit
                 and run.get("repository", {}).get("full_name") == "jwmcglynn/donner"
                 and run.get("status") == "in_progress", "automatic handoff is not this trusted workflow run")
    _auto_artifact(f"docs-site-payload-{attempt}", work, work / "payload")
    _auto_artifact(f"docs-site-attestation-{attempt}", work, work / "proof")
    site.require(set(site.inventory(work / "proof")) == {"bundle.json"}, "unexpected automatic proof files")
    bundle = work / "proof/bundle.json"
    path = work / "payload/composition.json"
    site.require(bcr_source.digest(path) == expected, "automatic composition record differs")
    _bundle_attestation(path, bundle, commit, work / "composition-attestation.json")
    record = site.read_json(path)
    site.require(record["run_id"] == run_id and record["attempt"] == attempt
                 and record["workflow_commit"] == commit, "automatic composition identity differs")
    files = site.inventory(work / "payload", store.CHUNK)
    files.pop("composition.json")
    site.require(files == record["payload_files"], "automatic handoff payload changed")
    site.require(not (work / "payload/releases").exists() and not (work / "payload/objects").exists(),
                 "automatic handoff cannot introduce release storage")
    _fetch_pages(record, work, bundle)
    return record


def preflight_auto(expected: str, work: Path, api=None) -> None:
    api = api or store.GitHub()
    site.empty(work)
    record = _fetch_auto_composition(expected, work)
    selected = site.read_json(work / "payload/selection.json")
    snapshot = api.tip()
    current = store.index(api, snapshot)
    site.require(current["current"] is not None, "automatic publication cannot initialize the site")
    prior = current["current"]
    _, prior_identity = _previous(api, snapshot, prior, work)
    manifest = site.read_json(work / "prior-proof/site/_docs/versions.json")
    _automatic_scope(record, selected, current, manifest)
    _verify_auto_source(selected["source_inputs"]["main"])
    if not _source_order(manifest["versions"]["main"]["identity"]["source_commit"],
                         selected["source_inputs"]["main"]["source_commit"]):
        _automatic_ready(False, "A later source is already published; skipping stale candidate.")
        return
    site.require(_live_identity(prior_identity), "current durable composition is not the live site")
    plan = {"record": record, "current": current, "snapshot": snapshot, "prior": prior,
            "prior_identity": prior_identity, "recovery_identity": prior_identity, "automatic": True,
            "previous_archive_sha256": bcr_source.digest(work / "previous-pages/artifact.tar")}
    (work / "preflight.json").write_bytes(site.canonical(plan))
    _automatic_ready(True, "Verified the signed candidate against the current publication snapshot.")


REQUEST_FIELDS = {
    "compose": {"snapshot_commit", "main_run", "release_record_run", "release_record_sha256", "bootstrap"},
    "publish": {"composition_run", "composition_sha256", "prior_composition", "bootstrap"},
    "rollback": {"target_composition", "snapshot_commit", "prior_composition"},
    "seed": {"composition_run", "composition_sha256", "bootstrap"},
}


def workflow_request(mode: str) -> dict:
    raw = os.environ["DOCS_REQUEST"]
    site.require(len(raw) <= 4096, "docs request is oversized")
    value = json.loads(raw)
    site.require(isinstance(value, dict) and set(value) == REQUEST_FIELDS[mode], "unexpected docs request fields")
    for key, item in value.items():
        if key == "bootstrap":
            validate_bootstrap(item)
        else:
            site.require(isinstance(item, str), "invalid docs request value")
    return value


def workflow_context() -> None:
    site.require(os.environ.get("GITHUB_REPOSITORY") == "jwmcglynn/donner"
                 and os.environ.get("GITHUB_REF") == "refs/heads/main"
                 and os.environ.get("GITHUB_EVENT_NAME") == "workflow_dispatch",
                 "docs publication runs only through a manual trusted-main workflow")


def command_prepare(args):
    request = workflow_request("compose")
    value = request["snapshot_commit"]
    site.require(value == "none" or site.SHA.fullmatch(value), "snapshot commit must be pinned")
    prepare(args.work, request["main_run"], request["release_record_run"],
            request["release_record_sha256"], None if value == "none" else value,
            bootstrap=request["bootstrap"])


def command_preflight(args):
    request = workflow_request("publish")
    preflight(request["composition_run"], request["composition_sha256"],
              None if request["prior_composition"] == "none" else request["prior_composition"],
              args.work, bootstrap=request["bootstrap"])


def command_rollback_preflight(args):
    request = workflow_request("rollback")
    rollback_preflight(request["target_composition"], request["snapshot_commit"],
                       request["prior_composition"], args.work)


def command_seed(args):
    request = workflow_request("seed")
    seed_recovery(request["composition_run"], request["composition_sha256"],
                  request["bootstrap"], args.work)


def command_finalize(args):
    site.require(args.archive is not None and args.artifact_id is not None, "finalize inputs missing")
    finalize(args.work, args.archive, args.artifact_id)
    digest = bcr_source.digest(args.work / "payload/composition.json")
    output = os.environ.get("GITHUB_OUTPUT")
    if output:
        with open(output, "a", encoding="utf-8") as stream:
            stream.write("record_sha256=" + digest + "\n")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8") as output:
            output.write("Composition record SHA-256: `" + digest + "`\n")


def command_promote(args):
    site.require(args.artifact_id is not None, "verified rollback artifact ID required")
    promote(args.work, args.artifact_id)


def command_rollback(args):
    site.require(args.artifact_id is not None, "verified rollback artifact ID required")
    rollback(args.work, int(os.environ["VERIFIED_CANDIDATE_ARTIFACT_ID"]), args.artifact_id)


def main() -> None:
    commands = {"prepare": command_prepare, "finalize": command_finalize,
                "preflight": command_preflight, "promote": command_promote,
                "seed": command_seed, "rollback-preflight": command_rollback_preflight,
                "rollback": command_rollback, "prepare-auto": lambda args: prepare_auto(args.work),
                "finalize-auto": command_finalize,
                "preflight-auto": lambda args: preflight_auto(os.environ["COMPOSITION_SHA256"], args.work),
                "promote-auto": command_promote}
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=commands)
    parser.add_argument("--work", type=Path, required=True)
    parser.add_argument("--archive", type=Path)
    parser.add_argument("--artifact-id", type=int)
    args = parser.parse_args()
    if args.operation.endswith("-auto"):
        store.automatic_context()
    else:
        workflow_context()
    commands[args.operation](args)


if __name__ == "__main__":
    main()
