#include "donner/svg/SVGFilterElement.h"

#include <variant>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/svg/SVGFEComponentTransferElement.h"
#include "donner/svg/SVGFEFuncAElement.h"
#include "donner/svg/SVGFEFuncBElement.h"
#include "donner/svg/SVGFEFuncGElement.h"
#include "donner/svg/SVGFEFuncRElement.h"
#include "donner/svg/SVGFEGaussianBlurElement.h"
#include "donner/svg/components/SVGDocumentContext.h"
#include "donner/svg/components/filter/FilterComponent.h"
#include "donner/svg/components/filter/FilterPrimitiveComponent.h"
#include "donner/svg/components/filter/FilterSystem.h"
#include "donner/svg/components/resources/ImageComponent.h"
#include "donner/svg/tests/ParserTestUtils.h"

using testing::AllOf;

namespace donner::svg {

namespace {

auto XEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("x", &SVGFilterElement::x, LengthIs(valueMatcher, unitMatcher));
}

auto YEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("y", &SVGFilterElement::y, LengthIs(valueMatcher, unitMatcher));
}

auto WidthEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("width", &SVGFilterElement::width, LengthIs(valueMatcher, unitMatcher));
}

auto HeightEq(auto valueMatcher, auto unitMatcher) {
  return testing::Property("height", &SVGFilterElement::height,
                           LengthIs(valueMatcher, unitMatcher));
}

MATCHER_P(FilterHas, matchers, "") {
  return testing::ExplainMatchResult(matchers, arg.element, result_listener);
}

}  // namespace

TEST(SVGFilterElementTests, EnabledWithoutExperimental) {
  auto element = instantiateSubtreeElement("<filter />");
  EXPECT_THAT(element->tryCast<SVGFilterElement>(), testing::Ne(std::nullopt));
}

TEST(SVGFilterElementTests, Defaults) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto filter = instantiateSubtreeElementAs<SVGFilterElement>("<filter />", options);
  EXPECT_THAT(filter, FilterHas(AllOf(XEq(-10.0, Lengthd::Unit::Percent),      //
                                      YEq(-10.0, Lengthd::Unit::Percent),      //
                                      WidthEq(120.0, Lengthd::Unit::Percent),  //
                                      HeightEq(120.0, Lengthd::Unit::Percent))));

  EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::ObjectBoundingBox));
  EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::UserSpaceOnUse));
}

TEST(SVGFilterElementTests, SetRect) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
      R"(<filter x="10" y="20" width="30" height="40" />)", options);
  EXPECT_THAT(filter, FilterHas(AllOf(XEq(10.0, Lengthd::Unit::None),      //
                                      YEq(20.0, Lengthd::Unit::None),      //
                                      WidthEq(30.0, Lengthd::Unit::None),  //
                                      HeightEq(40.0, Lengthd::Unit::None))));
}

TEST(SVGFilterElementTests, FilterUnits) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;
  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter filterUnits="userSpaceOnUse" />)", options);
    EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::UserSpaceOnUse));
  }

  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter filterUnits="objectBoundingBox" />)", options);
    EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::ObjectBoundingBox));
  }

  // An invalid option will go back to the default.
  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter filterUnits="invalid" />)", options);
    EXPECT_THAT(filter->filterUnits(), testing::Eq(FilterUnits::Default));
  }
}

TEST(SVGFilterElementTests, PrimitiveUnits) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter primitiveUnits="userSpaceOnUse" />)", options);
    EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::UserSpaceOnUse));
  }

  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter primitiveUnits="objectBoundingBox" />)", options);
    EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::ObjectBoundingBox));
  }

  // An invalid option will go back to the default.
  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter primitiveUnits="invalid" />)", options);
    EXPECT_THAT(filter->primitiveUnits(), testing::Eq(PrimitiveUnits::Default));
  }
}

TEST(SVGFilterElementTests, Href) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  {
    auto filter =
        instantiateSubtreeElementAs<SVGFilterElement>(R"(<filter href="#baseFilter" />)", options);
    EXPECT_THAT(filter->href(), testing::Optional(testing::Eq("#baseFilter")));
  }

  {
    auto filter = instantiateSubtreeElementAs<SVGFilterElement>(
        R"(<filter xmlns:xlink="http://www.w3.org/1999/xlink" xlink:href="#baseFilter" />)",
        options);
    EXPECT_THAT(filter->href(), testing::Optional(testing::Eq("#baseFilter")));
  }
}

