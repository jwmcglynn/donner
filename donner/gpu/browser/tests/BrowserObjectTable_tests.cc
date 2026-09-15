#include "donner/gpu/browser/BrowserObjectTable.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner::gpu::browser {

using testing::ElementsAre;
using testing::Eq;
using testing::Optional;
using testing::Pair;

TEST(BrowserObjectTable, MintsDistinctIdentifiersAcrossKinds) {
  BrowserObjectTable table;

  EXPECT_THAT(table.insert(BrowserObjectKind::Buffer, 0).id, 1u);
  EXPECT_THAT(table.insert(BrowserObjectKind::Texture, 0).id, 2u);
  EXPECT_THAT(table.insert(BrowserObjectKind::Buffer, 1).id, 3u);

  // One identifier space shared by every kind is what lets the browser side treat a kind check as
  // a second check rather than a restatement of the first.
  EXPECT_THAT(table.find(BrowserObjectKind::Buffer, 0), Optional(Eq(1u)));
  EXPECT_THAT(table.find(BrowserObjectKind::Texture, 0), Optional(Eq(2u)));
  EXPECT_THAT(table.liveCount(), 3u);
}

TEST(BrowserObjectTable, FindsNothingForAnEmptySlot) {
  BrowserObjectTable table;
  EXPECT_THAT(table.find(BrowserObjectKind::Buffer, 0), Eq(std::nullopt));

  table.insert(BrowserObjectKind::Buffer, 4);
  EXPECT_THAT(table.find(BrowserObjectKind::Buffer, 3), Eq(std::nullopt));
  EXPECT_THAT(table.find(BrowserObjectKind::Texture, 4), Eq(std::nullopt));
}

TEST(BrowserObjectTable, NeverReissuesAnIdentifierAfterItIsRemoved) {
  BrowserObjectTable table;

  const BrowserObjectId first = table.insert(BrowserObjectKind::Buffer, 0).id;
  EXPECT_THAT(table.remove(BrowserObjectKind::Buffer, 0), Optional(Eq(first)));
  EXPECT_THAT(table.remove(BrowserObjectKind::Buffer, 0), Eq(std::nullopt));

  // The runtime recycles slot 0; the identifier must not be recycled with it, or a reference the
  // browser side still holds would address the new occupant.
  const BrowserObjectId second = table.insert(BrowserObjectKind::Buffer, 0).id;
  EXPECT_THAT(second, Eq(first + 1));
}

TEST(BrowserObjectTable, ReportsTheIdentifierASlotStillHeld) {
  BrowserObjectTable table;

  const BrowserObjectId first = table.insert(BrowserObjectKind::Surface, 2).id;
  const BrowserObjectInsertion second = table.insert(BrowserObjectKind::Surface, 2);

  // A surface has no destruction hook, so its slot can be reused while the table still names the
  // browser object it held. Reporting that identifier is what lets the caller release it.
  EXPECT_THAT(second.displaced, Eq(first));
  EXPECT_THAT(second.id, Eq(first + 1));
  EXPECT_THAT(table.find(BrowserObjectKind::Surface, 2), Optional(Eq(second.id)));
  EXPECT_THAT(table.liveCount(), 1u);
}

TEST(BrowserObjectTable, DisplacesNothingWhenTheSlotWasReleasedFirst) {
  BrowserObjectTable table;

  table.insert(BrowserObjectKind::Buffer, 1);
  table.remove(BrowserObjectKind::Buffer, 1);
  EXPECT_THAT(table.insert(BrowserObjectKind::Buffer, 1).displaced, Eq(kNoBrowserObject));
}

TEST(BrowserObjectTable, TakeAllEmptiesTheTableInDeterministicOrder) {
  BrowserObjectTable table;

  const BrowserObjectId buffer = table.insert(BrowserObjectKind::Buffer, 1).id;
  const BrowserObjectId texture = table.insert(BrowserObjectKind::Texture, 0).id;
  const BrowserObjectId sampler = table.insert(BrowserObjectKind::Sampler, 3).id;

  EXPECT_THAT(table.takeAll(), ElementsAre(Pair(BrowserObjectKind::Buffer, buffer),
                                           Pair(BrowserObjectKind::Texture, texture),
                                           Pair(BrowserObjectKind::Sampler, sampler)));
  EXPECT_THAT(table.liveCount(), 0u);
  EXPECT_THAT(table.takeAll(), ElementsAre());
}

TEST(BrowserObjectTable, NamesEveryKindForDiagnostics) {
  EXPECT_THAT(BrowserObjectKindName(BrowserObjectKind::Buffer), "buffer");
  EXPECT_THAT(BrowserObjectKindName(BrowserObjectKind::BufferMapping), "buffer mapping");
  EXPECT_THAT(BrowserObjectKindName(static_cast<BrowserObjectKind>(200)), "unknown object");
}

}  // namespace donner::gpu::browser
