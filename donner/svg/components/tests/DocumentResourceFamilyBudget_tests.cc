#include "donner/svg/components/DocumentResourceFamilyBudget.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <limits>

namespace donner::svg::components {
namespace {

using testing::ElementsAre;
using testing::IsFalse;
using testing::IsTrue;

DocumentResourceFamilyBudget::Limits MakeLimits(std::size_t perKindBytes, std::size_t totalBytes) {
  return DocumentResourceFamilyBudget::Limits{
      .parsedPayloadBytes = perKindBytes,
      .geometryBytes = perKindBytes,
      .computedFilterBytes = perKindBytes,
      .computedStyleBytes = perKindBytes,
      .maximumTotalRetainedBytes = totalBytes,
      .decodedImageBytes = perKindBytes,
  };
}

TEST(DocumentResourceFamilyBudgetTest, DefaultPreservesTwoFullCategoryReservations) {
  DocumentResourceFamilyBudget budget;

  EXPECT_EQ(budget.limits().maximumTotalRetainedBytes,
            DocumentResourceFamilyBudget::kDefaultMaximumTotalRetainedBytes);
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::ParsedPayload, 64 * 1024 * 1024),
              IsTrue());
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::Geometry, 64 * 1024 * 1024),
              IsTrue());
  EXPECT_EQ(budget.totalRetainedBytes(), 128 * 1024 * 1024);
}

TEST(DocumentResourceFamilyBudgetTest, EnforcesAggregateBoundaryAcrossAllKinds) {
  DocumentResourceFamilyBudget budget(MakeLimits(64, 95));

  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::ParsedPayload, 19), IsTrue());
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::Geometry, 19), IsTrue());
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::ComputedFilter, 19), IsTrue());
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::ComputedStyle, 19), IsTrue());
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::DecodedImage, 19), IsTrue());
  EXPECT_EQ(budget.totalRetainedBytes(), 95u);

  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::ParsedPayload, 1), IsFalse());
  EXPECT_THAT(budget.securityStats().retainedBytes, ElementsAre(19, 19, 19, 19, 19));
  EXPECT_EQ(budget.totalRetainedBytes(), 95u);
  EXPECT_EQ(budget.securityStats().rejectedReservations, 1u);
  EXPECT_THAT(budget.securityStats().rejected, IsTrue());
}

TEST(DocumentResourceFamilyBudgetTest, AvailableBytesHonorsKindTotalAndRejectionLimits) {
  using Kind = DocumentResourceFamilyBudget::Kind;
  DocumentResourceFamilyBudget budget(MakeLimits(64, 76));
  EXPECT_EQ(budget.availableBytes(Kind::DecodedImage), 64u);
  ASSERT_TRUE(budget.reserve(Kind::Geometry, 20));
  EXPECT_EQ(budget.availableBytes(Kind::DecodedImage), 56u);
  ASSERT_TRUE(budget.reserve(Kind::DecodedImage, 19));
  EXPECT_EQ(budget.availableBytes(Kind::DecodedImage), 37u);
  budget.release(Kind::Geometry, 10);
  EXPECT_EQ(budget.availableBytes(Kind::DecodedImage), 45u);
  EXPECT_EQ(budget.availableBytes(Kind::Count), 0u);
  EXPECT_FALSE(budget.reserve(Kind::DecodedImage, 46));
  EXPECT_EQ(budget.availableBytes(Kind::ComputedStyle), 0u);
}

TEST(DocumentResourceFamilyBudgetTest, RejectionIsAtomicAndLatches) {
  DocumentResourceFamilyBudget budget(MakeLimits(100, 50));
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::ParsedPayload, 30), IsTrue());

  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::Geometry, 21), IsFalse());
  EXPECT_THAT(budget.securityStats().retainedBytes, ElementsAre(30, 0, 0, 0, 0));
  EXPECT_EQ(budget.totalRetainedBytes(), 30u);

  budget.release(DocumentResourceFamilyBudget::Kind::ParsedPayload, 30);
  EXPECT_EQ(budget.totalRetainedBytes(), 0u);
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::Geometry, 1), IsFalse());
  EXPECT_EQ(budget.securityStats().retainedBytes[1], 0u);
  EXPECT_EQ(budget.securityStats().rejectedReservations, 2u);
}

