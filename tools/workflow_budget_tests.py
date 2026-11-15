"""Holds the GitHub Actions workflow tree to the size ceilings in tools/workflow_budget.json.

Workflow files should wire jobs together, while the logic they run lives in tested scripts and
Bazel targets. The budget records, for every workflow and composite action and for the whole
tree, ceilings for the measures that grow when logic accumulates in YAML: lines, comment lines,
multi-line `run:` bodies, the longest `run:` body, `${{ }}` expressions inside `run:` bodies and
the longest `if:` expression. Every measure must equal its recorded value exactly: a change
that grows a file raises the budget as a visible, reviewed edit, and a change that shrinks one
lowers it in the same change.

The reader accepts only a subset of YAML whose values it never has to interpret, and fails on
everything else, so logic cannot grow in a form it would measure as zero:
- block mappings with plain keys, and block sequences;
- one-line scalars: plain, single-quoted, or double-quoted without any backslash;
- literal or folded block scalars with default indentation (`|`, `|-`, `>`, `>-`);
- flat flow sequences (and `{}`) only as the value of the keys in FLOW_KEYS, never naming
  `run`, `if` or `uses`. A test checks the extracted values against a YAML parser.
"""

import json
from pathlib import Path
import random
import re
import unittest

from python.runfiles import runfiles

BUDGET = "tools/workflow_budget.json"
MEASURES = ("lines", "comment_lines", "multiline_runs", "longest_run", "run_expressions",
            "longest_if")
# Measures summed over the tree; the others are maxima.
SUMMED = ("lines", "comment_lines", "multiline_runs", "run_expressions")
# Keys whose value may be a flat flow sequence such as `[a, b]`: job and trigger wiring and the
# matrix axes the workflows use. None of them holds logic.
FLOW_KEYS = ("branches", "bazel", "instance", "linker", "needs", "options", "os", "permissions",
             "runs-on", "types", "workflows")

_KEY = re.compile(r"^(?P<indent> *)(?P<dash>(?:- +)?)(?P<key>[A-Za-z0-9_.-]+)"
                  r":(?: +(?P<rest>.*)|$)")
_QUOTED_KEY = re.compile(r"""^ *(?:- +)?(?:'(?:[^']|'')*'|"[^"]*")\s*:(?:\s|$)""")
_ITEM = re.compile(r"^(?P<indent> *)- +(?P<rest>.*)$")
_BLOCK = re.compile(r"^(?P<style>[|>])(?P<chomp>-?)(?: +#.*)?$")
# One-line quoted scalars the reader takes literally: single-quoted ('' is the only escape) and
# double-quoted without a backslash.
_QUOTED = re.compile(r"'(?:[^']|'')*'|\"[^\"\\\\]*\"")
_TRAILER = re.compile(r"^(?: +#.*)?$")
_FLOW_ITEM = r"\s*(?:[^\s\[\]{},'\"#&*!|>%@`][^\[\]{},'\"#]*?|'(?:[^']|'')*'|\"[^\"\\\\]*\")\s*"
_FLOW = re.compile(r"^(?:\[(?:%s(?:,%s)*)?\]|\{\})(?: +#.*)?$" % (_FLOW_ITEM, _FLOW_ITEM))
_LOGIC_TOKEN = re.compile(r"\b(?:run|if|uses)\b")
_EXPRESSION = re.compile(r"\$\{\{.*?\}\}", re.DOTALL)


class Entry:
    """One `key: value` entry: `raw` is the value as written, `text` its value."""

    def __init__(self, key, kind, raw, text, body=()):
        self.key, self.kind, self.raw, self.text, self.body = key, kind, raw, text, list(body)


def _indent(line):
    return len(line) - len(line.lstrip(" "))


def _parse_block(text, where):
    block = _BLOCK.match(text)
    if not block:
        raise ValueError("%s: block scalar with an indentation indicator or keep chomping" % where)
    return "block" + block.group("style") + block.group("chomp"), text, ""


def _parse_quoted(text, where):
    quoted = _QUOTED.match(text)
    if not quoted or not _TRAILER.match(text[quoted.end():]):
        raise ValueError("%s: quoted value that does not close on its line, contains a "
                         "backslash, or has text after it" % where)
    raw = quoted.group(0)
    return "quoted", raw, raw[1:-1].replace("''", "'") if raw[0] == "'" else raw[1:-1]


