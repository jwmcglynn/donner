"""Run mutation and replay without exposing the reporter's credentials."""

import os
import shutil
from pathlib import Path


def fuzzer_command(binary, arguments, *, read_only=(), writable=()):
    """Return a fail-closed Linux sandbox command when FUZZ_SANDBOX=1."""
    binary = Path(binary).resolve(strict=True)
    if os.environ.get("FUZZ_SANDBOX") != "1":
        return [str(binary), *map(str, arguments)]
    tool = shutil.which("bwrap")
    if not tool:
        raise RuntimeError("FUZZ_SANDBOX requires bubblewrap")
    command = [tool, "--unshare-all", "--die-with-parent", "--new-session", "--clearenv"]
    for name in ("/usr", "/lib", "/lib64", "/etc/ld.so.cache", "/etc/fonts"):
        if Path(name).exists():
            command += ["--ro-bind", name, name]
    command += ["--symlink", "usr/bin", "/bin", "--proc", "/proc", "--dev", "/dev",
                "--tmpfs", "/tmp", "--setenv", "PATH", "/usr/bin:/bin",
                "--setenv", "HOME", "/nonexistent", "--setenv", "LANG", "C.UTF-8"]
    repo = Path(os.environ.get("FUZZ_REPO_DIR", Path(__file__).resolve().parents[2])).resolve()
    command += ["--ro-bind", str(repo), str(repo)]
    if (repo / ".git").is_dir():
        command += ["--tmpfs", str(repo / ".git")]
    elif (repo / ".git").exists():
        command += ["--ro-bind", "/dev/null", str(repo / ".git")]
    # Only this build's public dependency sources are visible, not other output
    # bases, the home directory, host /run, SSH agents or GitHub credentials.
    for parent in binary.parents:
        if parent.name == "execroot":
            external = parent.parent / "external"
            if external.is_dir():
                command += ["--ro-bind", str(external), str(external)]
            break
    runfiles = Path(str(binary) + ".runfiles")
    if runfiles.exists():
        command += ["--ro-bind", str(runfiles), str(runfiles),
                    "--setenv", "RUNFILES_DIR", str(runfiles),
                    "--setenv", "TEST_SRCDIR", str(runfiles)]
    for path in read_only:
        path = Path(path).resolve(strict=True)
        command += ["--ro-bind", str(path), str(path)]
    for path in writable:
        path = Path(path).resolve(strict=True)
        command += ["--bind", str(path), str(path)]
    command += ["--ro-bind", str(binary), "/fuzzer", "--chdir", str(repo),
                "--setenv", "UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1"]
    symbolizer = shutil.which("llvm-symbolizer")
    if symbolizer and str(Path(symbolizer).resolve()).startswith("/usr/"):
        command += ["--setenv", "ASAN_SYMBOLIZER_PATH", str(Path(symbolizer).resolve())]
    return command + ["--", "/fuzzer", *map(str, arguments)]
