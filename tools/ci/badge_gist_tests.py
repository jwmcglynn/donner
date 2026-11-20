"""Pins that the README badges render as valid SVG and publish in one gist update."""

import io
import json
import unittest
import urllib.error
from xml.parsers import expat

from tools.ci import badge_gist

GIST_ID = "91f7f490a72af9c06506c8176729d218"
TOKEN = "fixture-token-5f0c2a"
WORKFLOW_BADGES = (
    ("loc.svg", "Lines of code", "  45.2k", "lightgrey"),
    ("loc-tests.svg", "Lines of test code", "  30.1k", "blue"),
    ("comments.svg", "Comments", "  21.5%", "green"),
)


def _argv(*extra, badges=WORKFLOW_BADGES):
    argv = ["--gist-id", GIST_ID, *extra]
    for badge in badges:
        argv += ["--badge", *badge]
    return argv


class _Response:
    def __init__(self, status):
        self.status = status

    def __enter__(self):
        return self

    def __exit__(self, *_):
        return False


class RecordingOpener:
    """Records every request instead of sending it, and answers `status` or raises `error`."""

    def __init__(self, status=200, error=None):
        self.status, self.error, self.requests = status, error, []

    def __call__(self, request, timeout):
        self.requests.append(request)
        if self.error is not None:
            raise self.error
        return _Response(self.status)


WARNING = "::warning title=Badge update failed::"


def _run(argv, opener=None, environ=None):
    stdout = io.StringIO()
    code = badge_gist.main(argv, environ={"GIST_TOKEN": TOKEN} if environ is None else environ,
                           opener=opener or RecordingOpener(), stdout=stdout)
    return code, stdout.getvalue()


def _parse_svg(svg):
    """The root element name and the character data of every `title` and `text` element."""
    elements, texts, current = [], [], []
    parser = expat.ParserCreate()

    def reject(*_):
        raise AssertionError("badge SVG must not declare a doctype or entities")

    def start(name, _attributes):
        elements.append(name)
        current.append([])

    def end(name):
        content = "".join(current.pop())
        if name in ("title", "text"):
            texts.append((name, content))

    def data(text):
        if current:
            current[-1].append(text)

    parser.StartElementHandler, parser.EndElementHandler = start, end
    parser.CharacterDataHandler = data
    parser.StartDoctypeDeclHandler = reject
    parser.EntityDeclHandler = reject
    parser.ExternalEntityRefHandler = lambda *_: 0
    parser.Parse(svg, True)
    return elements[0], texts


