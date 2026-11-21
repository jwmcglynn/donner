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
    return HEADER.format(version=version) + data + "\n"


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