TEST(SVGFilterElementTests, SetHrefCanClearAndInvalidatesComputedFilter) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto filter =
      instantiateSubtreeElementAs<SVGFilterElement>(R"(<filter href="#baseFilter" />)", options);
  filter->entityHandle().emplace<components::ComputedFilterComponent>();

  filter->setHref(std::nullopt);
  EXPECT_EQ(filter->href(), std::nullopt);
  EXPECT_FALSE(filter->entityHandle().all_of<components::ComputedFilterComponent>());
}

TEST(SVGFilterElementTests, HrefInheritsPrimitivesAndRegion) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  SVGDocument document = instantiateSubtree(R"(
    <defs>
      <filter id="filter0" x="0.1" y="0.2" width="0.8" height="1.0">
        <feGaussianBlur stdDeviation="4"/>
      </filter>
      <filter id="filter1" href="#filter0"/>
    </defs>
  )",
                                            options, Vector2i(200, 200));

  ParseWarningSink warningSink;
  components::FilterSystem().instantiateAllComputedComponents(document.registry(), warningSink);

  const entt::entity filter1Entity =
      document.registry().ctx().get<const components::SVGDocumentContext>().getEntityById(
          "filter1");
  ASSERT_TRUE(filter1Entity != entt::null);

  const auto* computed =
      document.registry().try_get<components::ComputedFilterComponent>(filter1Entity);
  ASSERT_NE(computed, nullptr);
  ASSERT_EQ(computed->filterGraph.nodes.size(), 1u);

  const auto* blur = std::get_if<components::filter_primitive::GaussianBlur>(
      &computed->filterGraph.nodes.front().primitive);
  ASSERT_NE(blur, nullptr);
  EXPECT_DOUBLE_EQ(blur->stdDeviationX, 4.0);
  EXPECT_DOUBLE_EQ(blur->stdDeviationY, 4.0);
  EXPECT_THAT(computed->x.value, testing::DoubleEq(0.1));
  EXPECT_THAT(computed->y.value, testing::DoubleEq(0.2));
  EXPECT_THAT(computed->width.value, testing::DoubleEq(0.8));
  EXPECT_THAT(computed->height.value, testing::DoubleEq(1.0));
}

TEST(SVGFilterElementTests, HrefInheritsAttributesButKeepsLocalPrimitives) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  SVGDocument document = instantiateSubtree(R"(
    <defs>
      <filter id="filter0" x="0.1" height="1.0">
        <feGaussianBlur stdDeviation="2"/>
      </filter>
      <filter id="filter1" y="0.2" width="0.8" href="#filter0">
        <feGaussianBlur stdDeviation="4"/>
      </filter>
    </defs>
  )",
                                            options, Vector2i(200, 200));

  ParseWarningSink warningSink;
  components::FilterSystem().instantiateAllComputedComponents(document.registry(), warningSink);

  const entt::entity filter1Entity =
      document.registry().ctx().get<const components::SVGDocumentContext>().getEntityById(
          "filter1");
  ASSERT_TRUE(filter1Entity != entt::null);

  const auto* computed =
      document.registry().try_get<components::ComputedFilterComponent>(filter1Entity);
  ASSERT_NE(computed, nullptr);
  ASSERT_EQ(computed->filterGraph.nodes.size(), 1u);

  const auto* blur = std::get_if<components::filter_primitive::GaussianBlur>(
      &computed->filterGraph.nodes.front().primitive);
  ASSERT_NE(blur, nullptr);
  EXPECT_DOUBLE_EQ(blur->stdDeviationX, 4.0);
  EXPECT_DOUBLE_EQ(blur->stdDeviationY, 4.0);
  EXPECT_THAT(computed->x.value, testing::DoubleEq(0.1));
  EXPECT_THAT(computed->y.value, testing::DoubleEq(0.2));
  EXPECT_THAT(computed->width.value, testing::DoubleEq(0.8));
  EXPECT_THAT(computed->height.value, testing::DoubleEq(1.0));
}

