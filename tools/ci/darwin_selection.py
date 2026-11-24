"""Map the existing affected build and test selection to its Darwin variants."""

import json
import os
import sys


def root_query(fallback, affected):
    roots = ["//..."] if fallback else affected.split()
    if not roots:
        raise ValueError("Empty Darwin target selection")
    root_set = "set(" + " ".join(json.dumps(label) for label in roots) + ")"
    manual = r"(\[|, )manual(, |\])"
    return (f"let input = {root_set} in "
            f"let roots = $input except attr(tags, '{manual}', $input) in "
            'let expanded = (kind(".* rule", $roots) except kind("test_suite rule", $roots)) '
            'union tests($roots) in '
            f'$expanded except (kind("(package_group|toolchain_type) rule", $expanded) '
            f"union attr(tags, '{manual}', $expanded))")


# Tag of a rule with no Darwin counterpart (build_defs/ci_darwin.bzl LINUX_ONLY_TAG); the Darwin
# finalizer declares none for it, so the selection must not request one.
LINUX_ONLY_TAG = "ci-linux-only"


def darwin_variants(labels):
    """Vendor code enters the Darwin configuration through its consuming targets."""
    return (variant(label) for label in labels if not label.startswith(("//third_party:", "//third_party/")))


def darwin_roots(rules):
    """The labels of the queried rules that have Darwin counterparts.

    `rules` are the `rule` objects of the streamed jsonproto query, with their tags.
    """
    for rule in rules:
        tags = next((item.get("stringListValue", []) for item in rule.get("attribute", [])
                     if item["name"] == "tags"), [])
        if LINUX_ONLY_TAG not in tags:
            yield rule["name"]


def variant(label):
    if label.startswith("//") and ":" in label:
        package, name = label.split(":", 1)
        if name:
            return f"{package}:ci_darwin_{name}"
    raise ValueError(f"Expected a main-repository rule label, got {label!r}")


if __name__ == "__main__":
    if sys.argv[1:] == ["query"]:
        print(root_query(os.environ["TARGETS_FALLBACK"] == "true", os.environ["TARGETS_AFFECTED"]))
    elif sys.argv[1:] == ["variants"]:
        rules = (json.loads(line)["rule"] for line in sys.stdin)
        for label in darwin_variants(darwin_roots(rules)):
            print(label)
    else:
        sys.exit("usage: darwin_selection.py query|variants")
