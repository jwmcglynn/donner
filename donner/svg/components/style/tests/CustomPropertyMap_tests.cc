#include "donner/svg/components/style/CustomPropertyMap.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <string>
#include <type_traits>

#include "donner/svg/components/style/ComputedStyleComponent.h"

namespace donner::svg::components {
namespace {

using ::testing::Eq;
using ::testing::NotNull;
using ::testing::Optional;
using Action = CustomPropertyMap::Action;
using Change = CustomPropertyMap::Change;
using Kind = DocumentResourceFamilyBudget::Kind;

static_assert(std::is_same_v<decltype(std::declval<const TokenSequence&>().tokens()),
                             std::span<const css::ComponentValue>>);
static_assert(std::is_same_v<decltype(std::declval<const CustomPropertyMap&>().entries()),
                             std::span<const CustomPropertyMap::Entry>>);

std::vector<css::ComponentValue> TextTokens(std::string_view text) {
  std::vector<css::ComponentValue> result;
  result.emplace_back(css::Token(css::Token::String(RcString(text)), 17));
  return result;
}

TEST(CustomPropertyMapTest, OwnsNestedTokensAndTheirStringStorage) {
  auto account = std::make_shared<RetainedStyleBytes>(1 << 20);
  const std::string text(4096, 'q');
  std::shared_ptr<const TokenSequence> owned;
  {
    css::SimpleBlock block(css::Token::indexOf<css::Token::SquareBracket>(),
                           FileOffset::Offset(11));
    block.values = TextTokens(text);
    const char* sourceText =
        block.values.front().get<css::Token>().get<css::Token::String>().value.data();
    css::Function function(RcString("combine"), FileOffset::Offset(3));
    function.values.emplace_back(std::move(block));
    std::vector<css::ComponentValue> authored;
    authored.emplace_back(std::move(function));

    owned = TokenSequence::Create(account, authored);
    ASSERT_THAT(owned, NotNull());
    const auto& copied = owned->tokens().front().get<css::Function>();
    const auto& leaf = copied.values.front().get<css::SimpleBlock>().values.front();
    EXPECT_THAT(leaf.get<css::Token>().get<css::Token::String>().value.data(),
                ::testing::Ne(sourceText));
    EXPECT_THAT(leaf.sourceOffset(), Eq(FileOffset::Offset(17)));
    EXPECT_THAT(owned->tokens(), ::testing::ElementsAreArray(authored));
  }

  const auto& tokens = owned->tokens().front().get<css::Function>().values;
  ASSERT_THAT(tokens, ::testing::SizeIs(1));
  const auto& leaf = tokens.front().get<css::SimpleBlock>().values.front();
  EXPECT_THAT(leaf.get<css::Token>().get<css::Token::String>().value, Eq(RcString(text)));
  EXPECT_GT(account->retainedBytes(), text.size());
  owned.reset();
  EXPECT_EQ(account->retainedBytes(), 0u);
}

TEST(CustomPropertyMapTest, InheritsOverlaysAndDistinguishesEmptyFromMissing) {
  auto account = std::make_shared<RetainedStyleBytes>(1 << 20);
  const auto red = TokenSequence::Create(account, TextTokens("red"));
  const auto empty = TokenSequence::Create(account, {});
  ASSERT_THAT(red, NotNull());
  ASSERT_THAT(empty, NotNull());
  const auto parent =
      CustomPropertyMap::Overlay(account, nullptr,
                                 std::array{Change{RcString("--keep"), Action::Set, red},
                                            Change{RcString("--initial"), Action::Set, red},
                                            Change{RcString("--invalid"), Action::Set, red},
                                            Change{RcString("--unset"), Action::Set, red}});
  ASSERT_THAT(parent, Optional(NotNull()));

  const std::array unchanged{Change{RcString("--keep"), Action::Set, empty},
                             Change{RcString("--keep"), Action::Inherit, nullptr},
                             Change{RcString("--unset"), Action::Unset, nullptr}};
  const auto before = account->retainedBytes();
  EXPECT_THAT(CustomPropertyMap::Overlay(account, *parent, unchanged), Optional(Eq(*parent)));
  EXPECT_EQ(account->retainedBytes(), before);

  const auto child =
      CustomPropertyMap::Overlay(account, *parent,
                                 std::array{Change{RcString("--initial"), Action::Initial, nullptr},
                                            Change{RcString("--invalid"), Action::Set, nullptr},
                                            Change{RcString("--Keep"), Action::Set, empty},
                                            Change{RcString("--missing"), Action::Unset, nullptr}});
  ASSERT_THAT(child, Optional(NotNull()));
  const auto entries = (*child)->entries();
  ASSERT_THAT(entries, ::testing::SizeIs(3));
  EXPECT_THAT(entries[0].name, Eq(RcString("--Keep")));
  EXPECT_THAT(entries[1].name, Eq(RcString("--keep")));
  EXPECT_THAT(entries[2].name, Eq(RcString("--unset")));
  const auto* emptyValue = (*child)->find("--Keep");
  ASSERT_THAT(emptyValue, Eq(empty.get()));
  EXPECT_THAT(emptyValue->tokens(), ::testing::IsEmpty());
  EXPECT_THAT((*child)->find("--keep"), Eq(red.get()));
  for (std::string_view name : {"--initial", "--invalid", "--missing", "--KEEP"}) {
    EXPECT_THAT((*child)->find(name), ::testing::IsNull()) << name;
  }
}

TEST(CustomPropertyMapTest, RefusesDocumentAndFamilyChargesWithoutLeakingBytes) {
  const auto tokens = TextTokens(std::string(2048, 'x'));
  const auto owner = std::make_shared<RetainedStyleBytes>(1 << 20);
  auto measured = TokenSequence::Create(owner, tokens);
  ASSERT_THAT(measured, NotNull());
  const auto cost = owner->retainedBytes();

  const auto local = std::make_shared<RetainedStyleBytes>(cost - 1);
  EXPECT_THAT(TokenSequence::Create(local, tokens), ::testing::IsNull());
  EXPECT_EQ(local->retainedBytes(), 0u);

  DocumentResourceFamilyBudget::Limits limits;
  limits.computedStyleBytes = cost - 1;
  const auto family = std::make_shared<DocumentResourceFamilyBudget>(limits);
  const auto child = std::make_shared<RetainedStyleBytes>(1 << 20, family);
  EXPECT_THAT(TokenSequence::Create(child, tokens), ::testing::IsNull());
  EXPECT_EQ(child->retainedBytes(), 0u);
  EXPECT_EQ(family->totalRetainedBytes(), 0u);
  EXPECT_EQ(family->securityStats().rejectedReservations, 1u);

  const auto noMapSpace = std::make_shared<RetainedStyleBytes>(0);
  const std::array changes{Change{RcString("--x"), Action::Set, measured}};
  EXPECT_THAT(CustomPropertyMap::Overlay(noMapSpace, nullptr, changes), Eq(std::nullopt));
  EXPECT_EQ(noMapSpace->retainedBytes(), 0u);
  EXPECT_EQ(owner->retainedBytes(), cost);
}

TEST(CustomPropertyMapTest, RefusesOversizedTokenTextBeforeCharging) {
  const auto account = std::make_shared<RetainedStyleBytes>(1 << 24);
  const std::string tooLong(TokenSequence::kMaximumTextBytes + 1, 'x');
  EXPECT_THAT(TokenSequence::Create(account, TextTokens(tooLong)), ::testing::IsNull());
  EXPECT_EQ(account->retainedBytes(), 0u);
}

TEST(CustomPropertyMapTest, OverlaidMapsKeepOriginalTokenChargeInEitherReleaseOrder) {
  for (bool releaseFirstMapFirst : {false, true}) {
    SCOPED_TRACE(releaseFirstMapFirst);
    const auto family = std::make_shared<DocumentResourceFamilyBudget>();
    ASSERT_TRUE(family->reserve(Kind::ParsedPayload, 47));
    const auto firstOwner = std::make_shared<RetainedStyleBytes>(1 << 20, family);
    const auto overlayOwner = std::make_shared<RetainedStyleBytes>(1 << 20, family);
    std::size_t tokenCharge;
    std::array<std::shared_ptr<const CustomPropertyMap>, 2> overlays;
    {
      auto payload = TokenSequence::Create(firstOwner, TextTokens(std::string(4096, 'z')));
      ASSERT_THAT(payload, NotNull());
      tokenCharge = firstOwner->retainedBytes();
      auto parent = CustomPropertyMap::Overlay(
          firstOwner, nullptr, std::array{Change{RcString("--shared"), Action::Set, payload}});
      ASSERT_THAT(parent, Optional(NotNull()));
      for (std::size_t i = 0; i < overlays.size(); ++i) {
        auto overlay = CustomPropertyMap::Overlay(
            overlayOwner, *parent,
            std::array{Change{RcString(i == 0 ? "--first" : "--second"), Action::Set, payload}});
        ASSERT_THAT(overlay, Optional(NotNull()));
        overlays[i] = std::move(*overlay);
      }
    }

    EXPECT_EQ(firstOwner->retainedBytes(), tokenCharge);
    const std::size_t firstRelease = releaseFirstMapFirst ? 0 : 1;
    overlays[firstRelease].reset();
    EXPECT_EQ(firstOwner->retainedBytes(), tokenCharge);
    EXPECT_GT(overlayOwner->retainedBytes(), 0u);
    EXPECT_EQ(family->totalRetainedBytes(), tokenCharge + overlayOwner->retainedBytes() + 47);
    overlays[1 - firstRelease].reset();
    EXPECT_EQ(firstOwner->retainedBytes(), 0u);
    EXPECT_EQ(overlayOwner->retainedBytes(), 0u);
    EXPECT_EQ(family->totalRetainedBytes(), 47u);
    family->release(Kind::ParsedPayload, 47);
  }
}

}  // namespace
}  // namespace donner::svg::components