TEST(SVGFilterElementTests, RejectsInvalidFeFuncTableValuesWithUnitSuffix) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto transfer = instantiateSubtreeElementAs<SVGFEComponentTransferElement>(
      R"(
        <feComponentTransfer>
          <feFuncB type="table" tableValues="1px"/>
        </feComponentTransfer>
      )",
      options);

  auto maybeChild = transfer->firstChild();
  ASSERT_TRUE(maybeChild.has_value());

  auto funcB = maybeChild->cast<SVGFEFuncBElement>();
  const auto* component = funcB.entityHandle().try_get<components::FEFuncComponent>();
  ASSERT_NE(component, nullptr);
  EXPECT_TRUE(component->tableValues.empty());
}

TEST(SVGFilterElementTests, ParsesExperimentalPrimitiveAttributes) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  SVGDocument document = instantiateSubtree(R"(
    <svg xmlns="http://www.w3.org/2000/svg">
      <defs>
        <filter id="f">
          <feGaussianBlur id="blur" x="1" y="2" width="3" height="4" in="SourceGraphic"
                          result="blurOut" stdDeviation="5 6" edgeMode="wrap" />
          <feBlend id="blend" x="7" y="8" width="9" height="10" in="SourceAlpha"
                   in2="FillPaint" result="blendOut" mode="screen" />
          <feComponentTransfer id="transfer" x="11" y="12" width="13" height="14"
                               in="blurOut" result="transferOut">
            <feFuncR id="funcR" type="gamma" amplitude="2" exponent="3" offset="4" />
            <feFuncG id="funcG" type="linear" slope="1.5" intercept="0.5" />
            <feFuncB id="funcB" type="table" tableValues="0 0.5 1" />
            <feFuncA id="funcA" type="discrete" tableValues="1,0" />
          </feComponentTransfer>
          <feColorMatrix id="matrix" x="15" y="16" width="17" height="18" in="transferOut"
                         result="matrixOut" type="hueRotate" values="90" />
          <feComposite id="composite" x="19" y="20" width="21" height="22" in="StrokePaint"
                       in2="blendOut" operator="arithmetic" k1="1" k2="2" k3="3" k4="4" />
          <feDropShadow id="shadow" x="23" y="24" width="25" height="26" in="SourceGraphic"
                        dx="27" dy="28" stdDeviation="29 30" />
          <feFlood id="flood" x="31" y="32" width="33" height="34" result="floodOut"
                   flood-color="red" flood-opacity="0.5" />
          <feMorphology id="morph" x="35" y="36" width="37" height="38" in="matrixOut"
                        operator="dilate" radius="39 40" />
          <feDisplacementMap id="displace" x="41" y="42" width="43" height="44"
                             in="SourceGraphic" in2="blendOut" scale="45"
                             xChannelSelector="G" yChannelSelector="A" />
          <feImage id="image" x="46" y="47" width="48" height="49" href="texture.png"
                   preserveAspectRatio="none" />
          <feDiffuseLighting id="diffuse" x="50" y="51" width="52" height="53"
                             in="SourceGraphic" surfaceScale="54" diffuseConstant="55">
            <feDistantLight id="distant" azimuth="56" elevation="57" />
          </feDiffuseLighting>
          <feSpecularLighting id="specular" x="58" y="59" width="60" height="61"
                              in="SourceGraphic" surfaceScale="62" specularConstant="63"
                              specularExponent="64">
            <fePointLight id="point" x="65" y="66" z="67" />
          </feSpecularLighting>
          <feSpecularLighting id="specularSpot" in="SourceGraphic">
            <feSpotLight id="spot" x="68" y="69" z="70" pointsAtX="71" pointsAtY="72"
                         pointsAtZ="73" specularExponent="74" limitingConeAngle="75" />
          </feSpecularLighting>
          <feConvolveMatrix id="convolve" x="76" y="77" width="78" height="79"
                            in="SourceGraphic" order="3 4" kernelMatrix="1 2 3 4"
                            divisor="5" bias="6" targetX="1" targetY="2" edgeMode="wrap"
                            preserveAlpha="true" />
          <feTurbulence id="turbulence" x="80" y="81" width="82" height="83"
                        baseFrequency="0.1 0.2" numOctaves="3" seed="4"
                        type="fractalNoise" stitchTiles="stitch" />
          <feTile id="tile" x="84" y="85" width="86" height="87" in="blendOut"
                  result="tileOut" />
          <feOffset id="offset" x="88" y="89" width="90" height="91" in="SourceGraphic"
                    dx="92" dy="93" />
          <feMerge id="merge" x="94" y="95" width="96" height="97" result="mergeOut">
            <feMergeNode id="mergeNode" in="tileOut" />
          </feMerge>
        </filter>
      </defs>
    </svg>
  )",
                                            options);

  const auto query = [&document](const char* id) {
    const auto entity =
        document.registry().ctx().get<const components::SVGDocumentContext>().getEntityById(id);
    EXPECT_TRUE(entity != entt::null) << id;
    return EntityHandle(document.registry(), entity);
  };

  {
    const auto blur = query("blur");
    const auto& primitive = blur.get<components::FilterPrimitiveComponent>();
    const auto& component = blur.get<components::FEGaussianBlurComponent>();
    EXPECT_THAT(primitive.x, testing::Optional(Lengthd(1, Lengthd::Unit::None)));
    EXPECT_TRUE(std::holds_alternative<components::FilterStandardInput>(primitive.in->value));
    EXPECT_EQ(std::get<components::FilterStandardInput>(primitive.in->value),
              components::FilterStandardInput::SourceGraphic);
    EXPECT_THAT(primitive.result, testing::Optional(testing::Eq("blurOut")));
    EXPECT_DOUBLE_EQ(component.stdDeviationX, 5.0);
    EXPECT_DOUBLE_EQ(component.stdDeviationY, 6.0);
    EXPECT_EQ(component.edgeMode, components::FEGaussianBlurComponent::EdgeMode::Wrap);
  }

  {
    const auto blend = query("blend");
    const auto& primitive = blend.get<components::FilterPrimitiveComponent>();
    const auto& component = blend.get<components::FEBlendComponent>();
    EXPECT_TRUE(std::holds_alternative<components::FilterStandardInput>(primitive.in->value));
    EXPECT_EQ(std::get<components::FilterStandardInput>(primitive.in->value),
              components::FilterStandardInput::SourceAlpha);
    EXPECT_TRUE(std::holds_alternative<components::FilterStandardInput>(primitive.in2->value));
    EXPECT_EQ(std::get<components::FilterStandardInput>(primitive.in2->value),
              components::FilterStandardInput::FillPaint);
    EXPECT_EQ(component.mode, components::FEBlendComponent::Mode::Screen);
  }

  {
    const auto transfer = query("transfer");
    const auto& primitive = transfer.get<components::FilterPrimitiveComponent>();
    EXPECT_TRUE(std::holds_alternative<components::FilterInput::Named>(primitive.in->value));
    EXPECT_EQ(std::get<components::FilterInput::Named>(primitive.in->value).name, "blurOut");

    auto maybeTransferElement = document.querySelector("#transfer");
    ASSERT_TRUE(maybeTransferElement.has_value());
    auto transferElement = maybeTransferElement->cast<SVGFEComponentTransferElement>();
    ASSERT_TRUE(transferElement.firstChild().has_value());
    auto funcRHandle = transferElement.firstChild()->cast<SVGFEFuncRElement>().entityHandle();
    ASSERT_TRUE(transferElement.firstChild()->nextSibling().has_value());
    auto funcGHandle =
        transferElement.firstChild()->nextSibling()->cast<SVGFEFuncGElement>().entityHandle();
    ASSERT_TRUE(transferElement.firstChild()->nextSibling()->nextSibling().has_value());
    auto funcBHandle = transferElement.firstChild()
                           ->nextSibling()
                           ->nextSibling()
                           ->cast<SVGFEFuncBElement>()
                           .entityHandle();
    ASSERT_TRUE(
        transferElement.firstChild()->nextSibling()->nextSibling()->nextSibling().has_value());
    auto funcAHandle = transferElement.firstChild()
                           ->nextSibling()
                           ->nextSibling()
                           ->nextSibling()
                           ->cast<SVGFEFuncAElement>()
                           .entityHandle();
    const auto& funcR = funcRHandle.get<components::FEFuncComponent>();
    const auto& funcG = funcGHandle.get<components::FEFuncComponent>();
    const auto& funcB = funcBHandle.get<components::FEFuncComponent>();
    const auto& funcA = funcAHandle.get<components::FEFuncComponent>();
    EXPECT_EQ(funcR.type, components::FEFuncComponent::FuncType::Gamma);
    EXPECT_DOUBLE_EQ(funcR.amplitude, 2.0);
    EXPECT_DOUBLE_EQ(funcR.exponent, 3.0);
    EXPECT_DOUBLE_EQ(funcR.offset, 4.0);
    EXPECT_EQ(funcG.type, components::FEFuncComponent::FuncType::Linear);
    EXPECT_DOUBLE_EQ(funcG.slope, 1.5);
    EXPECT_DOUBLE_EQ(funcG.intercept, 0.5);
    EXPECT_EQ(funcB.type, components::FEFuncComponent::FuncType::Table);
    EXPECT_THAT(funcB.tableValues, testing::ElementsAre(0.0, 0.5, 1.0));
    EXPECT_EQ(funcA.type, components::FEFuncComponent::FuncType::Discrete);
    EXPECT_THAT(funcA.tableValues, testing::ElementsAre(1.0, 0.0));
  }

  {
    const auto matrix = query("matrix");
    const auto& primitive = matrix.get<components::FilterPrimitiveComponent>();
    const auto& component = matrix.get<components::FEColorMatrixComponent>();
    EXPECT_TRUE(std::holds_alternative<components::FilterInput::Named>(primitive.in->value));
    EXPECT_EQ(std::get<components::FilterInput::Named>(primitive.in->value).name, "transferOut");
    EXPECT_EQ(component.type, components::FEColorMatrixComponent::Type::HueRotate);
    EXPECT_THAT(component.values, testing::ElementsAre(90.0));
  }

  {
    const auto composite = query("composite");
    const auto& primitive = composite.get<components::FilterPrimitiveComponent>();
    const auto& component = composite.get<components::FECompositeComponent>();
    EXPECT_TRUE(std::holds_alternative<components::FilterStandardInput>(primitive.in->value));
    EXPECT_EQ(std::get<components::FilterStandardInput>(primitive.in->value),
              components::FilterStandardInput::StrokePaint);
    EXPECT_TRUE(std::holds_alternative<components::FilterInput::Named>(primitive.in2->value));
    EXPECT_EQ(std::get<components::FilterInput::Named>(primitive.in2->value).name, "blendOut");
    EXPECT_EQ(component.op, components::FECompositeComponent::Operator::Arithmetic);
    EXPECT_DOUBLE_EQ(component.k1, 1.0);
    EXPECT_DOUBLE_EQ(component.k2, 2.0);
    EXPECT_DOUBLE_EQ(component.k3, 3.0);
    EXPECT_DOUBLE_EQ(component.k4, 4.0);
  }

  {
    const auto shadow = query("shadow");
    const auto& component = shadow.get<components::FEDropShadowComponent>();
    EXPECT_DOUBLE_EQ(component.dx, 27.0);
    EXPECT_DOUBLE_EQ(component.dy, 28.0);
    EXPECT_DOUBLE_EQ(component.stdDeviationX, 29.0);
    EXPECT_DOUBLE_EQ(component.stdDeviationY, 30.0);
  }

  {
    const auto morph = query("morph");
    const auto& component = morph.get<components::FEMorphologyComponent>();
    EXPECT_EQ(component.op, components::FEMorphologyComponent::Operator::Dilate);
    EXPECT_DOUBLE_EQ(component.radiusX, 39.0);
    EXPECT_DOUBLE_EQ(component.radiusY, 40.0);
  }

  {
    const auto displace = query("displace");
    const auto& component = displace.get<components::FEDisplacementMapComponent>();
    EXPECT_DOUBLE_EQ(component.scale, 45.0);
    EXPECT_EQ(component.xChannelSelector, components::FEDisplacementMapComponent::Channel::G);
    EXPECT_EQ(component.yChannelSelector, components::FEDisplacementMapComponent::Channel::A);
  }

  {
    const auto image = query("image");
    const auto& component = image.get<components::FEImageComponent>();
    EXPECT_EQ(component.href, "texture.png");
    EXPECT_EQ(component.preserveAspectRatio, PreserveAspectRatio::None());
    EXPECT_EQ(image.get<components::ImageComponent>().href, "texture.png");
  }

  {
    const auto diffuse = query("diffuse");
    const auto& component = diffuse.get<components::FEDiffuseLightingComponent>();
    EXPECT_DOUBLE_EQ(component.surfaceScale, 54.0);
    EXPECT_DOUBLE_EQ(component.diffuseConstant, 55.0);
  }

  {
    const auto specular = query("specular");
    const auto& component = specular.get<components::FESpecularLightingComponent>();
    EXPECT_DOUBLE_EQ(component.surfaceScale, 62.0);
    EXPECT_DOUBLE_EQ(component.specularConstant, 63.0);
    EXPECT_DOUBLE_EQ(component.specularExponent, 64.0);
  }

  {
    const auto distant = query("distant");
    const auto& component = distant.get<components::LightSourceComponent>();
    EXPECT_DOUBLE_EQ(component.azimuth, 56.0);
    EXPECT_DOUBLE_EQ(component.elevation, 57.0);
  }

  {
    const auto point = query("point");
    const auto& component = point.get<components::LightSourceComponent>();
    EXPECT_DOUBLE_EQ(component.x, 65.0);
    EXPECT_DOUBLE_EQ(component.y, 66.0);
    EXPECT_DOUBLE_EQ(component.z, 67.0);
  }

  {
    const auto spot = query("spot");
    const auto& component = spot.get<components::LightSourceComponent>();
    EXPECT_DOUBLE_EQ(component.x, 68.0);
    EXPECT_DOUBLE_EQ(component.y, 69.0);
    EXPECT_DOUBLE_EQ(component.z, 70.0);
    EXPECT_DOUBLE_EQ(component.pointsAtX, 71.0);
    EXPECT_DOUBLE_EQ(component.pointsAtY, 72.0);
    EXPECT_DOUBLE_EQ(component.pointsAtZ, 73.0);
    EXPECT_DOUBLE_EQ(component.spotExponent, 74.0);
    EXPECT_THAT(component.limitingConeAngle, testing::Optional(75.0));
  }

  {
    const auto convolve = query("convolve");
    const auto& component = convolve.get<components::FEConvolveMatrixComponent>();
    EXPECT_EQ(component.orderX, 3);
    EXPECT_EQ(component.orderY, 4);
    EXPECT_THAT(component.kernelMatrix, testing::ElementsAre(1.0, 2.0, 3.0, 4.0));
    EXPECT_THAT(component.divisor, testing::Optional(5.0));
    EXPECT_DOUBLE_EQ(component.bias, 6.0);
    EXPECT_THAT(component.targetX, testing::Optional(1));
    EXPECT_THAT(component.targetY, testing::Optional(2));
    EXPECT_EQ(component.edgeMode, components::FEConvolveMatrixComponent::EdgeMode::Wrap);
    EXPECT_TRUE(component.preserveAlpha);
  }

  {
    const auto turbulence = query("turbulence");
    const auto& component = turbulence.get<components::FETurbulenceComponent>();
    EXPECT_EQ(component.type, components::FETurbulenceComponent::Type::FractalNoise);
    EXPECT_DOUBLE_EQ(component.baseFrequencyX, 0.1);
    EXPECT_DOUBLE_EQ(component.baseFrequencyY, 0.2);
    EXPECT_EQ(component.numOctaves, 3);
    EXPECT_DOUBLE_EQ(component.seed, 4.0);
    EXPECT_TRUE(component.stitchTiles);
  }

  {
    const auto offset = query("offset");
    const auto& component = offset.get<components::FEOffsetComponent>();
    EXPECT_DOUBLE_EQ(component.dx, 92.0);
    EXPECT_DOUBLE_EQ(component.dy, 93.0);
  }

  {
    const auto mergeNode = query("mergeNode");
    const auto& component = mergeNode.get<components::FEMergeNodeComponent>();
    ASSERT_TRUE(component.in.has_value());
    EXPECT_TRUE(std::holds_alternative<components::FilterInput::Named>(component.in->value));
    EXPECT_EQ(std::get<components::FilterInput::Named>(component.in->value).name, "tileOut");
  }
}

// TODO: Move to another file
TEST(SVGFEGaussianBlurElement, EnabledWithoutExperimental) {
  auto element = instantiateSubtreeElement("<feGaussianBlur />");
  EXPECT_THAT(element->tryCast<SVGFEGaussianBlurElement>(), testing::Ne(std::nullopt));
}

// TODO: Move to another file
TEST(SVGFEGaussianBlurElement, SetStdDeviation) {
  parser::SVGParser::Options options;
  options.enableExperimental = true;

  auto blur = instantiateSubtreeElementAs<SVGFEGaussianBlurElement>(
      "<feGaussianBlur stdDeviation=\"3\" />", options);
  EXPECT_EQ(blur->stdDeviationX(), 3.0);
  EXPECT_EQ(blur->stdDeviationY(), 3.0);
}

}  // namespace donner::svg
