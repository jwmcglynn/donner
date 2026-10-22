"""Check that a product NOTICE attributes every third-party component in its closure.

Reads the coverage report, manifest and NOTICE text that `donner_closure_notice_file` writes for
one product. Fails when the product's configured closure reaches a component with neither a
license catalog entry nor a reviewed exemption, when the NOTICE does not carry exactly the
catalog entries that cover the closure, or when a component the closure must reach is missing,
which would mean the traversal stopped working.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys


def _uncovered(coverage: dict) -> list[str]:
    return [
        f"{component} is linked into {coverage['target']} but has no license catalog entry "
        f"(reached through {', '.join(labels)}). Add a license() for it to "
        "//third_party/licenses and ALL_KNOWN_LICENSES, or, if it puts no third-party code "
        "or data in the binary, an exempt_labels entry with the reason for each such label."
        for component, labels in sorted(coverage["uncovered"].items())
    ] + [
        f"linker option {entry.partition(' ')[2]!r} from {entry.partition(' ')[0]} links a "
        "library or file that no label in the closure names. Attribute what it links, or add "
        "an exempt_linkopts entry with the reason."
        for entry in coverage["unexempt_linkopts"]
    ]


def _missing_required(coverage: dict, required: list[str]) -> list[str]:
    problems = []
    for component in required:
        if component not in coverage["components"]:
            problems.append(f"the closure of {coverage['target']} no longer reaches {component}")
        elif component not in coverage["covered"]:
            problems.append(f"required component {component} is not covered by a catalog entry")
    return problems


def _notice_mismatches(coverage: dict, manifest: dict, notice: str) -> list[str]:
    covering = {label for labels in coverage["covered"].values() for label in labels}
    attributed = {entry["label"] for entry in manifest["licenses"]}
    problems = [] if covering else ["the NOTICE covers no third-party component"]
    problems += [f"the NOTICE manifest is missing {label}" for label in sorted(covering - attributed)]
    problems += [f"the NOTICE manifest lists {label}, which the closure does not reach"
                 for label in sorted(attributed - covering)]
    names = [entry.get("package_name") or entry["label"] for entry in manifest["licenses"]]
    problems += [f"the NOTICE text has no section for {name}" for name in names
                 if f"\nPackage: {name}\n" not in notice]
    return problems


def _component_file(label: str, component: str) -> str:
    repository, separator, path = label.partition("//")
    if not separator:
        raise ValueError("component label has no repository boundary")
    if repository in ("", "@", "@@"):
        prefix = "third_party/" + component + ":"
        if not path.startswith(prefix):
            raise ValueError("component label is outside its vendored package")
        return path[len(prefix):]
    canonical = [part for part in repository.lstrip("@").split("+") if part]
    if not canonical or canonical[-1] != component or not path.startswith(":"):
        raise ValueError("component label is outside its external repository")
    return path[1:]


def _exact_component_files(coverage: dict, requirements) -> list[str]:
    expected = {}
    for entry in requirements:
        component, separator, path = entry.partition("=")
        if not separator or not component or not path:
            raise ValueError("exact component files require COMPONENT=PATH")
        expected.setdefault(component, set()).add(path)
    problems = []
    for component, files in expected.items():
        try:
            actual = {_component_file(label, component)
                      for label in coverage.get("component_labels", {}).get(component, [])}
        except ValueError:
            actual = set()
        if actual != files:
            problems.append("the reviewed file set changed for component " + component)
    return problems


def check(coverage: dict, manifest: dict, notice: str, required: list[str],
          exact_files=()) -> list[str]:
    """Returns one message per problem; an empty list means the NOTICE covers the closure."""
    return (_uncovered(coverage) + _missing_required(coverage, required) +
            _notice_mismatches(coverage, manifest, notice) +
            _exact_component_files(coverage, exact_files))


def _expected_failures(problems: list[str], expected: list[str]) -> int:
    """For a fixture that must fail: passes only when every expected problem is reported."""
    missing = [text for text in expected if not any(text in problem for problem in problems)]
    for text in missing:
        print(f"FAIL: expected a problem containing {text!r}; got {problems}", file=sys.stderr)
    if not missing:
        print(f"reported the expected problems: {problems}")
    return 1 if missing else 0


def _kind(path: Path) -> str:
    if path.name.endswith(".coverage.json"):
        return "coverage"
    return "manifest" if path.suffix == ".json" else "notice"


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--outputs", type=Path, nargs="+", required=True,
                        help="the NOTICE text, manifest and coverage report of one product")
    parser.add_argument("--require", action="append", default=[])
    parser.add_argument("--exact-component-file", action="append", default=[],
                        help="require the complete reviewed label set, as COMPONENT=PATH")
    parser.add_argument("--expect-failure", action="append", default=[],
                        help="pass only if a reported problem contains each of these texts")
    args = parser.parse_args(argv)
    outputs = {_kind(path): path for path in args.outputs}
    if set(outputs) != {"coverage", "manifest", "notice"} or len(args.outputs) != 3:
        parser.error("--outputs needs exactly one NOTICE .txt, manifest .json and .coverage.json")
    coverage = json.loads(outputs["coverage"].read_text(encoding="utf-8"))
    manifest = json.loads(outputs["manifest"].read_text(encoding="utf-8"))
    notice = outputs["notice"].read_text(encoding="utf-8")
    problems = check(coverage, manifest, notice, args.require, args.exact_component_file)
    if args.expect_failure:
        return _expected_failures(problems, args.expect_failure)
    for problem in problems:
        print(f"FAIL: {problem}", file=sys.stderr)
    if problems:
        return 1
    print(f"{coverage['target']}: NOTICE attributes every third-party component "
          f"({', '.join(sorted(coverage['covered']))}); exempt: "
          f"{', '.join(sorted(coverage['exempt'])) or 'none'}; exemptions not reached in this "
          f"configuration: {', '.join(coverage['unreached_exemptions']) or 'none'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
