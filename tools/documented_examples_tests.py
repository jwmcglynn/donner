"""Keeps documented code examples identical to example sources that Bazel builds.

Doxygen pages quote C++ with `\\snippet <file> <tag>`, which Doxygen resolves against the
top-level `examples/*.cc` files. Text that is also read outside Doxygen, such as the release
notes, mirrors a source region instead: an HTML comment `<!-- example: <path> [tag] -->` before a
fenced block, optionally followed by blank lines, names the source file and, optionally, the
region between the two lines that carry the `[tag]` marker (`//! [tag]` in C++, `# [tag]` in
Bazel files).

The checks fail when a snippet names a missing region, a mirrored block differs from its
source, a quoted example file has no non-manual build target, or a checked page has a C++ block,
tagged or recognizably C++ without a tag, that neither form covers.
"""

from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
import re
import textwrap
import unittest

from python.runfiles import runfiles

# Doxygen pages whose C++ examples must come from compiled sources.
DOXYGEN_PAGES = (
    "docs/building.md",
    "docs/getting_started.md",
    "docs/guides/embedding_geode.md",
)
RELEASE_NOTES = "RELEASE_NOTES.md"
EXAMPLES_DIR = "examples"

CPP_LANGUAGES = frozenset({"c++", "cc", "cpp", "cxx"})
# Signs that an untagged fence holds C++ rather than shell, Bazel or prose.
CPP_LOOKING = re.compile(
    r"^\s*#include\s*[<\"]|\b(?:donner|std)::\w|^\s*\w[\w:<>]*\s+\w+\(.*\)\s*\{", re.MULTILINE
)
FENCE = re.compile(r"^\s*```(?P<lang>[\w+-]*)\s*$")
BINDING = re.compile(r"^\s*<!--\s*example:\s*(?P<path>\S+)(?:\s+\[(?P<tag>[\w-]+)\])?\s*-->\s*$")
MARKER = re.compile(r"^\s*(?://!|#)\s*\[(?P<tag>[\w-]+)\]\s*$")
SNIPPET = re.compile(r"^\s*\\snippet(?:\{[^}]*\})?\s+(?P<file>\S+)\s+(?P<tag>[\w-]+)\s*$")


@dataclass(frozen=True)
class Fence:
    """One fenced code block in a Markdown page."""

    line: int  # 1-based line of the opening fence.
    language: str
    body: str
    binding: tuple[str, str | None] | None  # (path, tag) from the preceding comment.


def preceding_binding(lines: list[str], index: int) -> re.Match | None:
    """Return the example binding comment before line `index`, skipping blank lines."""
    previous = index - 1
    while previous >= 0 and not lines[previous].strip():
        previous -= 1
    return BINDING.match(lines[previous]) if previous >= 0 else None


def parse_fences(text: str) -> list[Fence]:
    """Return the fenced code blocks of a Markdown page, with their example bindings."""
    lines = text.splitlines()
    fences: list[Fence] = []
    index = 0
    while index < len(lines):
        opening = FENCE.match(lines[index])
        if opening is None:
            index += 1
            continue
        end = index + 1
        while end < len(lines) and lines[end].strip() != "```":
            end += 1
        binding = preceding_binding(lines, index)
        fences.append(
            Fence(
                line=index + 1,
                language=opening.group("lang").lower(),
                body=normalize("\n".join(lines[index + 1 : end])),
                binding=(binding.group("path"), binding.group("tag")) if binding else None,
            )
        )
        index = end + 1
    return fences


def normalize(text: str) -> str:
    """Dedent a block and drop trailing whitespace so indentation inside lists does not matter."""
    return "\n".join(line.rstrip() for line in textwrap.dedent(text).strip("\n").splitlines())


