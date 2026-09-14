"""Native attachment framing and peer-path validation, without starting an editor."""
import importlib.util
import io
import json
from pathlib import Path
import sys
import unittest

module_path = Path(__file__).with_name("editor_control_wrapper.py")
spec = importlib.util.spec_from_file_location("native_wrapper", module_path)
wrapper = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = wrapper
spec.loader.exec_module(wrapper)


class NativeFramingTest(unittest.TestCase):
  def test_standard_json_lines(self):
    message, legacy = wrapper.read_native_message(io.BytesIO(b'{"id":1,"method":"ping"}\n'))
    self.assertEqual(message, {"id": 1, "method": "ping"})
    self.assertFalse(legacy)

  def test_legacy_content_length(self):
    value = b'{"id":2,"method":"ping"}'
    message, legacy = wrapper.read_native_message(io.BytesIO(
        b"Content-Length: " + str(len(value)).encode() + b"\r\n\r\n" + value))
    self.assertEqual(message["id"], 2)
    self.assertTrue(legacy)

  def test_rejects_negative_oversized_and_duplicate_headers(self):
    for value in [b"Content-Length: -1\r\n\r\n",
                  b"Content-Length: 100000000\r\n\r\n",
                  b"Content-Length: 2\r\nContent-Length: 2\r\n\r\n{}"]:
      with self.subTest(value=value):
        with self.assertRaises(wrapper.ProtocolError):
          wrapper.read_native_message(io.BytesIO(value))

  def test_truncated_body_and_non_object_are_rejected(self):
    with self.assertRaises(wrapper.ProtocolError):
      wrapper.read_native_message(io.BytesIO(b"Content-Length: 10\r\n\r\n{}"))
    with self.assertRaises(wrapper.ProtocolError):
      wrapper.read_native_message(io.BytesIO(b"Content-Length: 2\r\n\r\n[]"))

  def test_eof_terminates_cleanly(self):
    self.assertEqual(wrapper.read_native_message(io.BytesIO(b"")), (None, False))


if __name__ == "__main__":
  unittest.main()
