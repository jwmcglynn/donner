/// @file
/// Tests for \ref donner::gpu::Result and \ref donner::gpu::Status.

#include "donner/gpu/GpuResult.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "donner/gpu/tests/GpuTestUtils.h"

using testing::Eq;
using testing::HasSubstr;

namespace donner::gpu {

TEST(GpuResultTests, HoldsValue) {
  const Result<int> result(42);
  EXPECT_THAT(result, HasResult());
  EXPECT_THAT(result.result(), Eq(42));
  EXPECT_FALSE(result.hasError());
}

TEST(GpuResultTests, HoldsError) {
  const Result<int> result(GpuError{GpuErrorType::OutOfBounds, "offset 4 exceeds size 2"});
  EXPECT_THAT(result, IsGpuError(GpuErrorType::OutOfBounds));
  EXPECT_THAT(result,
              IsGpuErrorWithMessage(GpuErrorType::OutOfBounds, HasSubstr("offset 4 exceeds")));
  EXPECT_FALSE(result.hasResult());
}

TEST(GpuResultTests, SupportsMoveOnlyValues) {
  Result<std::unique_ptr<int>> result(std::make_unique<int>(7));
  ASSERT_THAT(result, HasResult());

  const std::unique_ptr<int> value = std::move(result).result();
  EXPECT_THAT(*value, Eq(7));
}

TEST(GpuResultTests, StatusOkAndError) {
  EXPECT_THAT(OkStatus(), IsOk());

  const Status errorStatus(GpuError{GpuErrorType::InvalidState, "no render pass is active"});
  EXPECT_THAT(errorStatus, IsGpuError(GpuErrorType::InvalidState));
}

TEST(GpuResultTests, PrintToIsDiagnosable) {
  const Result<int> value(3);
  EXPECT_THAT(testing::PrintToString(value), HasSubstr("3"));

  const Result<int> error(GpuError{GpuErrorType::LimitExceeded, "too many bindings"});
  EXPECT_THAT(testing::PrintToString(error), HasSubstr("LimitExceeded: too many bindings"));

  EXPECT_THAT(testing::PrintToString(OkStatus()), HasSubstr("ok"));
}

TEST(GpuErrorTests, ToStringAndEquality) {
  const GpuError error{GpuErrorType::DeviceMismatch, "handle belongs to device 1"};
  EXPECT_THAT(error.toString(), Eq("DeviceMismatch: handle belongs to device 1"));
  EXPECT_THAT(error, Eq(GpuError{GpuErrorType::DeviceMismatch, "handle belongs to device 1"}));
  EXPECT_THAT(GpuError::TypeToString(GpuErrorType::Unsupported), Eq("Unsupported"));
}

}  // namespace donner::gpu