def extract_region(source: str, tag: str | None) -> str:
    """Return the text between the two `[tag]` marker lines, or the whole file without a tag."""
    lines = source.splitlines()
    if tag is None:
        return normalize(source)
    markers = [i for i, line in enumerate(lines) if (m := MARKER.match(line)) and m["tag"] == tag]
    if len(markers) != 2:
        raise LookupError(f"expected two [{tag}] markers, found {len(markers)}")
    return normalize("\n".join(lines[markers[0] + 1 : markers[1]]))


def newest_release_section(text: str) -> str:
    """Return the newest `## ` section of the release notes, the only one still edited."""
    sections = re.split(r"(?m)^## ", text)
    if len(sections) < 2:
        raise LookupError("release notes have no version section")
    return "## " + sections[1]


def binding_finding(where: str, fence: Fence, read_source) -> str | None:
    """Return why a bound block does not match its source region, or None when it does."""
    path, tag = fence.binding
    try:
        expected = extract_region(read_source(path), tag)
    except (LookupError, OSError) as error:
        return f"{where}: cannot read {path} [{tag}]: {error}"
    if any(MARKER.match(line) for line in expected.splitlines()):
        return f"{where}: region {path} [{tag}] contains another marker"
    if fence.body != expected:
        return f"{where}: block differs from {path} [{tag}]"
    return None


def is_cpp(fence: Fence) -> bool:
    """Return whether a fence holds C++, by its language tag or, untagged, by its content."""
    if fence.language in CPP_LANGUAGES:
        return True
    return fence.language == "" and CPP_LOOKING.search(fence.body) is not None


def built_example_sources(build_file: str) -> set[str]:
    """Return the source files that a non-manual target in an examples BUILD file compiles."""
    sources = set()
    for target in re.split(r"(?m)^(?=[a-z_]+\()", build_file):
        if '"manual"' in target:
            continue
        srcs = re.search(r"\bsrcs\s*=\s*\[([^\]]*)\]", target)
        if srcs:
            sources.update(re.findall(r'"([^"/:]+)"', srcs.group(1)))
    return sources


def mirror_findings(page: str, text: str, read_source) -> list[str]:
    """Report mirrored blocks that differ from their sources and unbound C++ blocks."""
    findings = []
    for fence in parse_fences(text):
        where = f"{page}:{fence.line}"
        if fence.binding is not None:
            finding = binding_finding(where, fence, read_source)
        elif is_cpp(fence):
            finding = f"{where}: C++ block is neither a \\snippet nor a bound mirror"
        else:
            finding = None
        if finding is not None:
            findings.append(finding)
    return findings


def snippet_findings(page: str, text: str, read_example) -> list[str]:
    """Report `\\snippet` commands whose file or region Doxygen would not find."""
    findings = []
    for number, line in enumerate(text.splitlines(), 1):
        snippet = SNIPPET.match(line)
        if snippet is None:
            continue
        name, tag = snippet["file"], snippet["tag"]
        if "/" in name or not name.endswith(".cc"):
            findings.append(f"{page}:{number}: \\snippet must name a top-level examples/*.cc file")
            continue
        try:
            extract_region(read_example(name), tag)
        except (LookupError, OSError) as error:
            findings.append(f"{page}:{number}: \\snippet {name} {tag}: {error}")
    return findings


