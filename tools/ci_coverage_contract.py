"""Checks that a trusted pull request ran every phase and test class main's hosted lanes run.

Trusted pull requests run most CI on self-hosted runners, while pushes to main run GitHub-hosted
lanes. A pull request whose lanes skipped something main runs can be green and still turn main
red. tools/ci/coverage_contract.json declares, for each main-hosted lane in main.yml, its phases
(steps), the classes of tests each phase covers, and the pull request steps that cover each class
on a trusted pull request. A class without a pull request counterpart must carry a reviewed
exception that names an owner. A lane whose hosted job itself ran on the pull request (its route
is off) needs no counterparts.

Each pull request job records a receipt of the phases it ran (`receipt`): every step's outcome
and, for steps that write a Bazel build event file, how many test targets reported a result and
how many of those executed rather than coming from a cache. The `check` command compares the
receipts with the manifest and fails when a class selected for this pull request has no
counterpart that ran (and reported its minimum number of test targets) and no exception.
"""

import argparse
import json
from pathlib import Path
import sys

# Pull request selections a class can depend on, from the determine-targets job. `always` holds
# whenever CI runs at all; a //... fallback reaches every class.
CONDITIONS = ("always", "browser_affected", "hosted_parity_affected",
              "hosted_parity_tests_affected")
# Routing outputs that say a lane's hosted job was replaced by self-hosted jobs on this pull
# request. A fallback does not change routing.
ROUTES = ("always", "self_hosted_macos")
# Step outcomes that mean the step ran its command (it may have failed; the job reports that).
EXECUTED = ("success", "failure")
RECEIPT_VERSION = 1


def _problem_list(prefix, items):
    return ["%s %s" % (prefix, item) for item in items]


def _counterpart_problems(where, counterparts):
    problems = []
    for counterpart in counterparts:
        keys = set(counterpart)
        if not {"job", "step"} <= keys or keys - {"job", "step", "min_tests"}:
            problems.append("%s has a malformed counterpart %r" % (where, counterpart))
        elif not isinstance(counterpart.get("min_tests", 0), int):
            problems.append("%s counterpart min_tests must be an integer" % where)
    return problems


def _exception_problems(where, exception):
    if exception is None:
        return []
    if set(exception) != {"owner", "decision", "reason"} or not all(
            isinstance(value, str) and value.strip() for value in exception.values()):
        return ["%s exception must name an owner, the decision and a reason" % where]
    return []


def _class_problems(where, entry):
    """Problems of one class entry: its condition, counterparts and exception."""
    problems = []
    if entry.get("when") not in CONDITIONS:
        problems.append("%s has an unknown condition %r" % (where, entry.get("when")))
    counterparts = entry.get("counterparts", [])
    exception = entry.get("exception")
    if bool(counterparts) == bool(exception):
        problems.append("%s needs either counterparts or an exception, not both or neither"
                        % where)
    problems += _counterpart_problems(where, counterparts)
    problems += _exception_problems(where, exception)
    if not isinstance(entry.get("pr_only", False), bool) or (
            entry.get("pr_only") and not counterparts):
        problems.append("%s pr_only must be true or false, and only on a class with "
                        "counterparts" % where)
    unknown = set(entry) - {"when", "counterparts", "exception", "description", "pr_only"}
    return problems + _problem_list("%s has unknown key" % where, sorted(unknown))


def manifest_problems(manifest):
    """Structural problems of a coverage manifest."""
    if manifest.get("version") != 1 or not isinstance(manifest.get("lanes"), dict):
        return ["manifest must be version 1 with a `lanes` mapping"]
    routes = manifest.get("routes", {})
    problems = ["%s has no route among %s" % (lane, ROUTES) for lane in sorted(manifest["lanes"])
                if routes.get(lane) not in ROUTES]
    for lane, phases in sorted(manifest["lanes"].items()):
        for phase, classes in sorted(phases.items()):
            if not classes:
                problems.append("%s / %s declares no classes" % (lane, phase))
            for name, entry in sorted(classes.items()):
                problems += _class_problems("%s / %s / %s" % (lane, phase, name), entry)
    return problems


def bep_counts(path):
    """Test targets that reported a result in a Bazel JSON build event file, and how many of
    them executed (any attempt not served from a local or remote cache)."""
    summaries = set()
    executed = set()
    with open(path, encoding="utf-8") as events:
        for line in events:
            if not line.strip():
                continue
            event = json.loads(line)
            identifier = event.get("id", {})
            if "testSummary" in identifier and "testSummary" in event:
                summaries.add(identifier["testSummary"]["label"])
            result = event.get("testResult")
            if "testResult" in identifier and result is not None:
                cached = (result.get("cachedLocally", False) or
                          result.get("executionInfo", {}).get("cachedRemotely", False))
                if not cached:
                    executed.add(identifier["testResult"]["label"])
    return {"tests": len(summaries), "executed": len(executed & summaries)}


def make_receipt(job, outcomes, beps):
    """A receipt for one job: each step's outcome and, where recorded, its test counts."""
    steps = {}
    for step, outcome in outcomes.items():
        entry = {"outcome": outcome}
        bep = beps.get(step)
        if outcome in EXECUTED and bep is not None and Path(bep).is_file():
            entry.update(bep_counts(bep))
        steps[step] = entry
    return {"version": RECEIPT_VERSION, "job": job, "steps": steps}