TEST(DocumentResourceFamilyBudgetTest, RejectsPerKindBoundaryWithoutChargingTotal) {
  DocumentResourceFamilyBudget budget(MakeLimits(10, 100));
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::ComputedFilter, 10), IsTrue());

  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::ComputedFilter, 1), IsFalse());
  EXPECT_EQ(budget.retainedBytes(DocumentResourceFamilyBudget::Kind::ComputedFilter), 10u);
  EXPECT_EQ(budget.totalRetainedBytes(), 10u);
}

TEST(DocumentResourceFamilyBudgetTest, TextPathWorkEpochResetsOnlyAtOutermostBoundary) {
  auto limits = MakeLimits(100, 100);
  limits.textPathMeasurementWorkUnits = 4;
  DocumentResourceFamilyBudget budget(limits);

  budget.beginTextPathMeasurementWorkEpoch();
  EXPECT_TRUE(budget.reserveTextPathMeasurementWork(3));
  budget.beginTextPathMeasurementWorkEpoch();
  EXPECT_EQ(budget.availableTextPathMeasurementWork(), 1u);
  budget.endTextPathMeasurementWorkEpoch();
  EXPECT_EQ(budget.availableTextPathMeasurementWork(), 1u);
  budget.endTextPathMeasurementWorkEpoch();

  budget.beginTextPathMeasurementWorkEpoch();
  EXPECT_EQ(budget.availableTextPathMeasurementWork(), 4u);
  EXPECT_FALSE(budget.securityStats().textPathMeasurementWorkRejected);
  budget.endTextPathMeasurementWorkEpoch();
}

TEST(DocumentResourceFamilyBudgetTest, AggregateAdditionCannotOverflow) {
  constexpr std::size_t kMaximum = std::numeric_limits<std::size_t>::max();
  DocumentResourceFamilyBudget budget(MakeLimits(kMaximum, kMaximum));
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::ParsedPayload, kMaximum - 1),
              IsTrue());

  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::Geometry, 2), IsFalse());
  EXPECT_EQ(budget.retainedBytes(DocumentResourceFamilyBudget::Kind::ParsedPayload), kMaximum - 1);
  EXPECT_EQ(budget.retainedBytes(DocumentResourceFamilyBudget::Kind::Geometry), 0u);
  EXPECT_EQ(budget.totalRetainedBytes(), kMaximum - 1);
}

TEST(DocumentResourceFamilyBudgetTest, ReleaseUsesOnlyBytesRetainedByTheKind) {
  DocumentResourceFamilyBudget budget(MakeLimits(100, 100));
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::ParsedPayload, 20), IsTrue());
  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::Geometry, 30), IsTrue());

  budget.release(DocumentResourceFamilyBudget::Kind::ParsedPayload,
                 std::numeric_limits<std::size_t>::max());
  EXPECT_EQ(budget.retainedBytes(DocumentResourceFamilyBudget::Kind::ParsedPayload), 0u);
  EXPECT_EQ(budget.retainedBytes(DocumentResourceFamilyBudget::Kind::Geometry), 30u);
  EXPECT_EQ(budget.totalRetainedBytes(), 30u);

  budget.release(DocumentResourceFamilyBudget::Kind::Geometry, 10);
  EXPECT_EQ(budget.retainedBytes(DocumentResourceFamilyBudget::Kind::Geometry), 20u);
  EXPECT_EQ(budget.totalRetainedBytes(), 20u);
}

TEST(DocumentResourceFamilyBudgetTest, InvalidKindFailsClosedWithoutOutOfBoundsAccess) {
  DocumentResourceFamilyBudget budget(MakeLimits(100, 100));

  EXPECT_THAT(budget.reserve(DocumentResourceFamilyBudget::Kind::Count, 1), IsFalse());
  EXPECT_EQ(budget.totalRetainedBytes(), 0u);
  EXPECT_THAT(budget.securityStats().rejected, IsTrue());
  EXPECT_EQ(budget.retainedBytes(DocumentResourceFamilyBudget::Kind::Count), 0u);
  budget.release(DocumentResourceFamilyBudget::Kind::Count, 1);
  EXPECT_EQ(budget.totalRetainedBytes(), 0u);
}

}  // namespace
}  // namespace donner::svg::components