class DocumentedExamplesTest(unittest.TestCase):
    maxDiff = None

    def setUp(self):
        self.resolver = runfiles.Create()

    def read(self, path: str) -> str:
        location = self.resolver.Rlocation(f"donner/{path}")
        if location is None:
            raise OSError(f"{path} is not in this test's runfiles")
        return Path(location).read_text(encoding="utf-8")

    def read_example(self, name: str) -> str:
        return self.read(f"{EXAMPLES_DIR}/{name}")

    def test_quoted_examples_have_ordinary_build_targets(self):
        built = built_example_sources(self.read(f"{EXAMPLES_DIR}/BUILD.bazel"))
        names = set()
        for page in DOXYGEN_PAGES:
            names.update(m["file"] for m in map(SNIPPET.match, self.read(page).splitlines()) if m)
        for fence in parse_fences(newest_release_section(self.read(RELEASE_NOTES))):
            if fence.binding and fence.binding[0].startswith(f"{EXAMPLES_DIR}/"):
                names.add(fence.binding[0].removeprefix(f"{EXAMPLES_DIR}/"))
        names.discard("BUILD.bazel")
        self.assertNotEqual(names, set())
        self.assertEqual(sorted(names - built), [], "quoted examples need a non-manual target")

    def test_doxygen_pages_quote_compiled_examples(self):
        findings = []
        for page in DOXYGEN_PAGES:
            text = self.read(page)
            findings += snippet_findings(page, text, self.read_example)
            findings += mirror_findings(page, text, self.read)
        self.assertEqual(findings, [])

    def test_release_notes_mirror_compiled_examples(self):
        section = newest_release_section(self.read(RELEASE_NOTES))
        bound = [fence for fence in parse_fences(section) if fence.binding is not None]
        self.assertNotEqual(bound, [], "the newest release notes need a compiled usage example")
        self.assertNotIn("\\snippet", section, "GitHub does not render \\snippet")
        self.assertEqual(mirror_findings(RELEASE_NOTES, section, self.read), [])


class CheckerTest(unittest.TestCase):
    """Exercises the checker on small inputs, so a broken parser cannot pass vacuously."""

    SOURCE = "int main() {\n  //! [body]\n  return 0;\n  //! [body]\n}\n"

    def read_source(self, path: str) -> str:
        if path != "examples/main.cc":
            raise OSError(f"no such file: {path}")
        return self.SOURCE

    def test_region_is_dedented_text_between_markers(self):
        self.assertEqual(extract_region(self.SOURCE, "body"), "return 0;")
        with self.assertRaisesRegex(LookupError, "expected two"):
            extract_region(self.SOURCE, "missing")

    def test_matching_mirror_passes_and_drift_fails(self):
        matching = "<!-- example: examples/main.cc [body] -->\n\n```cpp\nreturn 0;\n```\n"
        self.assertEqual(mirror_findings("page.md", matching, self.read_source), [])
        drifted = matching.replace("return 0;", "return 1;")
        self.assertEqual(
            mirror_findings("page.md", drifted, self.read_source),
            ["page.md:3: block differs from examples/main.cc [body]"],
        )

    def test_unbound_cpp_fails_but_other_languages_pass(self):
        text = "```cpp\nint x;\n```\n\n```sh\nbazel build //...\n```\n"
        self.assertEqual(
            mirror_findings("page.md", text, self.read_source),
            ["page.md:1: C++ block is neither a \\snippet nor a bound mirror"],
        )

    def test_untagged_cpp_fence_fails(self):
        cpp = "```\n#include <cstdio>\nint main() { return 0; }\n```\n"
        text = cpp + "\n```\nbazel build //...\n```\n"
        self.assertEqual(
            mirror_findings("page.md", text, self.read_source),
            ["page.md:1: C++ block is neither a \\snippet nor a bound mirror"],
        )

    def test_built_sources_exclude_manual_targets(self):
        build = (
            'cc_binary(\n    name = "a",\n    srcs = ["a.cc"],\n)\n\n'
            'cc_binary(\n    name = "b",\n    srcs = ["b.cc"],\n    tags = ["manual"],\n)\n'
        )
        self.assertEqual(built_example_sources(build), {"a.cc"})

    def test_snippet_must_resolve(self):
        text = "\\snippet main.cc body\n\\snippet main.cc missing\n"
        read_example = lambda name: self.read_source(f"examples/{name}")
        findings = snippet_findings("page.md", text, read_example)
        self.assertEqual(len(findings), 1)
        self.assertIn("page.md:2: \\snippet main.cc missing", findings[0])

    def test_newest_release_section_stops_at_the_next_version(self):
        text = "# Donner\n\n## v2\n\nnew\n\n## v1\n\nold\n"
        self.assertEqual(newest_release_section(text), "## v2\n\nnew\n\n")


if __name__ == "__main__":
    unittest.main()