def _selected(condition, selection):
    return selection.get("fallback", False) or selection.get(condition, False)


def _counterpart_ran(counterpart, receipts):
    receipt = receipts.get(counterpart["job"], {})
    step = receipt.get("steps", {}).get(counterpart["step"])
    if step is None or step.get("outcome") not in EXECUTED:
        return False
    return step.get("tests", 0) >= counterpart.get("min_tests", 0)


def _routed(manifest, lane, selection):
    route = manifest["routes"][lane]
    return route == "always" or selection.get(route, False)


def _classes(manifest, selection):
    """(lane / phase / class, entry) for every class of a replaced lane, in order."""
    for lane, phases in sorted(manifest["lanes"].items()):
        if not _routed(manifest, lane, selection):
            continue
        for phase, classes in sorted(phases.items()):
            for name, entry in sorted(classes.items()):
                yield "%s / %s / %s" % (lane, phase, name), entry


def _exception_line(where, exception):
    return "%s: %s (owner %s, %s)" % (where, exception["reason"], exception["owner"],
                                      exception["decision"])


def _gap(where, entry):
    expected = ", ".join("%s / %s" % (c["job"], c["step"]) for c in entry["counterparts"])
    if entry.get("pr_only"):
        return "%s is required on this pull request but did not run; expected an executed %s" % (
            where, expected)
    return "%s ran on main but not on this pull request; expected an executed %s" % (
        where, expected)


def check(manifest, receipts, selection):
    """(problems, exceptions used) for these receipts and this pull request's selection.

    A problem is a selected class with no counterpart that ran and no exception.
    """
    problems = []
    excepted = []
    for where, entry in _classes(manifest, selection):
        if not _selected(entry["when"], selection):
            continue
        if entry.get("exception"):
            excepted.append(_exception_line(where, entry["exception"]))
        elif not any(_counterpart_ran(c, receipts) for c in entry["counterparts"]):
            problems.append(_gap(where, entry))
    return problems, excepted


def _pairs(values, flag):
    pairs = {}
    for value in values:
        name, separator, rest = value.partition("=")
        if not separator or not name:
            raise SystemExit("%s expects NAME=VALUE, got %r" % (flag, value))
        pairs[name] = rest
    return pairs


def _load_receipts(directory):
    receipts = {}
    for path in sorted(Path(directory).rglob("coverage-receipt.json")):
        receipt = json.loads(path.read_text(encoding="utf-8"))
        if receipt.get("version") != RECEIPT_VERSION or receipt.get("job") in receipts:
            raise SystemExit("unexpected or duplicate receipt %s" % path)
        receipts[receipt["job"]] = receipt
    return receipts


def _run_receipt(args):
    receipt = make_receipt(args.job, _pairs(args.step, "--step"), _pairs(args.bep, "--bep"))
    Path(args.out).parent.mkdir(parents=True, exist_ok=True)
    Path(args.out).write_text(json.dumps(receipt, indent=2, sort_keys=True) + "\n",
                              encoding="utf-8")
    return 0


def _run_check(args):
    manifest = json.loads(Path(args.manifest).read_text(encoding="utf-8"))
    structure = manifest_problems(manifest)
    if structure:
        print("\n".join(structure), file=sys.stderr)
        return 1
    selection = {name: value == "true" for name, value in _pairs(args.selection,
                                                                 "--selection").items()}
    # Every selection must be passed, even empty: a missing one would read as false and silently
    # drop the classes or the lane it gates.
    expected = (set(CONDITIONS) | set(ROUTES) | {"fallback"}) - {"always"}
    if set(selection) != expected:
        print("selections must be exactly %s, got %s" % (sorted(expected), sorted(selection)),
              file=sys.stderr)
        return 2
    selection["always"] = True
    receipts = _load_receipts(args.receipts)
    problems, excepted = check(manifest, receipts, selection)
    for line in excepted:
        print("Coverage exception: %s" % line)
    for line in problems:
        print("::error::Coverage gap: %s" % line)
    if not problems:
        print("Every selected main phase and class ran on this pull request or has a reviewed "
              "exception.")
    elif any(step.get("outcome") == "failure" for receipt in receipts.values()
             for step in receipt["steps"].values()):
        print("A lane failed above; fix it first, since a failed job can skip later steps.")
    return 1 if problems else 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    commands = parser.add_subparsers(dest="command", required=True)
    receipt = commands.add_parser("receipt", help="record the phases one job ran")
    receipt.add_argument("--job", required=True)
    receipt.add_argument("--out", required=True)
    receipt.add_argument("--step", action="append", default=[], help="STEP=OUTCOME")
    receipt.add_argument("--bep", action="append", default=[], help="STEP=BEP_JSON_PATH")
    compare = commands.add_parser("check", help="compare receipts with the manifest")
    compare.add_argument("--manifest", required=True)
    compare.add_argument("--receipts", required=True)
    compare.add_argument("--selection", action="append", default=[], help="CONDITION=true|false")
    args = parser.parse_args(argv)
    return _run_receipt(args) if args.command == "receipt" else _run_check(args)


if __name__ == "__main__":
    sys.exit(main())
