"""Shared executable stubs for shell-fixture tests."""

from pathlib import Path
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

    def cleanup(self):
        self._temporary.cleanup()
