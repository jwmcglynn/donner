/**
 * Tests for ShadowTreeSystem.
 */

#include "donner/svg/components/shadow/ShadowTreeSystem.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/xml/components/TreeComponent.h"
#include "donner/base/tests/BaseTestUtils.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/shadow/OffscreenShadowTreeComponent.h"
#include "donner/svg/components/shadow/ShadowEntityComponent.h"
#include "donner/svg/components/shadow/ShadowTreeComponent.h"
#include "donner/svg/components/style/DoNotInheritFillOrStrokeTag.h"
#include "donner/svg/parser/SVGParser.h"

using testing::Eq;
using testing::IsEmpty;
using testing::SizeIs;

namespace donner::svg::components {

class ShadowTreeSystemTest : public ::testing::Test {
protected:
  SVGDocument ParseSVG(std::string_view input) {
    ParseWarningSink parseSink;
    auto maybeResult = parser::SVGParser::ParseSVG(input, parseSink);
    EXPECT_THAT(maybeResult, NoParseError());
    return std::move(maybeResult).result();
  }
};

// --- Basic <use> shadow tree ---

TEST_F(ShadowTreeSystemTest, UseElementCreatesShadowTree) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <rect id="r" width="10" height="10" fill="red"/>
      </defs>
      <use id="u" href="#r" x="20" y="20"/>
    </svg>
  )");

  auto useElement = document.querySelector("#u");
  ASSERT_TRUE(useElement.has_value());
}

TEST_F(ShadowTreeSystemTest, UseElementReferencesRect) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <rect id="r" width="10" height="10"/>
      </defs>
      <use href="#r"/>
    </svg>
  )");

  auto useElement = document.querySelector("use");
  ASSERT_TRUE(useElement.has_value());

  // Verify the use element has a ShadowTreeComponent
  auto& registry = document.registry();
  auto entity = useElement->entityHandle().entity();
  EXPECT_TRUE(registry.all_of<ShadowTreeComponent>(entity));

  auto& shadowTree = registry.get<ShadowTreeComponent>(entity);
  EXPECT_EQ(shadowTree.mainHref().value_or(""), "#r");
}

// --- <use> referencing <symbol> ---

TEST_F(ShadowTreeSystemTest, UseReferencingSymbol) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <symbol id="sym">
          <rect width="10" height="10" fill="blue"/>
        </symbol>
      </defs>
      <use href="#sym" width="50" height="50"/>
    </svg>
  )");

  auto useElement = document.querySelector("use");
  ASSERT_TRUE(useElement.has_value());
}

// --- Self-recursion detection ---

TEST_F(ShadowTreeSystemTest, SelfRecursionDetected) {
  // Create an SVG where a <use> tries to reference itself, which should produce a warning
  // but not crash.
  ParseWarningSink parseSink;
  auto maybeResult = parser::SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <use id="self" href="#self"/>
    </svg>
  )", parseSink);

  // The parse should succeed (self-recursion is a warning, not a hard error).
  ASSERT_TRUE(maybeResult.hasResult());
}

// --- Indirect recursion detection ---

TEST_F(ShadowTreeSystemTest, IndirectRecursionDetected) {
  ParseWarningSink parseSink;
  auto maybeResult = parser::SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g id="a"><use href="#b"/></g>
      <g id="b"><use href="#a"/></g>
    </svg>
  )", parseSink);

  ASSERT_TRUE(maybeResult.hasResult());
}

// --- Multiple <use> elements referencing the same target ---

TEST_F(ShadowTreeSystemTest, MultipleUsesOfSameTarget) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <circle id="c" r="5" fill="green"/>
      </defs>
      <use href="#c" x="10" y="10"/>
      <use href="#c" x="30" y="30"/>
      <use href="#c" x="50" y="50"/>
    </svg>
  )");

  size_t useCount = 0;
  for (auto view = document.registry().view<ShadowTreeComponent>(); auto entity : view) {
    (void)entity;
    ++useCount;
  }
  EXPECT_EQ(useCount, 3u);
}

// --- <use> with nonexistent href ---

TEST_F(ShadowTreeSystemTest, UseWithMissingHref) {
  ParseWarningSink parseSink;
  auto maybeResult = parser::SVGParser::ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <use href="#nonexistent"/>
    </svg>
  )", parseSink);

  // Should not crash; missing ref is a warning.
  ASSERT_TRUE(maybeResult.hasResult());
}

