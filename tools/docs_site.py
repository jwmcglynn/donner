"""Compose independent Doxygen versions into a single verified Pages site."""
from __future__ import annotations

import argparse
import hashlib
import html
from html.parser import HTMLParser
import json
from pathlib import Path, PurePosixPath
import re
import shutil
import tarfile

from tools import bcr_source
from tools import pages_artifact

SCHEMA = 1
MAX_FILE = 16 * 1024 * 1024
MAX_SITE = 1_000_000_000
MAX_FILES = 100_000
MAX_VERSIONS = 32
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+\Z")
SHA = re.compile(r"[0-9a-f]{40}\Z")
DIGEST = re.compile(r"[0-9a-f]{64}\Z")
NUMBER = re.compile(r'(<span\b[^>]*\bid=["\']projectnumber["\'][^>]*>)(.*?)(</span\s*>)', re.S)
ROOT = Path(__file__).parent


def require(condition: object, message: str) -> None:
    if not condition:
        raise ValueError(message)


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def canonical(value: object) -> bytes:
    return (json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True) + "\n").encode()


def read_json(path: Path, maximum: int = MAX_FILE) -> dict:
    require(path.is_file() and not path.is_symlink() and path.stat().st_size <= maximum,
            "missing, linked or oversized JSON file")
    result = json.loads(path.read_bytes())
    require(isinstance(result, dict), "JSON must be an object")
    return result


def safe_path(value: str) -> str:
    path = PurePosixPath(value)
    require(bool(value) and not path.is_absolute() and str(path) == value
            and all(part not in {".", ".."} for part in path.parts)
            and not re.search(r"[\\\s%?#:\x00-\x1f\x7f]", value), "unsafe site path")
    return value


def empty(path: Path) -> None:
    require(not path.exists(), "output must not already exist")
    path.mkdir(parents=True)


class Page(HTMLParser):
    def __init__(self, text: str):
        super().__init__(convert_charrefs=True)
        self.anchors: set[str] = set()
        self.versions: list[str] = []
        self.in_number = False
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        values = dict(attrs)
        if values.get("id"):
            self.anchors.add(values["id"])
        if tag == "a" and values.get("name"):
            self.anchors.add(values["name"])
        if values.get("id") == "projectnumber":
            self.in_number = True

    def handle_endtag(self, tag):
        if tag == "span":
            self.in_number = False

    def handle_data(self, data):
        if self.in_number and data.strip():
            self.versions.append(data.strip())


def check_version(site: Path, version: str) -> int:
    require(bcr_source.VERSION.fullmatch(version), "invalid module version")
    checked = 0
    for path in site.rglob("*.html"):
        require(not path.is_symlink() and path.stat().st_size <= MAX_FILE, "unsafe HTML file")
        page = Page(path.read_text(encoding="utf-8"))
        if "projectnumber" in page.anchors:
            require(page.versions == [version], "generated project version differs: " + str(path))
            checked += 1
    require(checked > 0, "Doxygen site has no versioned pages")
    return checked


def unpack(archive: Path, output: Path) -> None:
    require(archive.is_file() and not archive.is_symlink() and archive.stat().st_size <= MAX_SITE,
            "missing or oversized site archive")
    empty(output)
    seen: set[str] = set()
    total = 0
    with tarfile.open(archive, "r:") as tar:
        for member in tar:
            normalized = pages_artifact.validate_member(member, seen)
            require(len(seen) <= MAX_FILES, "too many site files")
            if member.isdir():
                continue
            name = safe_path(str(normalized))
            total += member.size
            require(member.size <= MAX_FILE and total <= MAX_SITE, "site size limit exceeded")
            target = output / name
            target.parent.mkdir(parents=True, exist_ok=True)
            with tar.extractfile(member) as source, target.open("xb") as destination:
                shutil.copyfileobj(source, destination, 1024 * 1024)
    require((output / "index.html").is_file(), "site has no index.html")


def inventory(root: Path, max_file: int = MAX_FILE) -> dict[str, str]:
    result = {}
    total = 0
    for path in sorted(root.rglob("*")):
        require(not path.is_symlink(), "site contains a symlink")
        if path.is_dir():
            continue
        name = safe_path(path.relative_to(root).as_posix())
        total += path.stat().st_size
        require(path.is_file() and path.stat().st_size <= max_file and total <= MAX_SITE,
                "site size limit exceeded")
        result[name] = digest(path.read_bytes())
        require(len(result) <= MAX_FILES, "too many site files")
    return result


