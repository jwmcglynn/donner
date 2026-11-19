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
        for line in sys.stdin:
            print(variant(line.rstrip("\n")))
    else:
        sys.exit("usage: darwin_selection.py query|variants")