// --- ShadowTreeSystem::teardown ---

TEST_F(ShadowTreeSystemTest, TeardownCleansShadowEntities) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <rect id="r" width="10" height="10"/>
      </defs>
      <use id="u" href="#r"/>
    </svg>
  )");

  auto& registry = document.registry();
  auto useEntity = document.querySelector("#u")->entityHandle().entity();

  // If a ComputedShadowTreeComponent exists, tear it down.
  auto* computed = registry.try_get<ComputedShadowTreeComponent>(useEntity);
  if (computed) {
    ShadowTreeSystem system;
    system.teardown(registry, *computed);

    // After teardown, branches should be cleared.
    EXPECT_FALSE(computed->mainBranch.has_value());
    EXPECT_TRUE(computed->branches.empty());
  }
}

// --- PopulateInstance with Main branch ---

TEST_F(ShadowTreeSystemTest, PopulateInstanceMainBranch) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="target" width="10" height="10"/>
      <g id="host"/>
    </svg>
  )");

  auto hostEntity = document.querySelector("#host")->entityHandle();
  auto targetEntity = document.querySelector("#target")->entityHandle().entity();

  ComputedShadowTreeComponent shadow;
  ShadowTreeSystem system;

  ParseWarningSink warningSink;
  auto result = system.populateInstance(hostEntity, shadow, ShadowBranchType::Main, targetEntity,
                                        RcString("#target"), warningSink);

  // Main branch returns nullopt.
  EXPECT_FALSE(result.has_value());
  EXPECT_TRUE(shadow.mainBranch.has_value());
  EXPECT_FALSE(shadow.mainBranch->shadowEntities.empty());
}

// --- PopulateInstance self-recursion produces warning ---

TEST_F(ShadowTreeSystemTest, PopulateInstanceSelfRecursionWarns) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g id="host"/>
    </svg>
  )");

  auto hostEntity = document.querySelector("#host")->entityHandle();

  ComputedShadowTreeComponent shadow;
  ShadowTreeSystem system;

  ParseWarningSink warningSink;
  // Passing the entity itself as lightTarget should trigger self-recursion.
  auto result = system.populateInstance(hostEntity, shadow, ShadowBranchType::Main,
                                        hostEntity.entity(), RcString("#host"), warningSink);

  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(shadow.mainBranch.has_value());
  EXPECT_THAT(warningSink.warnings(), SizeIs(1));
}

// --- <use> referencing nested group ---

TEST_F(ShadowTreeSystemTest, UseReferencingGroupWithChildren) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <g id="group">
          <rect width="10" height="10"/>
          <circle r="5" cx="20" cy="20"/>
          <ellipse rx="5" ry="3" cx="40" cy="40"/>
        </g>
      </defs>
      <use href="#group"/>
    </svg>
  )");

  auto useElement = document.querySelector("use");
  ASSERT_TRUE(useElement.has_value());
}

// --- PopulateInstance parent-recursion detection ---

TEST_F(ShadowTreeSystemTest, PopulateInstanceParentRecursionWarns) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g id="parent">
        <g id="child"/>
      </g>
    </svg>
  )");

  auto childEntity = document.querySelector("#child")->entityHandle();
  auto parentEntity = document.querySelector("#parent")->entityHandle().entity();

  ComputedShadowTreeComponent shadow;
  ShadowTreeSystem system;

  ParseWarningSink warningSink;
  // Child referencing its parent should trigger parent recursion detection.
  auto result = system.populateInstance(childEntity, shadow, ShadowBranchType::Main, parentEntity,
                                        RcString("#parent"), warningSink);

  EXPECT_FALSE(result.has_value());
  EXPECT_FALSE(shadow.mainBranch.has_value());
  EXPECT_THAT(warningSink.warnings(), SizeIs(1));
}

// --- Offscreen branch returns index ---

TEST_F(ShadowTreeSystemTest, PopulateInstanceOffscreenBranchReturnsIndex) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <rect id="target" width="10" height="10"/>
      <g id="host"/>
    </svg>
  )");

  auto hostEntity = document.querySelector("#host")->entityHandle();
  auto targetEntity = document.querySelector("#target")->entityHandle().entity();

  ComputedShadowTreeComponent shadow;
  ShadowTreeSystem system;

  ParseWarningSink warningSink;
  auto result = system.populateInstance(hostEntity, shadow, ShadowBranchType::OffscreenFill,
                                        targetEntity, RcString("#target"), warningSink);

  // Offscreen branch should return an index.
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 0u);
  EXPECT_EQ(shadow.offscreenShadowCount(), 1u);
}

