"""Regression guards for Python review failures that previously escaped local CI checks."""

import unittest

import check_python_quality as quality


class PythonQualityTest(unittest.TestCase):
    def test_xml_aliases_and_new_copies_are_detected(self):
        examples = (
            "import xml.etree.ElementTree as ET\ndef read(data):\n    return ET.fromstring(data)\n",
            "from xml.etree.ElementTree import fromstring as parse\ndef read(data):\n    return parse(data)\n",
            "import xml.etree.ElementTree\nxml.etree.ElementTree.parse('input')\n",
        )
        for source in examples:
            with self.subTest(source=source):
                self.assertTrue(any(item[1] == "PY-XML" for item in quality.check_source(source)))
                self.assertEqual([], quality.check_source(source, source))
        original = examples[0]
        copied = original + "def other(data):\n    return ET.fromstring(data)\n"
        self.assertEqual(1, sum(item[1] == "PY-XML" for item in quality.check_source(copied, original)))

    def test_nested_decisions_fail_before_review(self):
        source = "def complex_method(value):\n"
        for depth in range(7):
            source += "    " * (depth + 1) + "if value:\n"
        source += "    " * 8 + "return value\n"
        findings = quality.check_source(source)
        self.assertEqual("PY-COMPLEXITY", findings[0][1])
        self.assertEqual([], quality.check_source(source, source))

    @staticmethod
    def _nested_if_source(levels, variable="value"):
        source = f"def complex_method({variable}):\n"
        for depth in range(levels):
            source += "    " * (depth + 1) + f"if {variable}:\n"
        source += "    " * (levels + 1) + f"return {variable}\n"
        return source

    def test_reduced_debt_passes_against_baseline(self):
        baseline = self._nested_if_source(7)
        reduced = self._nested_if_source(6)
        self.assertEqual([], quality.check_source(reduced, baseline))

    def test_increased_debt_fails_against_baseline(self):
        baseline = self._nested_if_source(6)
        increased = self._nested_if_source(7)
        findings = quality.check_source(increased, baseline)
        self.assertEqual("PY-COMPLEXITY", findings[0][1])

    def test_unchanged_score_passes_against_baseline(self):
        baseline = self._nested_if_source(7, variable="value")
        renamed = self._nested_if_source(7, variable="other")
        self.assertEqual([], quality.check_source(renamed, baseline))

    def test_simple_streaming_parser_calls_are_not_blacklisted(self):
        source = "from xml.parsers import expat\ndef parser():\n    return expat.ParserCreate()\n"
        self.assertEqual([], quality.check_source(source))


if __name__ == "__main__":
    unittest.main()
