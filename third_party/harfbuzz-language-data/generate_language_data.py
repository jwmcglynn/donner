"""Extract forward OpenType language mappings from the pinned HarfBuzz tree."""

from __future__ import annotations

import argparse
from pathlib import Path
import re

HEADER = """// Derived from HarfBuzz {version} src/hb-ot-tag-table.hh, generated from the
// OpenType language-system registry and IANA language-subtag registry.
// Contains the forward language mappings only. See COPYING.

"""
TAG = re.compile(r"HB_TAG\('(.?)','(.?)','(.?)','(.?)'\)")


def compact_complex_language(data: str) -> str:
    """Keep the upstream predicates and replace each terminal action with a table index."""
    begin = data.index("static inline bool\nComplexLanguageTags")
    prefix, rules = data[:begin], data[begin:]
    if re.search(r"\breturn\b(?! (?:true|false);)", rules):
        raise ValueError("unexpected return in the upstream complex language rules")
    outcomes = [()]
    tag = r'PackTag\("[^"\\]{4}"\)'
    single = r'tags\[0\] = PackTag\("([^"\\]{4})"\);\s*\*count = 1;\s*return true;'
    multiple = (
        rf'unsigned int i;\s*uint32_t possible_tags\[\] = \{{((?:\s*{tag},)+\s*)\}};\s*'
        r'for \(i = 0; i < (\d+) && i < \*count; i\+\+\)\s*'
        r'tags\[i\] = possible_tags\[i\];\s*\*count = i;\s*return true;'
    )

    def replace(match: re.Match[str]) -> str:
        if match[1] is not None:
            tags = (match[1],)
        else:
            tags = tuple(re.findall(r'PackTag\("([^"]{4})"\)', match[2]))
            if not 1 <= len(tags) == int(match[3]) <= 3:
                raise ValueError("unexpected complex language initializer size")
        if tags not in outcomes:
            if len(outcomes) == 256:
                raise ValueError("complex language outcomes no longer fit the index type")
            outcomes.append(tags)
        return f"return {outcomes.index(tags)};"

    rules = re.sub(f"{single}|{multiple}", replace, rules, flags=re.S)
    rules, signatures = re.subn(
        r'static inline bool\nComplexLanguageTags \(const char +\*lang_str,\s*'
        r'const char +\*limit,\s*unsigned int \*count *,\s*uint32_t +\*tags *\)',
        "static uint8_t ComplexLanguageIndex(const char* lang_str, const char* limit)",
        rules,
    )
    rules = rules.replace("return false;", "return 0;")
    if signatures != 1 or re.search(r"\b(tags|count|possible_tags)\b", rules):
        raise ValueError("unrecognized complex language output action")
    if re.search(r"\breturn\b(?! [0-9]+;)", rules):
        raise ValueError("unrecognized complex language return")
    payloads = "\n".join(
        "  {" + ", ".join(f'PackTag("{tag}")' for tag in tags) + "}," for tags in outcomes
    )
    return (
        prefix + "static const LanguageTags kComplexLanguageOutcomes[] = {\n"
        + payloads + "\n};\n\n" + rules
    )


def language_data(source: str, version: str) -> str:
    start = source.index("static const LangTag ot_languages2")
    end = source.index("/**\n * hb_ot_ambiguous_tag_to_language", start)
    data = source[start:end]
    blocked = data.index("static const hb_tag_t ot_languages3_blocked[]")
    data = data[:blocked] + data[data.index("static const LangTag ot_languages3[]", blocked):]
    data = re.sub(r"/\*.*?\*/", "", data, flags=re.S)
    data = data.replace("#ifndef HB_NO_LANGUAGE_LONG\n", "").replace("#endif\n", "")
    data = TAG.sub(lambda match: 'PackTag("' + "".join(match.groups()) + '")', data)
    for original, replacement in (
        ("hb_ot_tags_from_complex_language", "ComplexLanguageTags"),
        ("hb_tag_t", "uint32_t"),
        ("subtag_matches", "SubtagMatches"),
        ("lang_matches", "LanguageMatches"),
        ("strchr (", "std::strchr("),
        ("strncmp (", "std::strncmp("),
        ("HB_TAG_NONE", "0"),
    ):
        data = data.replace(original, replacement)
    if re.search(r"\b(?:HB_|hb_)", data):
        raise ValueError("unrecognized dependency in the upstream generated language data")
    data = re.sub(r"[ \t]+\n", "\n", data)
    data = re.sub(r"\n\s*\n\s*\n", "\n\n", data).expandtabs(2).strip()
    return HEADER.format(version=version) + compact_complex_language(data) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("harfbuzz", type=Path, help="unpacked archive of the pinned HarfBuzz release")
    parser.add_argument("--check", action="store_true", help="compare the data and notice on disk")
    args = parser.parse_args()
    output = Path(__file__).resolve().parent
    pin = output.parent / "bazel" / "non_bcr_deps.bzl"
    match = re.search(r'strip_prefix = "harfbuzz-([0-9.]+)"', pin.read_text())
    if match is None:
        parser.error("could not identify the pinned HarfBuzz release")
    source = args.harfbuzz / "src" / "hb-ot-tag-table.hh"
    result = language_data(source.read_text(encoding="utf-8"), match[1])
    files = {
        output / "OpenTypeLanguageData.inc": result.encode("utf-8"),
        output / "COPYING": (args.harfbuzz / "COPYING").read_bytes(),
    }
    for path, content in files.items():
        if args.check:
            if path.read_bytes() != content:
                parser.error(f"{path.name} differs from regenerated data")
        else:
            path.write_bytes(content)


if __name__ == "__main__":
    main()