def _parse_unsupported(_text, where):
    raise ValueError("%s: flow collection, anchor, alias, tag or reserved indicator" % where)


_VALUE_PARSERS = {**dict.fromkeys("|>", _parse_block), **dict.fromkeys("'\"", _parse_quoted),
                  **dict.fromkeys("[{&*!%@`", _parse_unsupported)}


def _parse_value(text, where, key=None):
    """(kind, raw value, value) of a one-line value. Raises ValueError outside the subset."""
    if not text or text.startswith("#"):
        return "empty", "", ""
    if key in FLOW_KEYS and text[0] in "[{" and _FLOW.match(text) \
            and not _LOGIC_TOKEN.search(text):
        return "flow", text, text
    if text[0] in _VALUE_PARSERS:
        return _VALUE_PARSERS[text[0]](text, where)
    plain = text.split(" #", 1)[0].rstrip()
    if ": " in plain or plain.endswith(":"):
        raise ValueError("%s: plain value containing a mapping indicator" % where)
    return "plain", plain, plain


def _block_body(lines, start, indent):
    """The lines of a block scalar that follows lines[start] at `indent`, and the next index."""
    end = start + 1
    while end < len(lines) and (not lines[end].strip() or _indent(lines[end]) > indent):
        end += 1
    while end > start + 1 and not lines[end - 1].strip():
        end -= 1
    return lines[start + 1:end], end


def _decode_block(kind, body):
    content = [line for line in body if line.strip()]
    if not content:
        return ""
    width = min(_indent(line) for line in content)
    text = "\n".join(line[width:] for line in body)
    if kind.startswith("block>"):
        text = " ".join(text.split())
    return text if kind.endswith("-") else text + "\n"


def _continues(lines, index, indent):
    """Whether a content line after lines[index] is indented past `indent`."""
    for line in lines[index + 1:]:
        if line.strip() and not line.lstrip().startswith("#"):
            return _indent(line) > indent
    return False


def _line_parts(line, where):
    """(key, column of the value's parent, value text) for a mapping key or sequence item."""
    if _QUOTED_KEY.match(line):
        raise ValueError("%s: quoted keys are not read" % where)
    key = _KEY.match(line)
    if key:
        return (key.group("key"), len(key.group("indent")) + len(key.group("dash")),
                key.group("rest") or "")
    item = _ITEM.match(line)
    if item:
        return None, len(item.group("indent")), item.group("rest")
    raise ValueError("%s: line outside the accepted YAML subset: %s" % (where, line.strip()))


def entries(text):
    """Every comment line (key "#") and every key or sequence item of a workflow, in order."""
    lines = text.splitlines()
    index = 0
    while index < len(lines):
        line = lines[index]
        where = "line %d" % (index + 1)
        if not line.strip() or line.strip() == "---":
            index += 1
            continue
        if line.lstrip().startswith("#"):
            yield Entry("#", "comment", line, line)
            index += 1
            continue
        key, column, rest = _line_parts(line, where)
        kind, raw, value = _parse_value(rest.strip(), where, key)
        body, end = [], index + 1
        if kind.startswith("block"):
            body, end = _block_body(lines, index, column)
            value = _decode_block(kind, body)
        elif kind != "empty" and _continues(lines, index, column):
            raise ValueError("%s: value that continues on a following line" % where)
        yield Entry(key, kind, raw, value, body)
        index = end


def _count_run(result, entry):
    result["run_expressions"] += len(_EXPRESSION.findall("\n".join(entry.body) or entry.raw))
    if entry.body:
        result["multiline_runs"] += 1
        result["longest_run"] = max(result["longest_run"], len(entry.body))


def _count_if(result, entry):
    condition = " ".join(part.strip() for part in entry.body) or entry.raw
    result["longest_if"] = max(result["longest_if"], len(condition))


def _count_comment(result, _entry):
    result["comment_lines"] += 1


_COUNTERS = {"#": _count_comment, "run": _count_run, "if": _count_if}


def measure(text):
    """The budget measures of one workflow or action file."""
    result = dict.fromkeys(MEASURES, 0)
    result["lines"] = len(text.splitlines())
    for entry in entries(text):
        if entry.key in _COUNTERS and entry.kind != "empty":
            _COUNTERS[entry.key](result, entry)
    return result


