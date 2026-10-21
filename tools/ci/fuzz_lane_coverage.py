#!/usr/bin/env python3
"""Checks that the Fuzz workflow runs every fuzzer's soak.

Only the nightly Fuzz workflow (.github/workflows/fuzz.yml) mutates inputs: pull requests replay
each fuzzer's checked-in corpus but exclude its `{fuzzer}_soak` target. A fuzzer whose soak no
workflow step selects is therefore never fuzzed, and nothing fails to say so. This check queries
the Bazel graph and requires that:

  - every target that links the libFuzzer runtime belongs to a fuzzer with a `{fuzzer}_soak`
    target, which `donner_cc_fuzzer` defines;
  - some `tools/ci/fuzz_lane.py <lane> <patterns>` phase of the Fuzz workflow runs each soak: one
    of its patterns selects the soak (`//...` skips `manual` targets; a named target or test suite
    does not) and the lane's `--test_tag_filters` from .bazelrc admit the soak's tags;
  - each variant tag on a soak (a `fuzz_*` tag other than `fuzz_target` and `fuzz_soak`, such as
    `fuzz_text_full` or `fuzz_geode`) is selected by some step that runs the soak. Variant tags
    route a soak to the lane that builds its variant's configuration; a lane that merely admits
    the soak may find it incompatible and skip it. Each tag is checked on its own, so a soak with
    two variant tags needs a lane for each, not one lane with both configurations.

It reads the whole build graph, so it runs in tools/ci/lint_lane.sh rather than under
`bazel test //...`; //tools/ci:fuzz_lane_coverage_tests covers its rules hermetically.

Usage: tools/ci/fuzz_lane_coverage.py
"""

import json
from pathlib import Path
import re
import shlex
import subprocess
import sys

WORKFLOW = ".github/workflows/fuzz.yml"
BAZELRC = ".bazelrc"
SOAK_RULE = "_fuzzer_soak_test"
NON_VARIANT_TAGS = frozenset({"fuzz_target", "fuzz_soak"})
# A target links the libFuzzer runtime when its linkopts carry `-fsanitize=fuzzer` (not
# `-fsanitize=fuzzer-no-link`) or name the runtime archive. A configurable attribute matches when
# any of its branches does.
LIBFUZZER_LINKOPTS = r"fsanitize=fuzzer[],]|libclang_rt\.fuzzer"
# Suffixes of the libFuzzer-linked targets that `donner_cc_fuzzer` generates beside the soak.
FUZZER_SUFFIXES = ("_bin_impl", "_bin", "_impl", "")
# Target patterns that expand by package rather than by naming a target or test suite.
WILDCARDS = ("/...", ":all", ":*")
# A Fuzz workflow phase; //tools:security_workflow_policy_tests reads phases the same way.
LANE_STEP = re.compile(r"^\s*run: python3 tools/ci/fuzz_lane\.py (.*)$")


def bazelrc_configs(text):
    """config name -> [(command, flag)] in file order."""
    configs = {}
    for line in text.splitlines():
        match = re.match(r"^(build|test|common):(\S+)\s+(.*)$", line)
        if match:
            for flag in shlex.split(match.group(3), comments=True):
                configs.setdefault(match.group(2), []).append((match.group(1), flag))
    return configs


def lane_flags(configs, name):
    """A lane config's flags with nested `--config`s expanded, as `bazel test` applies them."""
    if name not in configs:
        raise ValueError("no .bazelrc config named " + name)
    flags = []
    for _, flag in configs[name]:
        nested = re.fullmatch(r"--config=(\S+)", flag)
        flags.extend(lane_flags(configs, nested.group(1)) if nested else [flag])
    return flags


def tag_filters(flags):
    """(included, excluded) tags of the last `--test_tag_filters` among the flags."""
    value = ""
    for flag in flags:
        if flag.startswith("--test_tag_filters="):
            value = flag.split("=", 1)[1]
    tags = [tag for tag in value.split(",") if tag]
    return (frozenset(tag for tag in tags if not tag.startswith("-")),
            frozenset(tag[1:] for tag in tags if tag.startswith("-")))


def workflow_steps(text):
    """(lane, patterns) for each `tools/ci/fuzz_lane.py LANE PATTERN...` phase of the workflow."""
    steps = []
    for line in text.splitlines():
        match = LANE_STEP.search(line)
        if match:
            lane, *patterns = shlex.split(match.group(1))
            steps.append((lane, patterns))
    return steps


def _wildcard_selects(pattern, label):
    if pattern == "//...":
        return True
    if pattern.endswith("/..."):
        package = label.split(":", 1)[0]
        prefix = pattern[:-len("/...")]
        return package == prefix or package.startswith(prefix + "/")
    if pattern.endswith(":all") or pattern.endswith(":*"):
        return label.split(":", 1)[0] == pattern.rsplit(":", 1)[0]
    return False