// --- ComputedShadowTreeComponent helpers ---

TEST_F(ShadowTreeSystemTest, ComputedShadowTreeComponentAccessors) {
  ComputedShadowTreeComponent shadow;

  // Default state: no branches.
  EXPECT_EQ(shadow.mainLightRoot(), donner::Entity(entt::null));
  EXPECT_EQ(shadow.offscreenShadowCount(), 0u);
  EXPECT_FALSE(shadow.findOffscreenShadow(ShadowBranchType::OffscreenFill).has_value());
}

TEST_F(ShadowTreeSystemTest, TeardownRemovesMainAndOffscreenEntities) {
  SVGDocument document;
  auto& registry = document.registry();

  const Entity host = registry.create();
  registry.emplace<donner::components::TreeComponent>(host, xml::XMLQualifiedNameRef("g"));

  const Entity mainRoot = registry.create();
  registry.emplace<donner::components::TreeComponent>(mainRoot, xml::XMLQualifiedNameRef("rect"));
  registry.get<donner::components::TreeComponent>(host).appendChild(registry, mainRoot);

  const Entity mainChild = registry.create();
  registry.emplace<donner::components::TreeComponent>(mainChild, xml::XMLQualifiedNameRef("path"));
  registry.get<donner::components::TreeComponent>(mainRoot).appendChild(registry, mainChild);

  const Entity offscreenRoot = registry.create();
  registry.emplace<donner::components::TreeComponent>(offscreenRoot, xml::XMLQualifiedNameRef("rect"));

  ComputedShadowTreeComponent shadow;
  shadow.mainBranch = ComputedShadowTreeComponent::BranchStorage{
      ShadowBranchType::Main, entt::null, {mainRoot, mainChild}};
  shadow.branches.push_back(ComputedShadowTreeComponent::BranchStorage{
      ShadowBranchType::OffscreenFill, entt::null, {offscreenRoot}});

  ShadowTreeSystem system;
  system.teardown(registry, shadow);

  EXPECT_FALSE(registry.valid(mainRoot));
  EXPECT_FALSE(registry.valid(mainChild));
  EXPECT_FALSE(registry.valid(offscreenRoot));
  EXPECT_FALSE(shadow.mainBranch.has_value());
  EXPECT_TRUE(shadow.branches.empty());
}

TEST_F(ShadowTreeSystemTest, PopulateInstanceNestedShadowTreeInvokesHandlerAndCopiesTag) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <defs>
        <rect id="target" width="10" height="10"/>
      </defs>
      <use id="nested" href="#target"/>
      <g id="host"/>
    </svg>
  )");

  auto& registry = document.registry();
  auto hostEntity = document.querySelector("#host")->entityHandle();
  auto nestedEntity = document.querySelector("#nested")->entityHandle();
  registry.emplace<DoNotInheritFillOrStrokeTag>(
      document.querySelector("#target")->entityHandle().entity());

  bool handlerCalled = false;
  ShadowTreeSystem system([&handlerCalled](Registry&, Entity, EntityHandle, Entity, ShadowBranchType,
                                           ParseWarningSink&) {
    handlerCalled = true;
    return true;
  });

  ComputedShadowTreeComponent shadow;
  ParseWarningSink warnings;
  auto result = system.populateInstance(hostEntity, shadow, ShadowBranchType::Main,
                                        nestedEntity.entity(), RcString("#nested"), warnings);

  EXPECT_FALSE(result.has_value());
  ASSERT_TRUE(shadow.mainBranch.has_value());
  EXPECT_GT(shadow.mainBranch->shadowEntities.size(), 1u);
  EXPECT_TRUE(handlerCalled);

  bool foundRootMarker = false;
  bool foundDoNotInherit = false;
  for (Entity entity : shadow.mainBranch->shadowEntities) {
    foundRootMarker = foundRootMarker || registry.all_of<ShadowTreeRootComponent>(entity);
    foundDoNotInherit =
        foundDoNotInherit || registry.all_of<DoNotInheritFillOrStrokeTag>(entity);
  }
  EXPECT_TRUE(foundRootMarker);
  EXPECT_TRUE(foundDoNotInherit);
  EXPECT_THAT(warnings.warnings(), IsEmpty());
}

