"""The actual no-text example must run without font assets or resource preflight."""

import unittest

from native_resource_delivery_tests import ResourceDeliveryFixture


class NativeNoTextResourceDeliveryTests(ResourceDeliveryFixture):
    def test_relocated_no_text_example_needs_no_companion_tree(self):
        self.assertFalse(self.assets and self.assets.exists())
        result, output = self.run_example()
        self.assert_rendered(result, output)
        self.assertNotIn("Computed text length:", result.stdout)
        self.assertNotIn("Packaged resources", result.stderr)


if __name__ == "__main__":
    unittest.main()