def step_runs(step, label, tags, expanded):
    """Whether a step with these filters and expanded patterns runs the soak."""
    included, excluded = step["filters"]
    if tags & excluded or (included and not tags & included):
        return False
    selected = False
    for pattern in step["patterns"]:
        negative = pattern.startswith("-")
        pattern = pattern.lstrip("-")
        if pattern.endswith(WILDCARDS):
            hit = _wildcard_selects(pattern, label) and "manual" not in tags
        else:
            hit = label in expanded.get(pattern, ())
        if hit:
            selected = not negative
    return selected


def _has_soak(target, soaks):
    package, name = target.split(":", 1)
    return any(name.endswith(suffix) and
               "%s:%s_soak" % (package, name[:len(name) - len(suffix)]) in soaks
               for suffix in FUZZER_SUFFIXES)


def _soak_problems(label, tags, steps, expanded):
    running = [step for step in steps if step_runs(step, label, tags, expanded)]
    if not running:
        return ["%s is not run by any Fuzz workflow step (tags: %s)" %
                (label, ", ".join(sorted(tags)))]
    selected = set().union(*(step["filters"][0] for step in running))
    variants = sorted(tags - NON_VARIANT_TAGS - selected)
    return ["%s carries %s, but no Fuzz workflow step that runs it selects that tag" %
            (label, variant) for variant in variants if variant.startswith("fuzz_")]


def problems(soaks, libfuzzer_targets, steps, expanded):
    """The fuzzers that the Fuzz workflow never soaks.

    soaks: soak label -> set of tags. libfuzzer_targets: labels that link the libFuzzer runtime.
    steps: [{"filters": (included, excluded), "patterns"}]. expanded: each pattern that
    is not a wildcard -> the test labels it selects.
    """
    found = ["%s links libFuzzer but has no soak target; define it with donner_cc_fuzzer" %
             target for target in sorted(libfuzzer_targets) if not _has_soak(target, soaks)]
    for label, tags in sorted(soaks.items()):
        found.extend(_soak_problems(label, tags, steps, expanded))
    return found


def _query(root, *args):
    command = ["bazelisk", "query", "--noshow_progress", *args]
    return subprocess.run(command, cwd=root, check=True, stdout=subprocess.PIPE,
                          text=True).stdout.splitlines()


def collect(root):
    """Queries the soaks, libFuzzer-linked targets and named step targets under root."""
    configs = bazelrc_configs((root / BAZELRC).read_text(encoding="utf-8"))
    steps = []
    for lane, patterns in workflow_steps((root / WORKFLOW).read_text(encoding="utf-8")):
        steps.append({"filters": tag_filters(lane_flags(configs, lane)), "patterns": patterns})
    soaks = {}
    for line in _query(root, "--output=streamed_jsonproto", 'kind("%s", //...)' % SOAK_RULE):
        rule = json.loads(line)["rule"]
        tags = [attribute.get("stringListValue", []) for attribute in rule["attribute"]
                if attribute["name"] == "tags"]
        soaks[rule["name"]] = frozenset(tags[0] if tags else [])
    libfuzzer_targets = set(_query(root, "--output=label",
                                   'attr(linkopts, "%s", //...)' % LIBFUZZER_LINKOPTS))
    expanded = {}
    for step in steps:
        for pattern in step["patterns"]:
            pattern = pattern.lstrip("-")
            if pattern not in expanded and not pattern.endswith(WILDCARDS):
                expanded[pattern] = set(_query(root, "--output=label", "tests(%s)" % pattern))
    return soaks, libfuzzer_targets, steps, expanded


def main(argv):
    if argv:
        print(__doc__.strip().splitlines()[-1], file=sys.stderr)
        return 2
    root = Path(__file__).resolve().parents[2]
    soaks, libfuzzer_targets, steps, expanded = collect(root)
    if not soaks or not libfuzzer_targets or not steps:
        print("::error::found %d %s targets, %d libFuzzer-linked targets and %d Fuzz workflow "
              "phases; expected all three" %
              (len(soaks), SOAK_RULE, len(libfuzzer_targets), len(steps)))
        return 1
    found = problems(soaks, libfuzzer_targets, steps, expanded)
    for problem in found:
        print("::error::%s" % problem)
    if found:
        return 1
    print("Every fuzzer's soak runs in a Fuzz workflow step: %d soaks, %d libFuzzer-linked "
          "targets, %d steps." % (len(soaks), len(libfuzzer_targets), len(steps)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
