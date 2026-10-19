"""Shared executable stubs for shell-fixture tests."""

from pathlib import Path
import shlex
import sys
import tempfile

_PYTHON = shlex.quote(sys.executable)


def write_python_stand_in(path, script, *leading_args):
    """Writes an executable at `path` that runs the Python file `script` on the test's runtime.

    `leading_args` come before the caller's arguments, so one recording script can stand in for
    several tools and still tell them apart. `path` may be anywhere, for example inside a fixture
    repository where a script under test expects a sibling tool.
    """
    path = Path(path)
    path.write_text("#!/bin/sh\nexec %s %s %s\"$@\"\n" % (
        _PYTHON, shlex.quote(str(script)),
        "".join(shlex.quote(str(argument)) + " " for argument in leading_args)),
        encoding="utf-8")
    path.chmod(0o755)
    return path


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
        return self.write("python3", "#!/bin/sh\nexec %s \"$@\"\n" % _PYTHON)

    def write_python_entry(self, name, script):
        """Writes an executable `name` that runs the Python file `script` on the test's runtime.

        A `#!/usr/bin/env python3` shebang would resolve a host interpreter instead.
        """
        return self.write(name, "#!/bin/sh\nexec %s %s \"$@\"\n"
                          % (_PYTHON, shlex.quote(str(script))))

    def cleanup(self):
        self._temporary.cleanup()
