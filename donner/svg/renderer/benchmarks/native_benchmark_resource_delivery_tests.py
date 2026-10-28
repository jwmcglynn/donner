"""Deployment checks that never open a GPU or include preflight in benchmark timing."""

import unittest

from donner.svg.renderer.tests.native_resource_delivery_fixture import (
    DeliveryChecks, NativeResourceFixture,
)


class RendererBenchDelivery(NativeResourceFixture, DeliveryChecks):
    consumer = ("donner/svg/renderer/benchmarks", "renderer_bench", "renderer_bench")

    def invoke(self):
        return self.run_binary(["--iterations=1", "--warmup=0"])

    def assert_resource_error(self, result):
        super().assert_resource_error(result)
        self.assertNotIn("=== Geode Renderer Benchmark ===", result.stdout)


class EngineCompareDelivery(NativeResourceFixture, DeliveryChecks):
    consumer = ("donner/svg/renderer/benchmarks", "engine_compare_bench", "engine_compare_bench")

    def invoke(self):
        return self.run_binary(["--backend=tiny-skia", "--iterations=1", "--warmup=0", self.svg("input", True)])

    def assert_resource_error(self, result):
        super().assert_resource_error(result)
        self.assertNotIn("RESULT engine=", result.stdout)
        self.assertNotIn("ALLOC engine=", result.stdout)

    def render(self, source, output):
        destination = self.cwd / output
        result = self.run_binary(["--backend=tiny-skia", "--iterations=1", "--warmup=0",
                                  "--output=" + str(destination), source])
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("RESULT engine=TinySkia ", result.stdout)
        return destination

    def test_relocated_fallback_produces_glyph_pixels(self):
        self.assert_glyph_pixels()


if __name__ == "__main__":
    unittest.main()
