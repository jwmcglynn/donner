#!/usr/bin/env python3
"""Conservative local guards for changed Python complexity and unsafe XML calls.

These guards cover recurring review failures; CodeFactor remains authoritative.
"""

import argparse
import ast
from collections import Counter
from pathlib import Path
import subprocess
import sys


COMPLEXITY_LIMIT = 15
DECISIONS = (ast.If, ast.For, ast.AsyncFor, ast.While, ast.IfExp, ast.ExceptHandler, ast.comprehension)
FUNCTIONS = (ast.FunctionDef, ast.AsyncFunctionDef)
UNSAFE_XML = {
    "xml.etree.ElementTree.parse", "xml.etree.ElementTree.iterparse",
    "xml.etree.ElementTree.fromstring", "xml.etree.ElementTree.XMLParser",
    "xml.etree.cElementTree.parse", "xml.etree.cElementTree.iterparse",
    "xml.etree.cElementTree.fromstring", "xml.etree.cElementTree.XMLParser",
    "xml.sax.parse", "xml.sax.parseString", "xml.sax.make_parser",
    "xml.dom.minidom.parse", "xml.dom.minidom.parseString",
    "xml.dom.pulldom.parse", "xml.dom.pulldom.parseString",
}


def decision_cost(node, depth=0):
    if isinstance(node, FUNCTIONS):
        return 0
    cost = 0
    nested = depth
    if isinstance(node, DECISIONS):
        cost = 1 + depth
        nested += 1
    elif isinstance(node, ast.BoolOp):
        cost = 1
    return cost + sum(decision_cost(child, nested) for child in ast.iter_child_nodes(node))


def definitions(nodes, prefix=""):
    for node in nodes:
        if isinstance(node, (ast.ClassDef, *FUNCTIONS)):
            name = prefix + node.name
            if isinstance(node, FUNCTIONS):
                yield name, node
            yield from definitions(node.body, name + ".")
        else:
            yield from definitions(ast.iter_child_nodes(node), prefix)


def imported_names(node):
    if isinstance(node, ast.Import):
        return [(item.asname or item.name.split(".")[0],
                 item.name if item.asname else item.name.split(".")[0]) for item in node.names]
    if isinstance(node, ast.ImportFrom) and node.module:
        return [(item.asname or item.name, node.module + "." + item.name) for item in node.names]
    return []


def aliases(tree):
    return dict(item for node in ast.walk(tree) for item in imported_names(node))


def qualified_name(node, names):
    if isinstance(node, ast.Name):
        return names.get(node.id, node.id)
    if isinstance(node, ast.Attribute):
        return qualified_name(node.value, names) + "." + node.attr
    return ""


def xml_calls(tree, functions):
    names = aliases(tree)
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        called = qualified_name(node.func, names)
        if called not in UNSAFE_XML:
            continue
        containers = [(name, function) for name, function in functions.items()
                      if function.lineno <= node.lineno <= function.end_lineno]
        owner = max(containers, key=lambda item: item[1].lineno)[0] if containers else "<module>"
        yield (owner, called, ast.dump(node, include_attributes=False)), node.lineno


def check_source(source, baseline=""):
    tree = ast.parse(source)
    previous = ast.parse(baseline)
    current_functions = dict(definitions(tree.body))
    old_functions = dict(definitions(previous.body))
    findings = []
    for name, function in current_functions.items():
        old = old_functions.get(name)
        if old is not None and ast.dump(function) == ast.dump(old):
            continue
        score = sum(decision_cost(statement) for statement in function.body)
        if score > COMPLEXITY_LIMIT:
            findings.append((function.lineno, "PY-COMPLEXITY",
                             f"{name} decision/nesting cost {score} exceeds {COMPLEXITY_LIMIT}"))
    previous_calls = Counter(key for key, _ in xml_calls(previous, old_functions))
    for key, line in xml_calls(tree, current_functions):
        if previous_calls[key]:
            previous_calls[key] -= 1
        else:
            findings.append((line, "PY-XML", f"Unsafe untrusted-XML entry point: {key[1]}"))
    return findings


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-ref", required=True)
    parser.add_argument("paths", nargs="+", type=Path)
    args = parser.parse_args()
    root = Path(subprocess.check_output(["git", "rev-parse", "--show-toplevel"], text=True).strip())
    baseline_ref = subprocess.check_output(
        ["git", "rev-parse", "--verify", args.base_ref + "^{commit}"], text=True).strip()
    count = 0
    for path in args.paths:
        relative = path.resolve().relative_to(root)
        old = subprocess.run(["git", "show", baseline_ref + ":" + relative.as_posix()],
                             capture_output=True, text=True)
        baseline = old.stdout if old.returncode == 0 else ""
        for line, code, message in check_source(path.read_text(encoding="utf-8"), baseline):
            print(f"{relative}:{line}: {code}: {message}")
            count += 1
    if count:
        return 1
    print(f"OK: {len(args.paths)} changed Python file(s) passed focused quality guards.")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, SyntaxError, subprocess.CalledProcessError) as error:
        print(f"Python quality check failed: {error}", file=sys.stderr)
        sys.exit(1)