def logic_values(text):
    """(key, decoded value) of every `run:` and `if:` that holds a value, in file order."""
    return [(entry.key, entry.text) for entry in entries(text)
            if entry.key in ("run", "if") and entry.kind != "empty"]


def _scalar_text(value):
    return str(value).lower() if isinstance(value, bool) else str(value)


def _parsed_values(node):
    """(key, value) of every scalar `run:` and `if:` in a parsed YAML document, in order."""
    if isinstance(node, list):
        return [found for item in node for found in _parsed_values(item)]
    if not isinstance(node, dict):
        return []
    found = []
    for key, value in node.items():
        if str(key) in ("run", "if") and not isinstance(value, (dict, list)):
            found.append((str(key), _scalar_text(value)))
        found += _parsed_values(value)
    return found


def _comparable(key, value):
    """A value as both readers agree on it: `if:` expressions compare up to whitespace."""
    return key, " ".join(value.split()) if key == "if" else value


def measure_tree(root):
    """Measures of every workflow and composite action under `root` (.github), by path."""
    paths = sorted((root / "workflows").glob("*.y*ml"))
    paths += sorted((root / "actions").glob("*/action.y*ml"))
    measured = {}
    for path in paths:
        name = str(path.relative_to(root.parent))
        try:
            measured[name] = measure(path.read_text(encoding="utf-8"))
        except ValueError as error:
            raise ValueError("%s: %s" % (name, error)) from None
    return measured


def tree_totals(files):
    return {name: (sum if name in SUMMED else max)(values[name] for values in files.values())
            for name in MEASURES}


def mismatches(measured, budget):
    """Each measure that differs from its recorded value, and each file missing from a side."""
    problems = ["%s has no budget; add its measures to %s" % (path, BUDGET)
                for path in sorted(set(measured) - set(budget["files"]))]
    problems += ["%s no longer exists; remove it from %s" % (path, BUDGET)
                 for path in sorted(set(budget["files"]) - set(measured))]
    checked = [(path, values, budget["files"][path]) for path, values in measured.items()
               if path in budget["files"]]
    checked.append(("the workflow tree", tree_totals(measured), budget["tree"]))
    for name, values, recorded in checked:
        for measure_name in MEASURES:
            actual, expected = values[measure_name], recorded[measure_name]
            if actual > expected:
                problems.append("%s: %s is %d, above its budget of %d" % (
                    name, measure_name, actual, expected))
            elif actual < expected:
                problems.append("%s: %s is %d, below its budget of %d; lower the budget" % (
                    name, measure_name, actual, expected))
    return problems


# Tricky but valid YAML forms of a step, each with the reader's verdict: the measures it must
# count, or REJECTED when the form is outside the accepted subset. None is silently miscounted.
REJECTED = "rejected"
# Every escape a YAML double-quoted scalar knows, including Unicode spellings of `$`, `{`, `}`,
# a quote and a line break. The reader takes no double-quoted value with a backslash.
_DOUBLE_QUOTE_ESCAPES = ("\\0", "\\a", "\\b", "\\t", "\\n", "\\v", "\\f", "\\r", "\\e",
                         "\\ ", "\\\"", "\\/", "\\\\", "\\N", "\\_", "\\L", "\\P", "\\x24",
                         "\\u0024", "\\U00000024", "\\u007b", "\\u007d", "\\u0022", "\\u000a")
