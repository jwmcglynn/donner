"""Expands `--config=<name>` from the repository's .bazelrc the way Bazel does.

The CI policy tests compare and check Bazel command lines. A lane's flags, tag filters and
quarantine can live in a `.bazelrc` config, so the tests read a command line with each repository
config replaced by its flags, in Bazel's order, instead of trusting the config's name.

Bazel applies an rc line `<command>:<config> <flags>` to a command that is, or inherits from,
`<command>`. Within one config, the lines are applied in the command's inheritance order (`common`,
then `build`, then `test`, ...), each kind in file order, and a nested `--config` expands in
place. A config the repository does not define (a user or runner rc can add one) is kept by name.
Only the repository's own .bazelrc is read: its `import` and `try-import` files are generated or
user-local, and a policy test must not depend on them.
"""

import re
import shlex

# Each command's rc sections, in the order Bazel applies them. `fetch` reads `test` options, as
# Bazel's `--announce_rc` shows for `bazel fetch --config=ci`.
_INHERITANCE = {
    "build": ("always", "common", "build"),
    "test": ("always", "common", "build", "test"),
    "coverage": ("always", "common", "build", "test", "coverage"),
    "fetch": ("always", "common", "build", "test", "fetch"),
    "run": ("always", "common", "build", "run"),
    "aquery": ("always", "common", "build", "aquery"),
    "cquery": ("always", "common", "build", "test", "cquery"),
    "info": ("always", "common", "build", "info"),
    "mobile-install": ("always", "common", "build", "mobile-install"),
    "print_action": ("always", "common", "build", "print_action"),
}
_RC_LINE = re.compile(r"^([a-z_-]+):([A-Za-z0-9_.+-]+)\s+(.*?)\s*$")


def parse(text):
    """{config: [(rc command, [flags])]} for every `<command>:<config>` line, in file order."""
    configs = {}
    for line in text.splitlines():
        match = _RC_LINE.match(line)
        if match:
            configs.setdefault(match.group(2), []).append(
                (match.group(1), shlex.split(match.group(3), comments=True)))
    return configs


def sections(command):
    """The rc sections that apply to `command`, in Bazel's order."""
    return _INHERITANCE.get(command, ("always", "common", command))


def _config_name(options, index):
    """(name, options consumed) when options[index] is a `--config`, else (None, 0)."""
    option = options[index]
    if option.startswith("--config="):
        return option.split("=", 1)[1], 1
    if option == "--config" and index + 1 < len(options):
        return options[index + 1], 2
    return None, 0


def expand(command, options, configs, _active=()):
    """`options` with each repository `--config` replaced by the flags it applies to `command`."""
    expanded = []
    index = 0
    while index < len(options):
        name, consumed = _config_name(options, index)
        if name is None or name not in configs:
            expanded.extend(options[index:index + max(consumed, 1)])
            index += max(consumed, 1)
            continue
        if name in _active:
            raise ValueError("config %s expands to itself" % name)
        for section in sections(command):
            for kind, flags in configs[name]:
                if kind == section:
                    expanded.extend(expand(command, flags, configs, _active + (name,)))
        index += consumed
    return expanded


# Bazel commands, so that `bazel` in other text (a path, a message) is not read as a command.
COMMANDS = frozenset(("aquery", "build", "canonicalize-flags", "clean", "coverage", "cquery",
                      "dump", "fetch", "help", "info", "license", "mobile-install", "mod",
                      "print_action", "query", "run", "shutdown", "sync", "test", "vendor",
                      "version"))
_BAZEL_WORD = re.compile(r"(?:^|(?<=[\s(]))(?:bazelisk|bazel)\s+(.*)$")
# Shell operators that end one command on a line and start the next.
_SEPARATOR = re.compile(r"\s*(?:&&|\|\||;|\|)\s+")
_VARIABLE = re.compile(r"^\$(?:\{(\w+)\}|(\w+))$")


def _substitute(words, variables):
    """`words` with each whole-word `$NAME` or `${NAME}` in `variables` replaced by its words."""
    substituted = []
    for word in words:
        match = _VARIABLE.match(word)
        name = match and (match.group(1) or match.group(2))
        substituted.extend(variables[name].split() if name in variables else [word])
    return substituted


def _segment_command(segment, configs, variables):
    """(command, options, other words) for one Bazel command in a shell segment, or None."""
    match = _BAZEL_WORD.search(segment)
    if not match:
        return None
    words = _substitute(match.group(1).split(), variables)
    while words and words[0].startswith("--"):
        words.pop(0)
    if not words or words[0] not in COMMANDS:
        return None
    rest = expand(words[0], words[1:], configs)
    return (words[0], [word for word in rest if word.startswith("--")],
            [word for word in rest if not word.startswith("--")])


def commands(script, configs, variables=None):
    """(command, options, other words) for each `bazel`/`bazelisk` command in a shell script.

    Line continuations are joined, each command of a `&&`, `||`, `;` or `|` chain is read on its
    own, startup options are skipped, whole-word variables named in `variables` are replaced by
    their value, and each repository `--config` is replaced by its flags. Words that are not
    options (targets, unknown variables) are returned as written.
    """
    segments = [segment for line in re.sub(r"\\\n\s*", " ", script).splitlines()
                if not line.lstrip().startswith("#") for segment in _SEPARATOR.split(line)]
    found = (_segment_command(segment, configs, variables or {}) for segment in segments)
    return [command for command in found if command]


def last_value(options, name):
    """The value of the last `--<name>=` option, or None when it is absent."""
    values = [option.split("=", 1)[1] for option in options
              if option.startswith("--%s=" % name)]
    return values[-1] if values else None
