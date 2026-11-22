"""Verify packed language-rule outputs against the uncompressed generated mapping."""

from __future__ import annotations

import argparse
from pathlib import Path
import re

from generate_language_data import compact_complex_language

TAG = re.compile(r'PackTag\("([^"\\]{4})"\)')
SINGLE = r'tags\[0\] = PackTag\("([^"\\]{4})"\);\s*\*count = 1;\s*return true;'
MANY = (
    r'unsigned int i;\s*uint32_t possible_tags\[\] = '
    r'\{((?:\s*PackTag\("[^"\\]{4}"\),)+\s*)\};\s*'
    r'for \(i = 0; i < (\d+) && i < \*count; i\+\+\)\s*'
    r'tags\[i\] = possible_tags\[i\];\s*\*count = i;\s*return true;'
)


def verify(before: str, after: str) -> tuple[int, int]:
    before_tables, before_rules = before.split("static inline bool\nComplexLanguageTags", 1)
    after_tables, after_rules = after.split(
        "static const LanguageTags kComplexLanguageOutcomes[] = {\n", 1
    )
    if before_tables != after_tables:
        raise ValueError("primary-language tables changed")
    payloads, after_body = after_rules.split(
        "};\n\nstatic uint8_t ComplexLanguageIndex(const char* lang_str, const char* limit)", 1
    )
    outcomes = [tuple(TAG.findall(line)) for line in payloads.splitlines()]
    if not outcomes or outcomes[0] != () or len(outcomes) != len(set(outcomes)):
        raise ValueError("the outcome table must reserve index zero uniquely for no match")

    def old_action(match: re.Match[str]) -> str:
        result = (match[1],) if match[1] is not None else tuple(TAG.findall(match[2]))
        if not 1 <= len(result) <= 3 or (match[3] is not None and len(result) != int(match[3])):
            raise ValueError("unexpected original output arity")
        return "outcome " + repr(result) + ";"

    def new_action(match: re.Match[str]) -> str:
        return "outcome " + repr(outcomes[int(match[1])]) + ";"

    original_body = before_rules[before_rules.index("{"):]
    original, recognized = re.subn(f"{SINGLE}|{MANY}", old_action, original_body)
    original = original.replace("return false;", "outcome ();")
    compacted, paths = re.subn(r"return (\d+);", new_action, after_body)
    if recognized != original_body.count("return true;") or original.strip() != compacted.strip():
        raise ValueError("language predicates, priority or return values changed")
    return paths, len(outcomes)


def verify_rejection(before: str) -> None:
    invalid = before.replace("return true;", "return 255;", 1)
    if invalid == before:
        raise ValueError("expected an original literal true return for the rejection control")
    try:
        compact_complex_language(invalid)
    except ValueError as error:
        if "unexpected return" not in str(error):
            raise
        return
    raise ValueError("generator accepted a non-boolean original return")



def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path, help="uncompressed generated language-data file")
    args = parser.parse_args()
    after = Path(__file__).with_name("OpenTypeLanguageData.inc")
    before = args.before.read_text()
    paths, outcomes = verify(before, after.read_text())
    verify_rejection(before)
    print(f"Equivalent primary tables, ordered predicates and all {paths} return paths.")
    print(f"The caller supplies three zero-initialized slots. Unique padded outcomes: {outcomes}.")
    print("The generator rejects a numeric return in the original rules.")


if __name__ == "__main__":
    main()