_STEP_FORMS = (
    # One-line values containing ` #`, `{`, `[`, `:` and quotes.
    ("- run: 'it''s ${{ a.b }} # not a comment' # comment ${{ c }}", {"run_expressions": 1}),
    ("- run: 'echo \\\" # ${{ a }}'", {"run_expressions": 1}),
    ("- run: \"echo '{run: x}' [if: y] a:b ${{ c }}\" # ${{ comment }}",
     {"run_expressions": 1}),
    ("- run: echo {run:x} [if:y] a:b ${{ c }}", {"run_expressions": 1}),
    ("- run: echo a: b", REJECTED),
    ("- if: \"a == ' # ' && ${{ b }}\"", {"longest_if": len("\"a == ' # ' && ${{ b }}\"")}),
    ("- if: ${{ a == 'x' }} # ${{ comment }}", {"longest_if": len("${{ a == 'x' }}")}),
    # Double-quoted values with any escape.
    *[("- run: \"echo %s${{ a }}\"" % escape, REJECTED) for escape in _DOUBLE_QUOTE_ESCAPES],
    *[("- if: \"%s{{ a }}\"" % escape, REJECTED) for escape in _DOUBLE_QUOTE_ESCAPES],
    # Flow collections: only flat sequences as the value of a wiring key, never naming logic.
    ("- needs: [a, 'b', \"c\"]\n  run: echo ${{ a }}", {"run_expressions": 1}),
    ("- permissions: {}\n  run: echo", {"run_expressions": 0}),
    ("- needs: [run]", REJECTED),
    ("- needs: ['uses']", REJECTED),
    ("- needs: [a, [b]]", REJECTED),
    ("- needs: {a: b}", REJECTED),
    ("- with: {args: \"[run: x]\", n: 1}", REJECTED),
    ("- env: [a, '{if: b}']", REJECTED),
    ("- {name: x, run: echo ${{ a }}}", REJECTED),
    ("- {\"run\": \"echo ${{ inputs.x }}\"}", REJECTED),
    ("- {'if': \"${{ a }}\"}", REJECTED),
    ("- with: {a: 1, if: true}", REJECTED),
    ("- with: [{run: x}]", REJECTED),
    ("- [run, x]", REJECTED),
    ("- with:\n    a: [b]", REJECTED),
    ("- needs: [a,\n    b]", REJECTED),
    # Block scalars: default indentation only.
    ("- run: |-\n    echo ${{ a }}\n    echo \\\" b", {"multiline_runs": 1, "longest_run": 2,
                                                         "run_expressions": 1}),
    ("- if: >\n    a &&\n    b", {"longest_if": len("a && b")}),
    ("- run: |2\n    echo hi", REJECTED),
    ("- run: |+\n    echo hi", REJECTED),
    ("- run: >+\n    echo hi", REJECTED),
    # Values that continue on another line, at any indentation.
    ("- run: \"echo one\n  ${{ a }}\"", REJECTED),
    ("- run: \"echo one\n    ${{ a }}\"", REJECTED),
    ("- run: 'echo one\n  ${{ a }}'", REJECTED),
    ("- run: echo one\n    ${{ a }}", REJECTED),
    ("- run: echo one\n  ${{ a }}", REJECTED),
    ("- if: a &&\n    b", REJECTED),
    # Keys and nodes the reader does not resolve.
    ("- \"run\": echo hi", REJECTED),
    ("- 'if': true", REJECTED),
    ("- name: x\n  \"run\": echo", REJECTED),
    ("- name: x\n  'uses': a@v1", REJECTED),
    ("- \"name\": x", REJECTED),
    ("- <<: *defaults", REJECTED),
    ("- run: *script", REJECTED),
    ("- run: !!str echo", REJECTED),
    ("- name: &anchor x", REJECTED),
)


class _SnippetGenerator:
    """Small step snippets from a fixed seed, each with the run: and if: values it holds when it
    is in the reader's subset, or None when it uses a form outside it."""

    _WORDS = ("echo", "a", "${{ inputs.x }}", "x{y}", "[z]", "a:b", "c#d", "'q'", "$HOME")
    _QUOTED_WORDS = ("${{ b }}", "' # '", "{run: x}", "[if: y]", "a: b", "#", "\\d")

    def __init__(self, seed):
        self._random = random.Random(seed)

    def _words(self, pool, count):
        return " ".join(self._random.choice(pool) for _ in range(count))

    def _value(self):
        """(lines of a value after `key:`, its value or None when outside the subset)."""
        words = "echo " + self._words(self._WORDS, self._random.randint(1, 4))
        more = self._words(self._WORDS, self._random.randint(1, 3))
        quoted = self._words(self._QUOTED_WORDS, self._random.randint(1, 3)).replace("'", "")
        comment = self._random.choice(("", " # note ${{ c }}"))
        forms = (
            lambda: ([words + comment], words),
            lambda: (["'%s'%s" % (quoted.replace("x", "x''"), comment)], quoted.replace("x", "x'")),
            lambda: (['"%s"%s' % (quoted.replace("\\", ""), comment)], quoted.replace("\\", "")),
            lambda: (['"%s%s"' % (self._random.choice(_DOUBLE_QUOTE_ESCAPES), quoted)], None),
            lambda: (["|", "  " + words, "  " + quoted], words + "\n" + quoted + "\n"),
            lambda: ([">-", "  " + words, "  " + more], words + " " + more),
            lambda: (["|2", "  " + words], None),
            lambda: ([words, "    " + more], None),
            lambda: (["{a: %s}" % words.replace(",", "")], None),
            lambda: (["'%s" % quoted, "  end'"], None),
        )
        # Forms in the subset come up twice as often as each form outside it.
        return forms[self._random.choice((0, 0, 1, 1, 2, 2, 3, 4, 4, 5, 5, 6, 7, 8, 9))]()

    def snippet(self):
        """(text, [(key, value)] or None)."""
        lines, values = [], []
        for _ in range(self._random.randint(1, 2)):
            key = self._random.choice(("run", "if", "name", "run", "if", '"run"', "'if'"))
            value_lines, value = self._value()
            lines.append("      - %s: %s" % (key, value_lines[0]))
            lines += ["          " + line for line in value_lines[1:]]
            if key[0] in "'\"" or value is None or values is None:
                values = None
            elif key in ("run", "if"):
                values.append((key, value))
        return "jobs:\n  a:\n    steps:\n" + "\n".join(lines) + "\n", values


