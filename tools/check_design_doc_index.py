#!/usr/bin/env python3
"""Check that the design-document index and Doxygen navigation match the documents.

Rules, for docs/design_docs/:
  - Every numbered document (`NNNN-name.md` or `NNNN-K-name.md`, including subdirectories) has a
    row in the README.md "Document Index" table and a `\\subpage` entry in navigation.dox.
  - Every index row links to a file that exists, and every design-doc `\\subpage` names one.
  - An index row's Status cell starts with the linked document's status keyword: the text of
    the document's first `**Status:**` line up to the first `.`/`:` followed by whitespace, `;`,
    `,`, `(`, `[`, or a spaced dash, with `**` emphasis removed. Anything after the keyword in
    the cell must begin with one of those delimiters. A document without a Status line is
    reported unless it is listed in STATUS_EXEMPT.

Usage: python3 tools/check_design_doc_index.py [--root=REPO_ROOT]
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path


DESIGN_DIR = Path("docs/design_docs")
NUMBERED_DOC_RE = re.compile(r"^\d{4}(?:-\d+)?-[A-Za-z0-9_]+\.md$")
STATUS_LINE_RE = re.compile(r"^\*\*Status:\*\*[ \t]*(?P<text>.*)$", re.MULTILINE)
# Doxygen takes a page name from a `{#anchor}` on the title heading that opens the file.
TITLE_ANCHOR_RE = re.compile(r"\A\s*# [^\n]*\{#(?P<anchor>[A-Za-z0-9_]+)\}")
SUBPAGE_RE = re.compile(r"\\subpage\s+(?P<page>\S+)")
INDEX_LINK_RE = re.compile(r"\[[^\]]*\]\((?P<target>[^)\s]+)\)")
DASHES = "-\u2013\u2014"
KEYWORD_END_RE = re.compile(r"[.:](?=\s|$)|[;,(\[]|\s(?:--|[" + DASHES + r"])\s")
DELIMITER_STARTS = tuple(".:;,([" + DASHES)

# Documents that predate the Status convention. Adding a Status line is a content change for the
# document's owner; until then the index cell for these is not compared.
STATUS_EXEMPT = frozenset(
    {
        "0001-terminal_image_viewer.md",
        "0002-mcp_test_triage_server.md",
        "0004-external_svg_references.md",
        "text/0052-2-architecture.md",
        "text/0052-4-testing.md",
    }
)


def doxygen_page_id(relative_path: str) -> str:
    """Return Doxygen's page name for a Markdown file without an explicit `{#anchor}`."""
    stem = relative_path[: -len(".md")] if relative_path.endswith(".md") else relative_path
    escaped = "".join(
        "__" if ch == "_" else "_2" if ch == "/" else f"_{ch.lower()}" if ch.isupper() else ch
        for ch in stem
    )
    return "md_" + escaped


def status_keyword(status_text: str) -> str:
    """Return the leading status keyword of a `**Status:**` line's text."""
    text = status_text.replace("**", "").strip().rstrip("\\").strip()
    end = KEYWORD_END_RE.search(text)
    return (text[: end.start()] if end else text).strip()


def cell_matches_keyword(cell: str, keyword: str) -> bool:
    if not cell.startswith(keyword):
        return False
    rest = cell[len(keyword) :].lstrip()
    return not rest or rest.startswith(DELIMITER_STARTS)


def numbered_docs(design_root: Path) -> list[str]:
    return sorted(
        path.relative_to(design_root).as_posix()
        for path in design_root.rglob("*.md")
        if NUMBERED_DOC_RE.match(path.name)
    )


def index_rows(readme: str) -> list[tuple[int, str, str]]:
    """Return (line number, link target, status cell) for each linked Document Index row."""
    rows = []
    in_index = False
    for number, line in enumerate(readme.splitlines(), start=1):
        if line.startswith("## "):
            in_index = line.strip() == "## Document Index"
            continue
        cells = [cell.strip() for cell in line.strip().strip("|").split("|")]
        link = INDEX_LINK_RE.search(cells[1]) if in_index and len(cells) >= 3 else None
        if link:
            rows.append((number, link.group("target"), cells[2]))
    return rows


def expected_page_id(design_root: Path, relative_path: str) -> str:
    anchor = TITLE_ANCHOR_RE.match((design_root / relative_path).read_text(encoding="utf-8"))
    if anchor:
        return anchor.group("anchor")
    return doxygen_page_id((DESIGN_DIR / relative_path).as_posix())


def check_navigation(design_root: Path, docs: list[str], navigation: str) -> list[str]:
    pages = set(SUBPAGE_RE.findall(navigation))
    errors = [
        f"navigation.dox: {doc} has no \\subpage {expected_page_id(design_root, doc)}"
        for doc in docs
        if expected_page_id(design_root, doc) not in pages
    ]
    known = {
        doxygen_page_id((DESIGN_DIR / path).as_posix()) for path in design_markdown(design_root)
    }
    prefix = doxygen_page_id(DESIGN_DIR.as_posix() + "/")
    errors.extend(
        f"navigation.dox: \\subpage {page} names no design document"
        for page in sorted(pages)
        if page.startswith(prefix) and page not in known
    )
    return errors


def design_markdown(design_root: Path) -> list[str]:
    return sorted(path.relative_to(design_root).as_posix() for path in design_root.rglob("*.md"))


def check_row_status(design_root: Path, line: int, target: str, cell: str) -> list[str]:
    status = STATUS_LINE_RE.search((design_root / target).read_text(encoding="utf-8"))
    if not status:
        if target in STATUS_EXEMPT:
            return []
        return [f"README.md:{line}: {target} has no **Status:** line"]
    keyword = status_keyword(status.group("text"))
    if cell_matches_keyword(cell, keyword):
        return []
    return [f"README.md:{line}: status cell {cell!r} does not start with {target}'s {keyword!r}"]


def check_index(design_root: Path, docs: list[str], readme: str, selected=None) -> list[str]:
    errors = []
    indexed = set()
    for line, target, cell in index_rows(readme):
        if "://" in target:
            continue
        indexed.add(target)
        if not (design_root / target).is_file():
            errors.append(f"README.md:{line}: index row links to missing file {target}")
            continue
        if selected is None or target in selected:
            errors.extend(check_row_status(design_root, line, target, cell))
    errors.extend(f"README.md: {doc} is missing from the Document Index" for doc in docs
                  if doc not in indexed)
    return errors


def check(root: Path, changed=()) -> list[str]:
    design_root = root / DESIGN_DIR
    docs = numbered_docs(design_root)
    readme = (design_root / "README.md").read_text(encoding="utf-8")
    navigation = (design_root / "navigation.dox").read_text(encoding="utf-8")
    selected = {p.removeprefix(DESIGN_DIR.as_posix() + "/") for p in changed}
    return check_index(design_root, docs, readme, selected if selected and "README.md" not in selected else None) + check_navigation(design_root, docs, navigation)


def main(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args(argv[1:])
    errors = check(args.root, args.paths)
    for error in errors:
        print(error)
    if errors:
        print(f"\n{len(errors)} design-doc index problem(s) found.", file=sys.stderr)
        return 1
    print("OK: design-doc index and navigation match the documents.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