class BadgeGistTest(unittest.TestCase):
    def test_publishes_all_badges_in_one_patch(self):
        opener = RecordingOpener()
        code, stdout = _run(_argv(), opener)
        self.assertEqual(0, code, stdout)
        self.assertNotIn("::warning", stdout)
        self.assertEqual(1, len(opener.requests), "badges must be written in one gist update")
        request = opener.requests[0]
        self.assertEqual(("PATCH", "https://api.github.com/gists/" + GIST_ID),
                         (request.get_method(), request.full_url))
        self.assertEqual({"Authorization": "Bearer " + TOKEN,
                          "Accept": "application/vnd.github+json",
                          "X-github-api-version": "2022-11-28",
                          "Content-type": "application/json"}, dict(request.header_items()))
        body = json.loads(request.data)
        self.assertEqual(["files"], list(body))
        self.assertEqual(["loc.svg", "loc-tests.svg", "comments.svg"], list(body["files"]))
        for name, content in body["files"].items():
            self.assertEqual(["content"], list(content), name)
        self.assertIn("loc.svg, loc-tests.svg, comments.svg", stdout)

    def test_badges_are_well_formed_svg_with_the_stripped_values(self):
        files = badge_gist.render_files(WORKFLOW_BADGES)
        for (name, label, message, _), svg in zip(WORKFLOW_BADGES, files.values()):
            with self.subTest(name=name):
                root, texts = _parse_svg(svg)
                self.assertEqual("svg", root)
                expected = [("text", label)] * 2 + [("text", message.strip())] * 2
                self.assertEqual([("title", f"{label}: {message.strip()}")] + expected, texts)

    def test_badge_colors_match_the_shields_palette(self):
        for color, hex_value in (("lightgrey", "#9f9f9f"), ("blue", "#007ec6"),
                                 ("green", "#97ca00")):
            with self.subTest(color=color):
                self.assertIn(f'fill="{hex_value}"', badge_gist.render_badge("a", "b", color))

    def test_text_is_escaped(self):
        message = 'a & b <c> "d" \'e\''
        svg = badge_gist.render_badge("x<y", message, "blue")
        _, texts = _parse_svg(svg)
        self.assertEqual([("title", "x<y: " + message)] + [("text", "x<y")] * 2 +
                         [("text", message)] * 2, texts)
        self.assertNotIn("<c>", svg)
        self.assertIn("a &amp; b &lt;c&gt; &quot;d&quot; &apos;e&apos;", svg)

    def test_width_grows_with_text_length(self):
        def width(message):
            return int(badge_gist.render_badge("Comments", message, "green")
                       .split('width="', 1)[1].split('"', 1)[0])
        self.assertLess(width("1"), width("12"))
        self.assertLess(width("12"), width("12.5%"))
        self.assertLess(badge_gist.text_width("i"), badge_gist.text_width("W"))

    def test_render_is_deterministic(self):
        self.assertEqual(badge_gist.render_files(WORKFLOW_BADGES),
                         badge_gist.render_files(WORKFLOW_BADGES))

    def test_rejects_invalid_badges_without_sending(self):
        cases = {
            "unknown color": ([("loc.svg", "Lines", "1", "brightgreen")], "unknown badge color"),
            "empty message": ([("loc.svg", "Lines", "  ", "blue")], "message is empty"),
            "control character": ([("loc.svg", "Lines", "1\n2", "blue")], "control character"),
            "duplicate file": ([("loc.svg", "A", "1", "blue")] * 2, "duplicate badge file"),
            "path file name": ([("../loc.svg", "A", "1", "blue")], "invalid badge file name"),
        }
        for case, (badges, expected) in cases.items():
            with self.subTest(case=case):
                opener = RecordingOpener()
                code, stdout = _run(_argv(badges=badges), opener)
                self.assertEqual(1, code)
                self.assertTrue(stdout.startswith(WARNING), stdout)
                self.assertIn(expected, stdout)
                self.assertEqual([], opener.requests)

    def test_rejects_a_missing_token_or_invalid_gist_id_without_sending(self):
        for case, argv, environ, expected in (
            ("missing token", _argv(), {}, "GIST_TOKEN is missing"),
            ("path gist id", ["--gist-id", "../user", "--badge", *WORKFLOW_BADGES[0]],
             None, "invalid gist ID"),
        ):
            with self.subTest(case=case):
                opener = RecordingOpener()
                code, stdout = _run(argv, opener, environ)
                self.assertEqual((1, []), (code, opener.requests))
                self.assertIn(expected, stdout)

    def test_dry_run_reports_the_request_without_sending_or_printing_the_token(self):
        opener = RecordingOpener()
        code, stdout = _run(_argv("--dry-run"), opener)
        self.assertEqual((0, []), (code, opener.requests))
        size = len(badge_gist.request_body(badge_gist.render_files(WORKFLOW_BADGES)))
        self.assertIn(f"with {size} bytes: loc.svg, loc-tests.svg, comments.svg", stdout)
        self.assertNotIn(TOKEN, stdout)

    def test_token_is_never_in_the_body(self):
        request = badge_gist.build_request(
            GIST_ID, TOKEN, badge_gist.render_files(WORKFLOW_BADGES))
        self.assertNotIn(TOKEN.encode(), request.data)

    def test_http_errors_fail_with_status_and_reason_but_not_the_token(self):
        refused = urllib.error.HTTPError(
            "https://api.github.com/gists/" + GIST_ID, 409, "Conflict",
            {"Authorization": "Bearer " + TOKEN}, io.BytesIO(TOKEN.encode()))
        for case, opener, expected in (
            ("conflict", RecordingOpener(error=refused), "HTTP 409 Conflict"),
            ("unreachable", RecordingOpener(error=urllib.error.URLError(TOKEN)), "URLError"),
            ("unexpected status", RecordingOpener(status=304), "HTTP 304"),
        ):
            with self.subTest(case=case):
                code, stdout = _run(_argv(), opener)
                self.assertEqual((1, 1), (code, len(opener.requests)))
                self.assertEqual(WARNING + "gist update failed: " + expected + "\n", stdout)
                self.assertNotIn(TOKEN, stdout)
                self.assertNotIn("Authorization", stdout)

    def test_warning_annotation_is_one_escaped_line(self):
        self.assertEqual(WARNING + "HTTP 409 Conflict", badge_gist.warning_annotation(
            "HTTP 409 Conflict"))
        self.assertEqual(WARNING + "a%25b%0Dc%0Ad", badge_gist.warning_annotation("a%b\rc\nd"))

    def test_warning_annotation_redacts_a_token_in_an_error(self):
        refused = urllib.error.HTTPError("https://api.github.com/gists/" + GIST_ID, 409,
                                         "Conflict " + TOKEN, {}, None)
        code, stdout = _run(_argv(), RecordingOpener(error=refused))
        self.assertEqual((1, WARNING + "gist update failed: HTTP 409 Conflict <redacted>\n"),
                         (code, stdout))


if __name__ == "__main__":
    unittest.main()
