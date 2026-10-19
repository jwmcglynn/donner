"""Shared executable stubs for shell-fixture tests."""

from pathlib import Path
import shlex
import sys
import tempfile


class ExecutableFixtureDirectory:
    """Owns immutable executable stubs reused by every test in a class."""

    def __init__(self):
        self._temporary = tempfile.TemporaryDirectory()
        self.path = Path(self._temporary.name)

    def write(self, name, body):
        path = self.path / name
        path.write_text(body, encoding="utf-8")
        path.chmod(0o755)
        return path

    def write_python3(self):
        """Puts the test's own declared Python runtime on PATH as `python3`.

        Workflow and CI shell scripts call a bare `python3`. A test that runs them must not
        resolve that name from the host: remote executors and minimal images need not provide
        one, and a host interpreter would differ from the toolchain the test declares.
        """
        return self.write("python3", "#!/bin/sh\nexec %s \"$@\"\n" % shlex.quote(sys.executable))

    def cleanup(self):
        self._temporary.cleanup()
