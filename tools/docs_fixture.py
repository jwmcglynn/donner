"""Build the real composer output used by the selector browser tests."""
import argparse
import io
from pathlib import Path
import tarfile
import tempfile
from tools import docs_site as site


def source(root: Path, channel: str) -> tuple[Path, dict]:
    version = "0.8.0" if channel == "release" else "0.9.0-pre"
    header = ('<!doctype html><html><head><title>Donner docs</title></head><body>'
              '<div id="top"><div id="projectname">Donner SVG '
              '<span id="projectnumber">' + version + '</span></div>'
              '<input aria-label="Search" /></div>')
    files = {"index.html": header + '<h1 id="intro">Introduction</h1></body></html>',
             "api/type.html": header + '<h1 id="member">API type</h1>'
             + ('<h2 id="new-section">Development section</h2>' if channel == "main" else '')
             + '</body></html>',
             "docs-build.json": site.canonical({"source_commit": "a" * 40,
                 "source_run_id": 123, "source_attempt": 1}).decode()}
    if channel == "main":
        files["main-only.html"] = header + '<h1>Development only</h1></body></html>'
    archive = root / (channel + ".tar")
    with tarfile.open(archive, "w:") as tar:
        for name, value in files.items():
            data = value.encode(); info = tarfile.TarInfo(name); info.size = len(data)
            tar.addfile(info, io.BytesIO(data))
    return archive, {"version": version, "source_commit": "a" * 40, "run_id": 123, "attempt": 1,
                     "archive_sha256": site.bcr_source.digest(archive)}


def main():
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        site.compose({"0.8.0": source(root, "release"), "main": source(root, "main")}, "0.8.0", root / "site")
        values = {name: (root / "site" / name).read_text() for name in site.inventory(root / "site")}
        args.output.write_bytes(site.canonical({"files": values}))


if __name__ == "__main__": main()
