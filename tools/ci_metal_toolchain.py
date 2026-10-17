"""Verify the selected Xcode's offline Metal compiler before native CI work."""

import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def probe(directory):
    """Compile an actual kernel, rather than accepting the downloadable tool's shim."""
    source = directory / "probe.metal"
    output = directory / "probe.air"
    source.write_text("kernel void donnerProbe() {}\n")
    output.unlink(missing_ok=True)
    result = subprocess.run(
        ["/usr/bin/xcrun", "--sdk", "macosx", "metal", "-std=metal3.0", "-c", str(source), "-o", str(output)],
        capture_output=True, text=True, timeout=30, check=False,
    )
    return result, output.is_file()


def ensure_compiler(install_missing=False):
    """Download only a reported missing component, then require successful compilation."""
    with tempfile.TemporaryDirectory(prefix="donner-metal-") as temporary:
        directory = Path(temporary)
        result, emitted = probe(directory)
        if result.returncode == 0 and emitted:
            return False
        diagnostic = (result.stdout or "") + (result.stderr or "")
        if not install_missing or "missing Metal Toolchain" not in diagnostic:
            raise RuntimeError("Offline Metal compiler probe failed: " + diagnostic[-4000:])
        installed = subprocess.run(
            ["/usr/bin/xcodebuild", "-downloadComponent", "MetalToolchain"],
            capture_output=True, text=True, timeout=600, check=False,
        )
        if installed.returncode != 0:
            raise RuntimeError("Metal Toolchain download failed: " + (installed.stdout + installed.stderr)[-4000:])
        result, emitted = probe(directory)
        if result.returncode != 0 or not emitted:
            raise RuntimeError("Metal compiler still cannot compile after component preparation")
        return True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--install-missing", action="store_true")
    args = parser.parse_args()
    try:
        installed = ensure_compiler(args.install_missing)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print("::error::" + str(error), file=sys.stderr)
        return 1
    print("Offline Metal compilation passed" + (" after component preparation" if installed else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
