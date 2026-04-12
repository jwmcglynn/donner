#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/svg/SVGFEBlendElement.h"
#include "donner/svg/SVGFEColorMatrixElement.h"
#include "donner/svg/SVGFEComponentTransferElement.h"
#include "donner/svg/SVGFECompositeElement.h"
#include "donner/svg/SVGFEConvolveMatrixElement.h"
#include "donner/svg/SVGFEDiffuseLightingElement.h"
#include "donner/svg/SVGFEDisplacementMapElement.h"
#include "donner/svg/SVGFEDistantLightElement.h"
#include "donner/svg/SVGFEDropShadowElement.h"
#include "donner/svg/SVGFEFloodElement.h"
#include "donner/svg/SVGFEFuncAElement.h"
#include "donner/svg/SVGFEFuncBElement.h"
#include "donner/svg/SVGFEFuncGElement.h"
#include "donner/svg/SVGFEFuncRElement.h"
#include "donner/svg/SVGFEGaussianBlurElement.h"
#include "donner/svg/SVGFEImageElement.h"
#include "donner/svg/SVGFEMergeElement.h"
#include "donner/svg/SVGFEMergeNodeElement.h"
#include "donner/svg/SVGFEMorphologyElement.h"
#include "donner/svg/SVGFEOffsetElement.h"
#include "donner/svg/SVGFEPointLightElement.h"
#include "donner/svg/SVGFESpecularLightingElement.h"
#include "donner/svg/SVGFESpotLightElement.h"
#include "donner/svg/SVGFETileElement.h"
#include "donner/svg/SVGFETurbulenceElement.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/tests/ParserTestUtils.h"

namespace donner::svg {
namespace {

template <typename ElementT>
void ExpectExperimentalElementInstantiates(std::string_view xml) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto fragment = instantiateSubtreeElementAs<ElementT>(xml, options);
  EXPECT_THAT(fragment.element.template tryCast<ElementT>(), testing::Ne(std::nullopt));
}

TEST(SVGFilterPrimitiveWrapperTests, TrivialWrappersInstantiate) {
  ExpectExperimentalElementInstantiates<SVGFEBlendElement>(R"(<feBlend />)");
  ExpectExperimentalElementInstantiates<SVGFEColorMatrixElement>(R"(<feColorMatrix />)");
  ExpectExperimentalElementInstantiates<SVGFEComponentTransferElement>(
      R"(<feComponentTransfer />)");
  ExpectExperimentalElementInstantiates<SVGFECompositeElement>(R"(<feComposite />)");
  ExpectExperimentalElementInstantiates<SVGFEConvolveMatrixElement>(R"(<feConvolveMatrix />)");
  ExpectExperimentalElementInstantiates<SVGFEDiffuseLightingElement>(
      R"(<feDiffuseLighting />)");
  ExpectExperimentalElementInstantiates<SVGFEDisplacementMapElement>(
      R"(<feDisplacementMap />)");
  ExpectExperimentalElementInstantiates<SVGFEDistantLightElement>(R"(<feDistantLight />)");
  ExpectExperimentalElementInstantiates<SVGFEDropShadowElement>(R"(<feDropShadow />)");
  ExpectExperimentalElementInstantiates<SVGFEFloodElement>(R"(<feFlood />)");
  ExpectExperimentalElementInstantiates<SVGFEFuncAElement>(R"(<feFuncA />)");
  ExpectExperimentalElementInstantiates<SVGFEFuncBElement>(R"(<feFuncB />)");
  ExpectExperimentalElementInstantiates<SVGFEFuncGElement>(R"(<feFuncG />)");
  ExpectExperimentalElementInstantiates<SVGFEFuncRElement>(R"(<feFuncR />)");
  ExpectExperimentalElementInstantiates<SVGFEGaussianBlurElement>(R"(<feGaussianBlur />)");
  ExpectExperimentalElementInstantiates<SVGFEImageElement>(R"(<feImage />)");
  ExpectExperimentalElementInstantiates<SVGFEMergeElement>(R"(<feMerge />)");
  ExpectExperimentalElementInstantiates<SVGFEMergeNodeElement>(R"(<feMergeNode />)");
  ExpectExperimentalElementInstantiates<SVGFEMorphologyElement>(R"(<feMorphology />)");
  ExpectExperimentalElementInstantiates<SVGFEPointLightElement>(R"(<fePointLight />)");
  ExpectExperimentalElementInstantiates<SVGFESpecularLightingElement>(
      R"(<feSpecularLighting />)");
  ExpectExperimentalElementInstantiates<SVGFESpotLightElement>(R"(<feSpotLight />)");
  ExpectExperimentalElementInstantiates<SVGFETileElement>(R"(<feTile />)");
  ExpectExperimentalElementInstantiates<SVGFETurbulenceElement>(R"(<feTurbulence />)");
}

TEST(SVGFilterPrimitiveWrapperTests, FeOffsetSetterAndGetter) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto offset = instantiateSubtreeElementAs<SVGFEOffsetElement>(R"(<feOffset />)", options);
  offset->setOffset(3.0, 4.0);

  EXPECT_DOUBLE_EQ(offset->dx(), 3.0);
  EXPECT_DOUBLE_EQ(offset->dy(), 4.0);
}

TEST(SVGFilterPrimitiveWrapperTests, FilterPrimitiveStandardAttributes) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto offset = instantiateSubtreeElementAs<SVGFEOffsetElement>(R"(<feOffset />)", options);
  EXPECT_THAT(offset->x(), LengthIs(-10.0, Lengthd::Unit::Percent));
  EXPECT_THAT(offset->y(), LengthIs(-10.0, Lengthd::Unit::Percent));
  EXPECT_THAT(offset->width(), LengthIs(120.0, Lengthd::Unit::Percent));
  EXPECT_THAT(offset->height(), LengthIs(120.0, Lengthd::Unit::Percent));
  EXPECT_EQ(offset->result(), std::nullopt);

  offset->setX(Lengthd(1, Lengthd::Unit::Px));
  offset->setY(Lengthd(2, Lengthd::Unit::Px));
  offset->setWidth(Lengthd(3, Lengthd::Unit::Px));
  offset->setHeight(Lengthd(4, Lengthd::Unit::Px));
  offset->setResult("out");

  EXPECT_THAT(offset->x(), LengthIs(1.0, Lengthd::Unit::Px));
  EXPECT_THAT(offset->y(), LengthIs(2.0, Lengthd::Unit::Px));
  EXPECT_THAT(offset->width(), LengthIs(3.0, Lengthd::Unit::Px));
  EXPECT_THAT(offset->height(), LengthIs(4.0, Lengthd::Unit::Px));
  EXPECT_THAT(offset->result(), testing::Optional(testing::Eq("out")));
}

}  // namespace
}  // namespace donner::svg
