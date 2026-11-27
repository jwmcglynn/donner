#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "donner/base/ParseWarningSink.h"
#include "donner/base/tests/ParseResultTestUtils.h"
#include "donner/svg/SVGStyleQuery.h"
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

TEST(CustomPropertyRetentionTest, BatchedProvenanceKeepsLocalWinnerAtExternalUseRoot) {
  using testing::Optional;
  ParseWarningSink warnings;
  auto parsedHost = parser::SVGParser::ParseSVG(
      R"(<svg xmlns="http://www.w3.org/2000/svg"><use id="host" style="--host: red"/></svg>)",
      warnings);
  auto parsedExternal = parser::SVGParser::ParseSVG(
      R"svg(<svg xmlns="http://www.w3.org/2000/svg"><g id="occurrence">
        <rect id="leaf" style="--local: blue; fill: var(--host)"/>
      </g></svg>)svg",
      warnings);
  ASSERT_THAT(parsedHost, NoParseError());
  ASSERT_THAT(parsedExternal, NoParseError());
  auto host = std::move(parsedHost).result();
  auto external = std::move(parsedExternal).result();
  const auto use = host.querySelector("#host");
  const auto occurrence = external.querySelector("#occurrence");
  const auto leaf = external.querySelector("#leaf");
  ASSERT_THAT(use, Optional(testing::_));
  ASSERT_THAT(occurrence, Optional(testing::_));
  ASSERT_THAT(leaf, Optional(testing::_));

  std::ignore = use->getComputedStyle();
  const auto inherited = use->entityHandle().get<ComputedStyleComponent>().style;
  ASSERT_THAT(inherited, testing::NotNull());
  auto& registry = external.registry();
  registry.ctx().emplace<ExternalUseStyleContext>(occurrence->entityHandle().entity(), inherited,
                                                  PaintServer::None(), PaintServer::None(),
                                                  nullptr);
  ExternalUseStyleRetention::PrepareOccurrence(registry);
  EXPECT_THAT(leaf->getComputedStyle().fill.get(),
              Optional(PaintServer::Solid(css::Color(css::RGBA(255, 0, 0, 255)))));

  const auto offset = external.source().find("--local:");
  ASSERT_NE(offset, std::string_view::npos);
  const auto localRange = Optional(
      testing::AllOf(testing::Field("start", &SourceRange::start, FileOffset::Offset(offset)),
                     testing::Field("end", &SourceRange::end, FileOffset::Offset(offset + 7))));
  EXPECT_THAT(FindCustomPropertyDeclaration(*leaf, "--local"), localRange);
  EXPECT_THAT(FindCustomPropertyDeclaration(*leaf, "--host"), testing::Eq(std::nullopt));

  const std::array<std::string_view, 2> names{"--local", "--host"};
  constexpr std::size_t kWorkLimit = 4096;
  const auto batch = FindCustomPropertyDeclarations(*leaf, names, kWorkLimit);
  EXPECT_TRUE(batch.complete);
  EXPECT_FALSE(batch.budgetExceeded);
  EXPECT_THAT(batch.declarations, testing::ElementsAre(localRange, std::nullopt));
  ASSERT_GT(batch.work, 0u);
  const auto refused = FindCustomPropertyDeclarations(*leaf, names, batch.work - 1);
  EXPECT_FALSE(refused.complete);
  EXPECT_TRUE(refused.budgetExceeded);
  EXPECT_THAT(refused.declarations, testing::ElementsAre(std::nullopt, std::nullopt));

  ExternalUseStyleRetention::FinishOccurrence(registry);
  registry.ctx().erase<ExternalUseStyleContext>();
}

}  // namespace
}  // namespace donner::svg::components
