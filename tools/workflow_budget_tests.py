"""Holds the GitHub Actions workflow tree to the size ceilings in tools/workflow_budget.json.

Workflow files should wire jobs together, while the logic they run lives in tested scripts and
Bazel targets. The budget records, for every workflow and composite action and for the whole
tree, ceilings for the measures that grow when logic accumulates in YAML: lines, comment lines,
multi-line `run:` bodies, the longest `run:` body, `${{ }}` expressions inside `run:` bodies and
the longest `if:` expression. Every measure must equal its recorded value exactly: a change
that grows a file raises the budget as a visible, reviewed edit, and a change that shrinks one
lowers it in the same change. A `run:` or `if:` written in a form the reader does not model fails
instead of measuring as zero.
"""

import json
from pathlib import Path
import re
import unittest

from python.runfiles import runfiles

BUDGET = "tools/workflow_budget.json"
MEASURES = ("lines", "comment_lines", "multiline_runs", "longest_run", "run_expressions",
            "longest_if")
# Measures summed over the tree; the others are maxima.
SUMMED = ("lines", "comment_lines", "multiline_runs", "run_expressions")

_KEY = re.compile(r"^(?P<indent> *)(?:- )?(?P<key>[A-Za-z0-9_-]+):(?: +(?P<value>.*))?$")
_BLOCK = re.compile(r"^[|>][+-]?$")
# `run:` and `if:` forms the reader does not measure: quoted keys, flow mappings, and block
# scalars with an indentation indicator.
_UNMODELLED = (
    re.compile(r"""^\s*(?:- )?["'](?:run|if)["']\s*:"""),
    re.compile(r"""[{,]\s*["']?(?:run|if)["']?\s*:"""),
    re.compile(r"^\s*(?:- )?(?:run|if):\s+[|>][+-]?[1-9]"),
)
_DEFAULTS = re.compile(r"^\s+(?:shell|working-directory):")
_EXPRESSION = re.compile(r"\$\{\{.*?\}\}", re.DOTALL)


def _block(lines, start, indent):
    """The lines of a block scalar whose key line is lines[start] at `indent`, and its end."""
    end = start + 1
    while end < len(lines) and (not lines[end].strip()
                                or len(lines[end]) - len(lines[end].lstrip()) > indent):
        end += 1
    while end > start + 1 and not lines[end - 1].strip():
        end -= 1
    return lines[start + 1:end], end


# A quoted YAML scalar on one line: single-quoted with '' as an escaped quote, or double-quoted
# with backslash escapes.
_QUOTED = re.compile(r"'(?:[^']|'')*'" + r'|"(?:[^"\\]|\\.)*"')


def _scalar_value(text):
    """A one-line YAML value without its trailing comment.

    A quoted value ends at its closing quote, so a ` #` or an escaped quote inside it is part of
    the value. A quoted value that does not close on its line continues on the next one, which
    the check for multi-line values rejects.
    """
    text = text.strip()
    if text[:1] not in ("'", '"'):
        return text.split(" #", 1)[0].strip()
    match = _QUOTED.match(text)
    return match.group(0) if match else text


def _indent(line):
    return len(line) - len(line.lstrip())


def _check_modelled(lines, index, key, indent, value):
    """Raises ValueError for a `run:` or `if:` the measures would miss or undercount."""
    line = lines[index]
    if any(pattern.search(line) for pattern in _UNMODELLED):
        raise ValueError("line %d: unmodelled run: or if: form: %s" % (index + 1, line.strip()))
    if key not in ("run", "if") or _BLOCK.match(value):
        return
    following = next((other for other in lines[index + 1:] if other.strip()), "")
    # `defaults: run:` holds step settings, not a script.
    if key == "run" and not value and _DEFAULTS.match(following):
        return
    if _indent(following) > indent:
        raise ValueError("line %d: multi-line plain or quoted %s: value" % (index + 1, key))


def _entries(lines):
    """Each YAML comment line as (None, None, None) and each key as (key, value, block lines).

    A block scalar's lines, such as a `run: |` script, are its key's value, so a shell comment
    inside a script is not a YAML comment.
    """
    index = 0
    while index < len(lines):
        line = lines[index]
        match = _KEY.match(line)
        if line.lstrip().startswith("#"):
            yield None, None, None
            index += 1
            continue
        if not match:
            _check_modelled(lines, index, None, 0, "")
            index += 1
            continue
        indent = len(match.group("indent")) + (2 if line.lstrip().startswith("- ") else 0)
        value = _scalar_value(match.group("value") or "")
        _check_modelled(lines, index, match.group("key"), indent, value)
        body, end = _block(lines, index, indent) if _BLOCK.match(value) else ([], index + 1)
        yield match.group("key"), value, body
        index = end


def _count_comment(result, _value, _body):
    result["comment_lines"] += 1


def _count_run(result, value, body):
    result["run_expressions"] += len(_EXPRESSION.findall("\n".join(body) or value))
    result["multiline_runs"] += 1 if body else 0
    result["longest_run"] = max(result["longest_run"], len(body))


def _count_if(result, value, body):
    condition = " ".join(part.strip() for part in body) or value
    result["longest_if"] = max(result["longest_if"], len(condition))


_COUNTERS = {None: _count_comment, "run": _count_run, "if": _count_if}


def measure(text):
    """The budget measures of one workflow or action file."""
    lines = text.splitlines()
    result = dict.fromkeys(MEASURES, 0)
    result["lines"] = len(lines)
    for key, value, body in _entries(lines):
        if key in _COUNTERS:
            _COUNTERS[key](result, value, body)
    return result


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
        # Escaped double quotes do not end a double-quoted value.
        escaped = measure('      - run: "echo \\"${{ inputs.x }}\\" \\\\" # c\n'
                          '      - if: "a == \\"${{ b }}\\""\n')
        self.assertEqual(escaped["run_expressions"], 1)
        self.assertEqual(escaped["longest_if"], len('"a == \\"${{ b }}\\""'))
        # Siblings at the key's own indentation are not a continuation.
        self.assertEqual(measure("    steps:\n      - run: echo hi\n        shell: bash\n"
                                 "      - if: success()\n        uses: x\n")["longest_if"],
                         len("success()"))

    def test_the_measured_files_are_the_bazel_data(self):
        build = Path(runfiles.Create().Rlocation("donner/BUILD.bazel")).read_text()
        group = build.split('name = "github_action_definitions"', 1)[1].split("\n)\n", 1)[0]
        self.assertEqual(sorted(re.findall(r'"(\.github/[^"]+)"', group)), sorted([
            ".github/actions/*/action.yaml", ".github/actions/*/action.yml",
            ".github/workflows/*.yaml", ".github/workflows/*.yml"]))


if __name__ == "__main__":
    unittest.main()
