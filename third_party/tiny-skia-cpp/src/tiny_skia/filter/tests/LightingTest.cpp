/// A vertical light over a uniform height field has unit diffuse and specular dot products.
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "tiny_skia/filter/Lighting.h"

namespace tiny_skia::filter {
namespace {

LightSourceParams verticalLight(LightType type) {
  LightSourceParams light;
  light.type = type;
  light.elevation = 90.0;
  light.x = light.pointsAtX = 1.0;
  light.y = light.pointsAtY = 1.0;
  light.z = 10.0;
  return light;
}

TEST(LightingTest, VerticalSourcesPreserveByteAndFloatPixels) {
  for (const auto type : {LightType::Distant, LightType::Point, LightType::Spot}) {
    SCOPED_TRACE(static_cast<int>(type));
    auto input = Pixmap::fromSize(3, 3);
    auto output = Pixmap::fromSize(3, 3);
    auto floatInput = FloatPixmap::fromSize(3, 3);
    auto floatOutput = FloatPixmap::fromSize(3, 3);
    ASSERT_TRUE(input && output && floatInput && floatOutput);
    for (std::size_t index = 3; index < input->data().size(); index += 4) {
      input->data()[index] = 255;
      floatInput->data()[index] = 1.0f;
    }

    DiffuseLightingParams diffuse;
    diffuse.light = verticalLight(type);
    diffuse.diffuseConstant = 0.5;
    diffuse.lightG = 0.5;
    diffuse.lightB = 0.25;
    diffuseLighting(*input, *output, diffuse);
    diffuseLighting(*floatInput, *floatOutput, diffuse);
    EXPECT_THAT(output->data().subspan(16, 4), testing::ElementsAre(128, 64, 32, 255));
    EXPECT_THAT(floatOutput->data().subspan(16, 4),
                testing::ElementsAre(0.5f, 0.25f, 0.125f, 1.0f));

    SpecularLightingParams specular;
    specular.light = verticalLight(type);
    specular.specularConstant = 0.5;
    specular.lightG = 0.5;
    specular.lightB = 0.25;
    specularLighting(*input, *output, specular);
    specularLighting(*floatInput, *floatOutput, specular);
    EXPECT_THAT(output->data().subspan(16, 4), testing::ElementsAre(128, 64, 32, 128));
    EXPECT_THAT(floatOutput->data().subspan(16, 4),
                testing::ElementsAre(0.5f, 0.25f, 0.125f, 0.5f));
  }
}

}  // namespace
}  // namespace tiny_skia::filter