class WorkflowBudgetTest(unittest.TestCase):
    maxDiff = None

    def test_the_workflow_tree_stays_within_its_budget(self):
        resolver = runfiles.Create()
        github = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).parent.parent
        budget = json.loads(Path(resolver.Rlocation("donner/" + BUDGET)).read_text())
        measured = measure_tree(github)
        self.assertGreater(len(measured), 20, "workflow runfiles are missing")
        self.assertEqual(mismatches(measured, budget), [])
        for ceilings in [budget["tree"], *budget["files"].values()]:
            self.assertEqual(sorted(ceilings), sorted(MEASURES))

    def test_measures_follow_the_workflow_syntax(self):
        text = (
            "# A comment.\n"
            "on: push\n"
            "jobs:\n"
            "  a:\n"
            "    if: ${{ github.event_name == 'push' }}\n"
            "    steps:\n"
            "      - run: echo ${{ github.sha }}\n"
            "      - name: Script\n"
            "        if: >-\n"
            "          a &&\n"
            "          b\n"
            "        run: |\n"
            "          # A shell comment is part of the script.\n"
            "          echo \"${{ inputs.x }}\"\n"
            "\n"
            "          echo done\n"
            "\n"
            "      - name: Other block\n"
            "        with:\n"
            "          script: |\n"
            "            # Not a YAML comment either.\n"
            "            ${{ inputs.y }}\n"
        )
        self.assertEqual(measure(text), {
            "lines": 22, "comment_lines": 1, "multiline_runs": 1, "longest_run": 4,
            "run_expressions": 2, "longest_if": len("${{ github.event_name == 'push' }}")})

    def test_every_difference_from_the_budget_is_reported(self):
        values = dict.fromkeys(MEASURES, 3)
        budget = {"tree": dict.fromkeys(MEASURES, 3), "files": {"a.yml": dict(values)}}
        self.assertEqual(mismatches({"a.yml": dict(values)}, budget), [])
        self.assertEqual(mismatches({"a.yml": dict(values, longest_run=4)}, budget), [
            "a.yml: longest_run is 4, above its budget of 3",
            "the workflow tree: longest_run is 4, above its budget of 3"])
        # A shrink must lower the budget in the same change, so it never silently loosens.
        self.assertEqual(mismatches({"a.yml": dict(values, lines=2)}, budget), [
            "a.yml: lines is 2, below its budget of 3; lower the budget",
            "the workflow tree: lines is 2, below its budget of 3; lower the budget"])
        self.assertEqual(mismatches({"b.yml": dict(values)}, budget), [
            "b.yml has no budget; add its measures to %s" % BUDGET,
            "a.yml no longer exists; remove it from %s" % BUDGET])

    def test_unmodelled_run_and_if_forms_fail(self):
        for text in (
                '      - "run": echo hi\n',
                "      - {name: x, run: echo hi}\n",
                "      - with: {a: 1, if: true}\n",
                "        run: |2\n            echo hi\n",
                "        if: >-1\n          a\n",
                "        run: echo one\n          echo two\n",
                "        if: github.ref == 'refs/heads/main' &&\n          success()\n",
                "        run:\n          echo plain\n",
                '        run: "echo one\n          two"\n'):
            with self.subTest(text=text), self.assertRaises(ValueError):
                measure("jobs:\n  a:\n    steps:\n" + text)
        self.assertEqual(measure("    defaults:\n      run:\n        shell: bash\n")["lines"], 3)
        # A ` #` inside a quoted value is not a comment, so the expressions after it count.
        quoted = measure("      - run: 'echo \" # ${{ inputs.x }}\"' # note\n"
                         "      - if: \"a == ' # ' && ${{ b }}\"\n")
        self.assertEqual(quoted["run_expressions"], 1)
        self.assertEqual(quoted["longest_if"], len("\"a == ' # ' && ${{ b }}\""))
        self.assertEqual(measure("      - run: 'it''s # ${{ x }}' # c\n")["run_expressions"], 1)
        self.assertEqual(measure("      - run: echo hi # ${{ not.run }}\n")["run_expressions"], 0)
        # A double-quoted value with an escape is outside the subset, whatever the escape.
        with self.assertRaises(ValueError):
            measure('      - run: "echo \\u0024{{ inputs.x }}"\n')
        # Siblings at the key's own indentation are not a continuation.
        self.assertEqual(measure("    steps:\n      - run: echo hi\n        shell: bash\n"
                                 "      - if: success()\n        uses: x\n")["longest_if"],
                         len("success()"))

    def test_tricky_forms_are_counted_correctly_or_rejected(self):
        for form, expected in _STEP_FORMS:
            step = "".join("      " + line + "\n" for line in form.split("\n"))
            text = "jobs:\n  a:\n    steps:\n" + step
            with self.subTest(form=form):
                if expected == REJECTED:
                    with self.assertRaises(ValueError):
                        measure(text)
                    continue
                measured = measure(text)
                self.assertEqual({name: measured[name] for name in expected}, expected)
                for name in set(MEASURES) - set(expected) - {"lines"}:
                    self.assertEqual(measured[name], 0, name)

    def test_extracted_values_match_a_yaml_parser(self):
        """The run: and if: values the reader extracts are the ones a YAML parser reads."""
        try:
            import yaml  # pylint: disable=import-outside-toplevel
        except ImportError:
            self.skipTest("no YAML parser is importable in this environment")
        resolver = runfiles.Create()
        github = Path(resolver.Rlocation("donner/.github/workflows/main.yml")).parent.parent
        paths = sorted((github / "workflows").glob("*.y*ml"))
        paths += sorted((github / "actions").glob("*/action.y*ml"))
        for path in paths:
            text = path.read_text(encoding="utf-8")
            with self.subTest(path=path.name):
                self.assertEqual([_comparable(*value) for value in logic_values(text)],
                                 [_comparable(*value) for value in
                                  _parsed_values(yaml.safe_load(text))])

    def test_generated_snippets_are_rejected_or_read_exactly(self):
        """A fixed-seed sample of step forms, in and outside the subset."""
        try:
            import yaml  # pylint: disable=import-outside-toplevel
        except ImportError:
            yaml = None
        generator = _SnippetGenerator(20261115)
        outcomes = {"rejected": 0, "read": 0}
        for index in range(400):
            text, expected = generator.snippet()
            with self.subTest(index=index, text=text):
                if expected is None:
                    with self.assertRaises(ValueError):
                        measure(text)
                    outcomes["rejected"] += 1
                    continue
                self.assertEqual(logic_values(text), expected)
                if yaml is not None:
                    self.assertEqual([_comparable(*value) for value in logic_values(text)],
                                     [_comparable(*value) for value in
                                      _parsed_values(yaml.safe_load(text))])
                outcomes["read"] += 1
        # The sample exercises both verdicts.
        self.assertGreater(min(outcomes.values()), 100, outcomes)

    def test_the_measured_files_are_the_bazel_data(self):
        build = Path(runfiles.Create().Rlocation("donner/BUILD.bazel")).read_text()
        group = build.split('name = "github_action_definitions"', 1)[1].split("\n)\n", 1)[0]
        self.assertEqual(sorted(re.findall(r'"(\.github/[^"]+)"', group)), sorted([
            ".github/actions/*/action.yaml", ".github/actions/*/action.yml",
            ".github/workflows/*.yaml", ".github/workflows/*.yml"]))


if __name__ == "__main__":
    unittest.main()