def validate_identity(value: dict) -> None:
    require(set(value) == {"version", "source_commit", "run_id", "attempt", "archive_sha256"},
            "unexpected source identity fields")
    require(bool(bcr_source.VERSION.fullmatch(value.get("version", "")))
            and bool(SHA.fullmatch(value.get("source_commit", "")))
            and bool(DIGEST.fullmatch(value.get("archive_sha256", ""))), "invalid source identity")
    for key in ("run_id", "attempt"):
        require(type(value.get(key)) is int and 0 < value[key] <= 9_007_199_254_740_991,
                "invalid source run identity")


def read_source(archive: Path, identity: dict, output: Path) -> None:
    validate_identity(identity)
    require(bcr_source.digest(archive) == identity["archive_sha256"], "source archive changed")
    pages_artifact.verify_archive(archive, identity["archive_sha256"], identity)
    unpack(archive, output)
    require(not any((output / name).exists() for name in ("_docs", "main", "docs-site.json")),
            "source already contains composition paths")
    require(not any(VERSION.fullmatch(p.name) for p in output.iterdir()),
            "source contains a release route")
    check_version(output, identity["version"])
    if (output / "docs-version.json").exists():
        require(read_json(output / "docs-version.json") == {"schema": 1,
                "semantic_version": identity["version"], "source_commit": identity["source_commit"]},
                "recorded docs version differs from selected source")


def _navigation(versions: list[str], current: str, base: str, identity: dict) -> str:
    options = "".join('<option value="%s"%s>%s</option>' % (
        key, ' selected="selected"' if key == current else "",
        "main (development)" if key == "main" else key + " (stable)" if key == versions[0] else key)
        for key in versions)
    links = " ".join('<a href="%s%s/index.html">%s</a>' % (base, key, key) for key in versions)
    return ('<div class="docs-version"><label for="docs-version-select">Documentation version</label>'
            '<select id="docs-version-select" disabled="disabled">' + options + '</select>'
            '<span class="docs-source">' + identity['version'] + ' · ' + identity['source_commit'][:8]
            + '</span><span class="docs-version-links">' + links + '</span></div>')


def version_links(text: str, current: str, versions: list[str], base: str) -> str:
    prefixes = ("https://jwmcglynn.github.io" + base, base)
    def replace(match):
        value = match[2]
        for prefix in prefixes:
            if value.startswith(prefix):
                remainder = value[len(prefix):]
                if remainder.split("/", 1)[0] in versions:
                    return match[0]
                return match[1] + base + current + "/" + remainder + match[3]
        return match[0]
    return re.sub(r'((?:href|src)=["\'])(.*?)(["\'])', replace, text)


def decorate(root: Path, current: str, identity: dict, versions: list[str], base: str) -> dict:
    pages = {}
    for path in sorted(root.rglob("*.html")):
        relative = path.relative_to(root).as_posix()
        text = version_links(path.read_text(encoding="utf-8"), current, versions, base)
        page = Page(text)
        anchor_bytes = canonical({"anchors": sorted(page.anchors)})
        anchor_name = "_docs/anchors/" + digest(relative.encode()) + ".json"
        target = root / anchor_name
        target.parent.mkdir(parents=True, exist_ok=True)
        require(len(anchor_bytes) <= 1024 * 1024, "anchor inventory is oversized")
        target.write_bytes(anchor_bytes)
        pages[relative] = {"anchors": anchor_name, "sha256": digest(anchor_bytes)}
        if "projectnumber" not in page.anchors:
            path.write_text(text, encoding="utf-8")
            continue
        label = identity["version"] if current != "main" else "main"
        number = NUMBER.search(text)
        require(number is not None, "Doxygen version span is malformed")
        text = NUMBER.sub(lambda m: m[1] + "&#160;" + label + m[3]
                          + _navigation(versions, current, base, identity), text, count=1)
        metadata = ('<link rel="stylesheet" href="' + base + '_docs/selector.css" />'
                    '<script defer="defer" src="' + base + '_docs/selector.js"'
                    ' data-docs-current="' + current + '" data-docs-base="' + base + '"'
                    ' data-docs-page="' + html.escape(relative, quote=True) + '"></script>')
        if current == "main":
            metadata += '<meta name="docs-source" content="' + identity["version"] + ' ' + identity["source_commit"] + '" />'
        text = text.replace("</head>", metadata + "</head>", 1)
        require(metadata in text, "HTML has no head")
        path.write_text(text, encoding="utf-8")
    data = canonical({"schema": SCHEMA, "pages": pages})
    require(len(data) <= 8 * 1024 * 1024, "page inventory is oversized")
    (root / "_docs/pages.json").write_bytes(data)
    return {"identity": identity, "pages_sha256": digest(data)}


