#include "donner/css/VariableSubstitution.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "donner/css/VariableGraph.h"
#include "donner/css/parser/DeclarationListParser.h"

namespace donner::css {
namespace {

using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::SizeIs;
using Values = std::span<const ComponentValue>;

std::vector<ComponentValue> Tokens(std::string_view text) {
  const auto declarations =
      parser::DeclarationListParser::ParseOnlyDeclarations("test:" + std::string(text));
  EXPECT_THAT(declarations, SizeIs(1));
  return declarations.empty() ? std::vector<ComponentValue>{} : declarations.front().values;
}

std::string Text(Values values) {
  std::string result;
  for (const auto& value : values) {
    result += value.toCssText();
  }
  return result;
}

TEST(VariableSubstitution, InspectsNestedReferencesIncludingUnusedFallbacks) {
  const auto tokens = Tokens("outer([VaR(--X, var(--y))]) var(--z,)");
  std::vector<std::string> names;
  EXPECT_TRUE(
      VisitVariableReferences(tokens, [&](std::string_view name) { names.emplace_back(name); }));
  EXPECT_THAT(names, ElementsAre("--X", "--y", "--z"));
  EXPECT_EQ(InspectVariableReferences(Tokens("\"var(--x)\"")), VariableReferences::None);
  for (auto spelling :
       {"var(--)", "var(3)", "var(--x keyword)", "var(--x, var(name))", "var(--x, !bad)"}) {
    EXPECT_EQ(InspectVariableReferences(Tokens(spelling)), VariableReferences::Invalid) << spelling;
  }
}

TEST(VariableSubstitution, KeepsTokenBoundariesAndOriginalCase) {
  const auto integer = Tokens("20");
  const auto input = Tokens("calc(var(--X)px + var(--x,2px))");
  const auto result =
      SubstituteVariables(input, [&](std::string_view name) -> std::optional<Values> {
        return name == "--X" ? std::optional<Values>(integer) : std::nullopt;
      });
  ASSERT_EQ(result.status, VariableSubstitutionStatus::Resolved);
  ASSERT_THAT(result.values, SizeIs(1));
  ASSERT_TRUE(result.values.front().is<Function>());
  const auto& children = result.values.front().get<Function>().values;
  ASSERT_THAT(children, SizeIs(6));
  EXPECT_TRUE(children[0].isToken<Token::Number>());
  EXPECT_TRUE(children[1].isToken<Token::Ident>());
  EXPECT_EQ(Text(result.values), "calc(20px + 2px)");
}

TEST(VariableSubstitution, DistinguishesMissingAndEmptyAndPreservesFallbackCommas) {
  const VariableLookup lookup = [](std::string_view name) -> std::optional<Values> {
    return name == "--empty" ? std::optional<Values>(Values{}) : std::nullopt;
  };
  const auto empty = SubstituteVariables(Tokens("var(--empty,red)"), lookup);
  EXPECT_EQ(empty.status, VariableSubstitutionStatus::Resolved);
  EXPECT_THAT(empty.values, IsEmpty());
  const auto fallback = SubstituteVariables(Tokens("var(--a,var(--b,blue, green))"), lookup);
  EXPECT_EQ(fallback.status, VariableSubstitutionStatus::Resolved);
  EXPECT_EQ(Text(fallback.values), "blue, green");
  const auto emptyFallback = SubstituteVariables(Tokens("var(--a,)"), lookup);
  EXPECT_EQ(emptyFallback.status, VariableSubstitutionStatus::Resolved);
  EXPECT_THAT(emptyFallback.values, IsEmpty());
  const auto missing = SubstituteVariables(Tokens("red var(--missing)"), lookup);
  EXPECT_EQ(missing.status, VariableSubstitutionStatus::Invalid);
  EXPECT_THAT(missing.values, IsEmpty());
}

TEST(VariableSubstitution, AppliesComponentByteAndAggregateAllowancesBeforeAppending) {
  const auto replacement = Tokens("fn(blue)");
  const VariableLookup lookup = [&](std::string_view) -> std::optional<Values> {
    return replacement;
  };
  const auto input = Tokens("var(--x)");
  EXPECT_EQ(SubstituteVariables(input, lookup, {.components = 2, .bytes = 8}).status,
            VariableSubstitutionStatus::Resolved);
  for (const auto limits : {VariableSubstitutionLimits{1, 8}, VariableSubstitutionLimits{2, 7}}) {
    const auto result = SubstituteVariables(input, lookup, limits);
    EXPECT_EQ(result.status, VariableSubstitutionStatus::LimitExceeded);
    EXPECT_THAT(result.values, IsEmpty());
  }
  std::size_t work = 0;
  const auto result = SubstituteVariables(input, lookup, {}, [&](std::size_t, std::size_t bytes) {
    work += bytes;
    return work <= 4;
  });
  EXPECT_EQ(result.status, VariableSubstitutionStatus::LimitExceeded);
  EXPECT_THAT(result.values, IsEmpty());
}

TEST(VariableSubstitution, ParsedDeclarationsRecordWhetherSubstitutionIsRequired) {
  const auto declarations = parser::DeclarationListParser::ParseOnlyDeclarations(
      "fill: blue; stroke:var(--ink); --shade:rgb(var(--r),0,0); opacity:var(oops)");
  ASSERT_THAT(declarations, SizeIs(4));
  EXPECT_EQ(declarations[0].variableReferences, VariableReferences::None);
  EXPECT_EQ(declarations[1].variableReferences, VariableReferences::Valid);
  EXPECT_EQ(declarations[2].variableReferences, VariableReferences::Valid);
  EXPECT_EQ(declarations[3].variableReferences, VariableReferences::Invalid);
}

std::optional<std::vector<VariableSubstitutionResult>> CollectDefinitions(
    std::span<const VariableDefinition> definitions, const VariableLookup& inherited) {
  std::vector<VariableSubstitutionResult> values(definitions.size(),
                                                 {VariableSubstitutionStatus::Invalid, {}});
  const VariableOutputs outputs{
      [&](std::size_t index) -> std::optional<Values> {
        if (values[index].status != VariableSubstitutionStatus::Resolved) {
          return std::nullopt;
        }
        return values[index].values;
      },
      [&](std::size_t index, VariableSubstitutionResult value) {
        values[index] = std::move(value);
        return true;
      },
  };
  if (!ResolveVariableDefinitions(definitions, inherited, outputs)) {
    return std::nullopt;
  }
  return values;
}

TEST(VariableSubstitution, UnusedFallbackEdgesDoNotCreateCycles) {
  const auto aa = Tokens("var(--y,var(--b))");
  const auto bb = Tokens("var(--y,var(--a))");
  const auto yy = Tokens("green");
  const std::vector<VariableDefinition> definitions = {
      {"--a", Values(aa)}, {"--b", Values(bb)}, {"--y", Values(yy)}};
  auto result = CollectDefinitions(
      definitions, [](std::string_view) -> std::optional<Values> { return std::nullopt; });
  ASSERT_THAT(result, testing::Optional(SizeIs(3)));
  EXPECT_EQ(Text((*result)[0].values), "green");
  EXPECT_EQ(Text((*result)[1].values), "green");
  EXPECT_EQ(Text((*result)[2].values), "green");
}

TEST(VariableSubstitution, ActualCycleStaysInvalidAndDependentUsesFallback) {
  const auto aa = Tokens("var(--b,red)");
  const auto bb = Tokens("var(--a,blue)");
  const auto cc = Tokens("var(--a,green)");
  const std::vector<VariableDefinition> definitions = {
      {"--c", Values(cc)}, {"--a", Values(aa)}, {"--b", Values(bb)}};
  auto result = CollectDefinitions(
      definitions, [](std::string_view) -> std::optional<Values> { return std::nullopt; });
  ASSERT_THAT(result, testing::Optional(SizeIs(3)));
  EXPECT_EQ(Text((*result)[0].values), "green");
  EXPECT_EQ((*result)[1].status, VariableSubstitutionStatus::Invalid);
  EXPECT_EQ((*result)[2].status, VariableSubstitutionStatus::Invalid);
}

TEST(VariableSubstitution, SelectedFallbackCanCreateCycle) {
  const auto aa = Tokens("var(--missing,var(--b))");
  const auto bb = Tokens("var(--a)");
  const std::vector<VariableDefinition> definitions = {{"--a", Values(aa)}, {"--b", Values(bb)}};
  auto result = CollectDefinitions(
      definitions, [](std::string_view) -> std::optional<Values> { return std::nullopt; });
  ASSERT_THAT(result, testing::Optional(SizeIs(2)));
  EXPECT_EQ((*result)[0].status, VariableSubstitutionStatus::Invalid);
  EXPECT_EQ((*result)[1].status, VariableSubstitutionStatus::Invalid);
}

TEST(VariableSubstitution, OverlappingCyclesInvalidateTheirUnion) {
  const auto xx = Tokens("var(--a,valid)");
  const auto aa = Tokens("var(--b,cycle)");
  const auto bb = Tokens("var(--c,cycle)");
  const auto cc = Tokens("var(--d,cycle) var(--a,cycle)");
  const auto dd = Tokens("var(--b,cycle) var(--y)");
  const auto yy = Tokens("valid");
  const std::vector<VariableDefinition> definitions = {{"--x", Values(xx)}, {"--a", Values(aa)},
                                                       {"--b", Values(bb)}, {"--c", Values(cc)},
                                                       {"--d", Values(dd)}, {"--y", Values(yy)}};
  auto result = CollectDefinitions(
      definitions, [](std::string_view) -> std::optional<Values> { return std::nullopt; });
  ASSERT_THAT(result, testing::Optional(SizeIs(6)));
  EXPECT_EQ(Text((*result)[0].values), "valid");
  for (const auto i : {1u, 2u, 3u, 4u}) {
    EXPECT_EQ((*result)[i].status, VariableSubstitutionStatus::Invalid) << i;
  }
  EXPECT_EQ(Text((*result)[5].values), "valid");
}

TEST(VariableSubstitution, EachActiveCycleFrameIsChargedOnce) {
  const auto outer = Tokens("var(--inner,teal)");
  const auto inner = Tokens("var(--inner,var(--branch,navy))");
  const auto branch = Tokens("var(--outer,olive)");
  const std::vector<VariableDefinition> definitions = {
      {"--outer", Values(outer)}, {"--inner", Values(inner)}, {"--branch", Values(branch)}};

  std::vector<VariableSubstitutionStatus> statuses;
  const VariableOutputs outputs{
      [](std::size_t) -> std::optional<Values> { return std::nullopt; },
      [&](std::size_t, VariableSubstitutionResult value) {
        statuses.push_back(value.status);
        return true;
      },
  };
  std::vector<std::pair<std::size_t, std::size_t>> charges;
  EXPECT_TRUE(ResolveVariableDefinitions(
      definitions, [](std::string_view) -> std::optional<Values> { return std::nullopt; },
      outputs, {}, [&](std::size_t work, std::size_t bytes) {
        charges.emplace_back(work, bytes);
        return true;
      }));
  EXPECT_THAT(statuses, ElementsAre(VariableSubstitutionStatus::Invalid,
                                   VariableSubstitutionStatus::Invalid,
                                   VariableSubstitutionStatus::Invalid));
  EXPECT_THAT(charges, ElementsAre(testing::Pair(1u, 0u), testing::Pair(2u, 0u)));
}

TEST(VariableSubstitution, InitialLocalValueShadowsAnInheritedValue) {
  const auto inherited = Tokens("red");
  const auto fallback = Tokens("var(--x,blue)");
  const auto empty = Tokens("var(--x,)");
  const std::vector<VariableDefinition> definitions = {
      {"--x", std::nullopt}, {"--b", Values(fallback)}, {"--c", Values(empty)}};
  auto result = CollectDefinitions(
      definitions, [&](std::string_view) -> std::optional<Values> { return inherited; });
  ASSERT_THAT(result, testing::Optional(SizeIs(3)));
  EXPECT_EQ((*result)[0].status, VariableSubstitutionStatus::Invalid);
  EXPECT_EQ(Text((*result)[1].values), "blue");
  EXPECT_EQ((*result)[2].status, VariableSubstitutionStatus::Resolved);
  EXPECT_THAT((*result)[2].values, IsEmpty());
}

TEST(VariableSubstitution, ComputedReferencesAreResolvedOnceInDependencyOrder) {
  const auto ref1 = Tokens("var(--b) var(--b)");
  const auto ref2 = Tokens("var(--c,ignored)");
  const auto literal = Tokens("blue");
  const std::vector<VariableDefinition> definitions = {
      {"--a", Values(ref1)}, {"--b", Values(ref2)}, {"--c", Values(literal)}};
  const auto result = CollectDefinitions(
      definitions, [](std::string_view) -> std::optional<Values> { return std::nullopt; });
  ASSERT_THAT(result, testing::Optional(SizeIs(3)));
  EXPECT_EQ(Text((*result)[0].values), "blue blue");
  EXPECT_EQ(Text((*result)[1].values), "blue");
  EXPECT_EQ(Text((*result)[2].values), "blue");
}

TEST(VariableSubstitution, ResolvesDeepDependencyChainWithoutNativeRecursion) {
  constexpr std::size_t kDepth = 2048;
  std::vector<std::string> names;
  std::vector<std::vector<ComponentValue>> tokens;
  names.reserve(kDepth);
  tokens.reserve(kDepth);
  for (std::size_t i = 0; i < kDepth; ++i) {
    names.emplace_back("--v" + std::to_string(i));
    tokens.emplace_back(
        Tokens(i + 1 == kDepth ? "green" : "var(--v" + std::to_string(i + 1) + ")"));
  }
  std::vector<VariableDefinition> definitions;
  definitions.reserve(kDepth);
  for (std::size_t i = 0; i < kDepth; ++i) {
    definitions.push_back({names[i], Values(tokens[i])});
  }
  const auto result = CollectDefinitions(
      definitions, [](std::string_view) -> std::optional<Values> { return std::nullopt; });
  ASSERT_THAT(result, testing::Optional(SizeIs(kDepth)));
  EXPECT_EQ(Text(result->front().values), "green");
  EXPECT_EQ(Text(result->back().values), "green");
}

TEST(VariableSubstitution, StaticNameSyntaxRejectsDynamicNameAndDependentUsesFallback) {
  const auto invalid = Tokens("var(var(--name))");
  const auto dependent = Tokens("var(--bad,green)");
  const std::vector<VariableDefinition> definitions = {{"--bad", Values(invalid)},
                                                       {"--dependent", Values(dependent)}};
  auto result = CollectDefinitions(
      definitions, [](std::string_view) -> std::optional<Values> { return std::nullopt; });
  ASSERT_THAT(result, testing::Optional(SizeIs(2)));
  EXPECT_EQ((*result)[0].status, VariableSubstitutionStatus::Invalid);
  EXPECT_EQ(Text((*result)[1].values), "green");
}

TEST(VariableSubstitution, InvalidDefinitionListReturnsFalseWithoutPublishing) {
  const auto red = Tokens("red");
  const std::vector<VariableDefinition> definitions = {{"--a", Values(red)}, {"--a", Values(red)}};
  std::size_t publications = 0;
  const VariableOutputs outputs{
      [](std::size_t) -> std::optional<Values> { return std::nullopt; },
      [&](std::size_t, VariableSubstitutionResult) {
        ++publications;
        return true;
      },
  };
  EXPECT_FALSE(ResolveVariableDefinitions(
      definitions, [](std::string_view) -> std::optional<Values> { return std::nullopt; },
      outputs));
  EXPECT_EQ(publications, 0u);
}

TEST(VariableSubstitution, PerValueLimitPublishesInvalidAndDependentUsesFallback) {
  const auto tooLarge = Tokens("red blue");
  const auto dependent = Tokens("var(--large,green)");
  const std::vector<VariableDefinition> definitions = {{"--dependent", Values(dependent)},
                                                       {"--large", Values(tooLarge)}};
  std::vector<VariableSubstitutionResult> values(definitions.size(),
                                                 {VariableSubstitutionStatus::Invalid, {}});
  const VariableOutputs outputs{
      [&](std::size_t index) -> std::optional<Values> {
        return values[index].status == VariableSubstitutionStatus::Resolved
                   ? std::optional<Values>(values[index].values)
                   : std::nullopt;
      },
      [&](std::size_t index, VariableSubstitutionResult value) {
        values[index] = std::move(value);
        return true;
      },
  };
  EXPECT_TRUE(ResolveVariableDefinitions(
      definitions, [](std::string_view) -> std::optional<Values> { return std::nullopt; }, outputs,
      {.components = 1, .bytes = 1024}));
  EXPECT_EQ(Text(values[0].values), "green");
  EXPECT_EQ(values[1].status, VariableSubstitutionStatus::LimitExceeded);
}

TEST(VariableSubstitution, AggregateAdmissionRefusalStopsBeforePublishingCurrentValue) {
  const auto red = Tokens("red");
  const auto blue = Tokens("blue");
  const std::vector<VariableDefinition> definitions = {{"--first", Values(red)},
                                                       {"--second", Values(blue)}};
  std::vector<std::optional<VariableSubstitutionResult>> staged(definitions.size());
  std::size_t publications = 0;
  std::size_t admissions = 0;
  const VariableOutputs outputs{
      [&](std::size_t index) -> std::optional<Values> {
        if (!staged[index] || staged[index]->status != VariableSubstitutionStatus::Resolved) {
          return std::nullopt;
        }
        return staged[index]->values;
      },
      [&](std::size_t index, VariableSubstitutionResult value) {
        ++publications;
        staged[index] = std::move(value);
        return true;
      },
  };
  const bool resolved = ResolveVariableDefinitions(
      definitions, [](std::string_view) -> std::optional<Values> { return std::nullopt; }, outputs,
      {}, [&](std::size_t, std::size_t) { return ++admissions == 1; });
  EXPECT_FALSE(resolved);
  EXPECT_EQ(admissions, 2u);
  EXPECT_EQ(publications, 1u);
  staged.clear();
  EXPECT_THAT(staged, IsEmpty());
}

}  // namespace
}  // namespace donner::css
