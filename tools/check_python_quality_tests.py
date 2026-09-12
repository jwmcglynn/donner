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

    def test_simple_streaming_parser_calls_are_not_blacklisted(self):
        source = "from xml.parsers import expat\ndef parser():\n    return expat.ParserCreate()\n"
        self.assertEqual([], quality.check_source(source))


if __name__ == "__main__":
    unittest.main()