def pack(root: Path, destination: Path) -> str:
    require(not destination.exists(), "archive output already exists")
    with tarfile.open(destination, "w:", format=tarfile.PAX_FORMAT) as tar:
        for name in inventory(root):
            path = root / name
            info = tar.gettarinfo(str(path), arcname=name)
            info.uid = info.gid = info.mtime = 0
            info.uname = info.gname = ""
            info.mode = 0o644
            with path.open("rb") as source:
                tar.addfile(info, source)
    return bcr_source.digest(destination)


def compose(sources: dict[str, tuple[Path, dict]], stable: str, output: Path,
            base: str = "/donner/") -> dict:
    require(VERSION.fullmatch(stable) and stable in sources and "main" in sources,
            "composition needs a stable release and main")
    require(2 <= len(sources) <= MAX_VERSIONS and all(k == "main" or VERSION.fullmatch(k)
            for k in sources), "invalid documentation versions")
    require(re.fullmatch(r"/(?:[A-Za-z0-9_-]+/)*", base), "invalid site base")
    empty(output)
    versions = [stable, "main", *sorted(k for k in sources if k not in {stable, "main"})]
    records = {}
    originals = {}
    for key in versions:
        archive, identity = sources[key]
        require(key == "main" or identity["version"] == key, "release route differs from module version")
        read_source(archive, identity, output / key)
        originals[key] = digest(canonical(inventory(output / key)))
        records[key] = decorate(output / key, key, identity, versions, base)
    shutil.copytree(output / stable, output, dirs_exist_ok=True)
    for path in output.rglob("*.html"):
        relative = path.relative_to(output)
        if relative.parts[0] in sources:
            continue
        text = path.read_text(encoding="utf-8")
        canonical_link = '<link rel="canonical" href="' + base + stable + '/' + relative.as_posix() + '" />'
        path.write_text(text.replace("</head>", canonical_link + "</head>", 1), encoding="utf-8")
    chrome = {}
    for name in ("selector.js", "selector.css"):
        data = (ROOT / "docs" / name).read_bytes()
        (output / "_docs" / name).write_bytes(data)
        chrome[name] = digest(data)
    manifest = {"schema": SCHEMA, "stable": stable, "base": base, "versions": records,
                "original_content": originals, "chrome": chrome}
    (output / "_docs/versions.json").write_bytes(canonical(manifest))
    files = inventory(output)
    marker = {"schema": SCHEMA, "manifest_sha256": digest(canonical(manifest)), "files": files}
    (output / "docs-site.json").write_bytes(canonical(marker))
    inventory(output)
    return marker


def verify_site(root: Path, expected_manifest: str | None = None) -> dict:
    marker = read_json(root / "docs-site.json")
    require(set(marker) == {"schema", "manifest_sha256", "files"} and marker["schema"] == SCHEMA,
            "invalid composition marker")
    actual = inventory(root)
    actual.pop("docs-site.json")
    require(actual == marker["files"], "composition files changed")
    manifest_bytes = (root / "_docs/versions.json").read_bytes()
    require(digest(manifest_bytes) == marker["manifest_sha256"]
            and (expected_manifest is None or marker["manifest_sha256"] == expected_manifest),
            "composition manifest changed")
    manifest = json.loads(manifest_bytes)
    require(manifest["schema"] == SCHEMA and manifest["stable"] in manifest["versions"]
            and "main" in manifest["versions"], "composition is missing a version")
    return marker


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("operation", choices=("check-version", "compose", "verify"))
    parser.add_argument("--site", type=Path, required=True)
    parser.add_argument("--module", type=Path, default=Path("MODULE.bazel"))
    parser.add_argument("--selection", type=Path)
    parser.add_argument("--archive", type=Path)
    args = parser.parse_args()
    if args.operation == "check-version":
        version = bcr_source.module_values(args.module.read_text())["version"]
        print("Validated", check_version(args.site, version), "Doxygen pages at", version)
        (args.site / "docs-version.json").write_bytes(canonical({"schema": 1, "semantic_version": version,
                                                              "source_commit": bcr_source.git("rev-parse", "HEAD")}))
    elif args.operation == "verify":
        verify_site(args.site)
    else:
        require(args.selection is not None and args.archive is not None, "compose needs selection and archive")
        selection = read_json(args.selection)
        sources = {key: (args.selection.parent / value["archive"], value["identity"])
                   for key, value in selection["sources"].items()}
        compose(sources, selection["stable"], args.site)
        print("Composed Pages archive", pack(args.site, args.archive))


if __name__ == "__main__":
    main()
