#!/usr/bin/env python3
"""Select fail-closed remote wrappers for CI-parallel GPU test suites."""

import argparse


REMOTE_TARGETS = {
    "//donner/editor/tests:rnr_replay_tests_geode":
        "//donner/editor/tests:rnr_replay_tests_geode_ci_remote",
    "//donner/svg/renderer/tests:resvg_test_suite_geode":
        "//donner/svg/renderer/tests:resvg_test_suite_geode_ci_remote",
}


def select_remote_targets(labels):
    """Replace opted-in local-isolation wrappers and preserve stable order."""
    result = []
    seen = set()
    for label in labels:
        selected = REMOTE_TARGETS.get(label, label)
        if selected not in seen:
            result.append(selected)
            seen.add(selected)
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("labels", nargs="*")
    parser.add_argument("--one-per-line", action="store_true")
    args = parser.parse_args()
    separator = "\n" if args.one_per_line else " "
    print(separator.join(select_remote_targets(args.labels)))


if __name__ == "__main__":
    main()
