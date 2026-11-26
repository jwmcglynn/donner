#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/components/style/ComputedStyleComponent.h"
#include "donner/svg/components/style/CustomPropertyMap.h"
#include "donner/svg/components/style/ExternalUseStyleContext.h"
#include "donner/svg/parser/SVGParser.h"
#include "donner/svg/renderer/ExternalUseStyleRetention.h"

namespace donner::svg::components {
namespace {

TEST(CustomPropertyRetentionTest, SavedOccurrenceOwnsInheritedMapAfterFirstRecordDies) {
  using ::testing::NotNull;
  using ::testing::Optional;
  using Kind = DocumentResourceFamilyBudget::Kind;
  const auto family = std::make_shared<DocumentResourceFamilyBudget>();
  ASSERT_TRUE(family->reserve(Kind::ParsedPayload, 53));
  const auto account = std::make_shared<RetainedStyleBytes>(1 << 20, family);
  std::shared_ptr<const ComputedStyle> first;
  std::shared_ptr<const ComputedStyle> inherited;
  {
    const std::vector<css::ComponentValue> values{
        css::ComponentValue(css::Token(css::Token::String(RcString(std::string(4096, 'r'))), 0))};
    const auto tokens = TokenSequence::Create(account, values);
    ASSERT_THAT(tokens, NotNull());
    auto map = CustomPropertyMap::Overlay(
        account, nullptr,
        std::array{CustomPropertyMap::Change{RcString("--shared"), CustomPropertyMap::Action::Set,
                                             tokens}});
    ASSERT_THAT(map, Optional(NotNull()));
    auto original = std::make_shared<ComputedStyle>();
    original->customProperties = std::move(*map);
    inherited = std::make_shared<const ComputedStyle>(*original);
    first = std::move(original);
  }

  const auto bytes = account->retainedBytes();
  ASSERT_GT(bytes, 4096u);
  ParseWarningSink warnings;
  auto parsed = parser::SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><rect id="target" width="4" height="4"/></svg>)",
      warnings);
  ASSERT_THAT(parsed, NoParseError());
  auto document = std::move(parsed).result();
  auto target = document.querySelector("#target");
  ASSERT_THAT(target, Optional(::testing::_));
  auto& registry = document.registry();
  const Entity root = target->entityHandle().entity();
  registry.ctx().emplace<ExternalUseStyleContext>(root, inherited, PaintServer::None(),
                                                  PaintServer::None(), nullptr);
  ExternalUseStyleRetention::PrepareOccurrence(registry);
  ExternalUseStyleRetention::FinishOccurrence(registry);
  registry.ctx().erase<ExternalUseStyleContext>();

  const std::weak_ptr<const ComputedStyle> firstRecord = first;
  const std::weak_ptr<const ComputedStyle> savedRecord = inherited;
  first.reset();
  inherited.reset();
  EXPECT_THAT(firstRecord.expired(), ::testing::IsTrue());
  EXPECT_THAT(savedRecord.expired(), ::testing::IsFalse());
  EXPECT_EQ(account->retainedBytes(), bytes);
  EXPECT_EQ(family->totalRetainedBytes(), bytes + 53);

  ExternalUseStyleRetention::RestoreStandalone(registry);
  EXPECT_THAT(savedRecord.expired(), ::testing::IsTrue());
  EXPECT_EQ(account->retainedBytes(), 0u);
  EXPECT_EQ(family->totalRetainedBytes(), 53u);
  family->release(Kind::ParsedPayload, 53);
}

}  // namespace
}  // namespace donner::svg::components
