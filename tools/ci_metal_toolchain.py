"""Verify the selected Xcode's offline Metal compiler before native CI work."""

import argparse
import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile


def diagnostics(result):
    """Keep a bounded tail from both compiler streams, including timeout output."""
    streams = []
    for value in (result.stdout, result.stderr):
        if isinstance(value, bytes):
            value = value.decode("utf-8", errors="replace")
        if value:
            streams.append(value)
    return "\n".join(streams)[-4000:]


def run_tool(command, timeout, description):
    """Run one fixed setup command while retaining diagnostics if it times out."""
    try:
        return subprocess.run(command, capture_output=True, text=True,
                              timeout=timeout, check=False)
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"{description} timed out after {timeout}s: " + diagnostics(error)) from error


def probe(directory, lookup_options=()):
    """Compile an actual kernel, rather than accepting the downloadable tool's shim."""
    source = directory / "probe.metal"
    output = directory / "probe.air"
    source.write_text("kernel void donnerProbe() {}\n")
    output.unlink(missing_ok=True)
    result = run_tool(
        ["/usr/bin/xcrun", *lookup_options, "--sdk", "macosx", "metal", "-std=metal3.0", "-c", str(source), "-o", str(output)],
        30, "Metal compiler probe",
    )
    return result, output.is_file()


def ensure_compiler(install_missing=False):
    """Download only a reported missing component, then require successful compilation."""
    with tempfile.TemporaryDirectory(prefix="donner-metal-") as temporary:
        directory = Path(temporary)
        result, emitted = probe(directory)
        if result.returncode == 0 and emitted:
            return False
        diagnostic = diagnostics(result)
        if not install_missing or "missing Metal Toolchain" not in diagnostic:
            raise RuntimeError(f"Offline Metal compiler probe failed (exit={result.returncode}, AIR={'present' if emitted else 'missing'}): " + diagnostic)
        installed = run_tool(
            ["/usr/bin/xcodebuild", "-downloadComponent", "MetalToolchain"],
            600, "Metal Toolchain download",
        )
        if installed.returncode != 0:
            raise RuntimeError(f"Metal Toolchain download failed (exit={installed.returncode}): " + diagnostics(installed))
        result, emitted = probe(directory)
        if result.returncode != 0 or not emitted:
            raise RuntimeError(f"Metal compiler still cannot compile after component preparation (exit={result.returncode}, AIR={'present' if emitted else 'missing'}): " + diagnostics(result))
        return True


def component_status():
    """Read a public-field receipt without exposing asset paths or raw status output."""
    try:
        result = subprocess.run(
            ["/usr/bin/xcodebuild", "-showComponent", "MetalToolchain", "-json"],
            capture_output=True, text=True, timeout=30, check=False,
        )
    except subprocess.TimeoutExpired:
        return {"status": "query-timeout", "timeout_seconds": 30}
    except OSError:
        return {"status": "query-unavailable"}
    if result.returncode != 0:
        return {"status": "query-failed", "exit_code": result.returncode}
    try:
        data = json.loads(result.stdout)
    except (TypeError, ValueError):
        return {"status": "invalid-response"}
    if not isinstance(data, dict):
        return {"status": "invalid-response"}
    receipt = {"status": data.get("status") if data.get("status") in
               ("installed", "available", "not_installed", "not installed", "unavailable")
               else "unrecognized"}
    for key, pattern in (("buildVersion", r"[0-9]{1,3}[A-Z][0-9]{1,6}[a-z]?"),
                         ("toolchainIdentifier", r"com\.apple\.dt\.toolchain\.Metal(?:\.[0-9]+){1,6}")):
        value = data.get(key)
        if isinstance(value, str) and re.fullmatch(pattern, value):
            receipt[key] = value
    return receipt


def lookup_probe_status():
    """Compare two fixed lookup modes without changing the required compiler gate."""
    receipt = {}
    with tempfile.TemporaryDirectory(prefix="donner-metal-lookup-") as temporary:
        directory = Path(temporary)
        for name, options in (("uncached", ("--no-cache",)),
                              ("named_metal", ("--toolchain", "Metal"))):
            try:
                result, emitted = probe(directory, options)
                receipt[name] = {"exit_code": result.returncode, "air_output": emitted}
            except (OSError, RuntimeError):
                receipt[name] = {"status": "probe-unavailable"}
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--install-missing", action="store_true")
    mode.add_argument("--describe", action="store_true")
    args = parser.parse_args()
    if args.describe:
        receipt = component_status()
        try:
            receipt["lookup_probes"] = lookup_probe_status()
        except OSError:
            receipt["lookup_probes"] = {"status": "probe-setup-unavailable"}
        print("Metal component status: " + json.dumps(receipt, sort_keys=True))
        return 0
    try:
        installed = ensure_compiler(args.install_missing)
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        print("::error::" + str(error), file=sys.stderr)
        return 1
    print("Offline Metal compilation passed" + (" after component preparation" if installed else ""))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