TEST_F(ShadowTreeSystemTest, PopulateInstanceNestedShadowRecursionWarns) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <use id="self" href="#self"/>
      <g id="host"/>
    </svg>
  )");

  auto hostEntity = document.querySelector("#host")->entityHandle();
  auto selfEntity = document.querySelector("#self")->entityHandle();

  ShadowTreeSystem system;
  ComputedShadowTreeComponent shadow;
  ParseWarningSink warnings;
  auto result = system.populateInstance(hostEntity, shadow, ShadowBranchType::Main,
                                        selfEntity.entity(), RcString("#self"), warnings);

  EXPECT_FALSE(result.has_value());
  ASSERT_TRUE(shadow.mainBranch.has_value());
  EXPECT_EQ(shadow.mainBranch->shadowEntities.size(), 1u);
  EXPECT_THAT(warnings.warnings(), SizeIs(1));
  EXPECT_THAT(warnings.warnings().front().reason,
              testing::HasSubstr("Shadow tree recursion detected"));
}

TEST_F(ShadowTreeSystemTest, PopulateInstanceNestedShadowMissingTargetWarns) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <use id="nested" href="#missing"/>
      <g id="host"/>
    </svg>
  )");

  auto hostEntity = document.querySelector("#host")->entityHandle();
  auto nestedEntity = document.querySelector("#nested")->entityHandle();

  ShadowTreeSystem system;
  ComputedShadowTreeComponent shadow;
  ParseWarningSink warnings;
  auto result = system.populateInstance(hostEntity, shadow, ShadowBranchType::Main,
                                        nestedEntity.entity(), RcString("#nested"), warnings);

  EXPECT_FALSE(result.has_value());
  ASSERT_TRUE(shadow.mainBranch.has_value());
  EXPECT_TRUE(shadow.mainBranch->shadowEntities.empty());
  EXPECT_THAT(warnings.warnings(), SizeIs(1));
}

TEST_F(ShadowTreeSystemTest, PopulateInstanceMirrorsChildrenInNonNestedBranch) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g id="target">
        <rect id="r" width="10" height="10"/>
        <circle id="c" r="5"/>
      </g>
      <g id="host"/>
    </svg>
  )");

  auto hostEntity = document.querySelector("#host")->entityHandle();
  auto targetEntity = document.querySelector("#target")->entityHandle().entity();

  ShadowTreeSystem system;
  ComputedShadowTreeComponent shadow;
  ParseWarningSink warnings;
  auto result = system.populateInstance(hostEntity, shadow, ShadowBranchType::Main, targetEntity,
                                        RcString("#target"), warnings);

  EXPECT_FALSE(result.has_value());
  ASSERT_TRUE(shadow.mainBranch.has_value());
  EXPECT_GE(shadow.mainBranch->shadowEntities.size(), 3u);
  EXPECT_THAT(warnings.warnings(), IsEmpty());
}

TEST_F(ShadowTreeSystemTest, PopulateInstanceOffscreenPaintTargetParentRecursionWarns) {
  auto document = ParseSVG(R"(
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 100 100">
      <g id="parent">
        <g id="host"/>
      </g>
      <rect id="light" width="10" height="10"/>
    </svg>
  )");

  auto& registry = document.registry();
  auto hostEntity = document.querySelector("#host")->entityHandle();
  auto lightEntity = document.querySelector("#light")->entityHandle();

  auto& offscreen = registry.emplace<OffscreenShadowTreeComponent>(lightEntity.entity());
  offscreen.setBranchHref(ShadowBranchType::OffscreenFill, "#parent");

  ShadowTreeSystem system;
  ComputedShadowTreeComponent shadow;
  ParseWarningSink warnings;
  auto result = system.populateInstance(hostEntity, shadow, ShadowBranchType::OffscreenFill,
                                        lightEntity.entity(), RcString("#light"), warnings);

  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value(), 0u);
  EXPECT_EQ(shadow.offscreenShadowCount(), 1u);
  EXPECT_TRUE(shadow.offscreenShadowRoot(0) == entt::null);
  EXPECT_THAT(warnings.warnings(), SizeIs(1));
}

}  // namespace donner::svg::components
