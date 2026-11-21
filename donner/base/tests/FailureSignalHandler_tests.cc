#include "donner/base/FailureSignalHandler.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace donner {
namespace {

using Function = void (*)();

// Volatile so the compiler cannot prove the pointers null and fold the faults away. The faulting
// functions opt out of UBSan so its null checks in the sanitizer lanes do not report first.
volatile Function gNullFunction = nullptr;
int* volatile gNullData = nullptr;

__attribute__((noinline, no_sanitize("undefined"))) void CallThroughNullFunctionPointer() {
  gNullFunction();
}

__attribute__((noinline, no_sanitize("undefined"))) void WriteThroughNullPointer() {
  *gNullData = 1;
}

class FailureSignalHandlerDeathTest : public testing::Test {
protected:
  void SetUp() override { GTEST_FLAG_SET(death_test_style, "threadsafe"); }
};

// A call through a null function pointer stops the unwinder at program counter 0, so the stack
// trace alone has no frames. The report still states the fault and the interrupted program counter.
TEST_F(FailureSignalHandlerDeathTest, NullFunctionCallReportsFaultContext) {
  EXPECT_DEATH(
      {
        InstallFailureSignalHandler();
        CallThroughNullFunctionPointer();
      },
      testing::ContainsRegex("SIGSEGV received.*Fault address: 0x0\n.*Interrupted PC:  0x0\n"));
}

TEST_F(FailureSignalHandlerDeathTest, NullDataWriteReportsFaultAddress) {
  EXPECT_DEATH(
      {
        InstallFailureSignalHandler();
        WriteThroughNullPointer();
      },
      testing::ContainsRegex(
          "SIGSEGV received.*Fault address: 0x0\n.*Interrupted PC:  0x[1-9a-f]"));
}

}  // namespace
}  // namespace donner
