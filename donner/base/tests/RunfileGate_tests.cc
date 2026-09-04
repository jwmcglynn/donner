#include "donner/base/tests/RunfileGate.h"

#include <gmock/gmock.h>
#include <gtest/gtest-spi.h>
#include <gtest/gtest.h>

#include <filesystem>

namespace donner::tests {
namespace {

using testing::HasSubstr;

constexpr const char* kPresentRunfile = "donner/base/tests/testdata/test.txt";
constexpr const char* kAbsentRunfile = "donner/base/tests/testdata/no_such_entry.txt";

/// Free function so EXPECT_FATAL_FAILURE, which cannot see a caller's locals, can drive the gate.
void RequireAbsentRunfile() {
  const RequiredRunfile runfile = ReadRequiredRunfile(kAbsentRunfile);
  DONNER_REQUIRE_RUNFILE(runfile);
}

}  // namespace

TEST(RunfileGate, ResolvesAFileTheTargetCarriesInData) {
  const RequiredRunfile runfile = ResolveRequiredRunfile(kPresentRunfile);

  ASSERT_TRUE(runfile.ok()) << runfile.error;
  EXPECT_EQ(runfile.requestedPath, kPresentRunfile);
  EXPECT_TRUE(std::filesystem::exists(runfile.path)) << runfile.path;
}

TEST(RunfileGate, ReadsAFileTheTargetCarriesInData) {
  const RequiredRunfile runfile = ReadRequiredRunfile(kPresentRunfile);

  ASSERT_TRUE(runfile.ok()) << runfile.error;
  EXPECT_FALSE(runfile.contents.empty());
}

TEST(RunfileGate, ReportsAnAbsentEntryRatherThanResolvingIt) {
  const RequiredRunfile runfile = ReadRequiredRunfile(kAbsentRunfile);

  EXPECT_FALSE(runfile.ok());
  EXPECT_TRUE(runfile.path.empty());
  EXPECT_TRUE(runfile.contents.empty());
  EXPECT_THAT(runfile.error, HasSubstr(kAbsentRunfile));
}

TEST(RunfileGate, LabelsTheTargetBazelIsRunning) {
  EXPECT_THAT(TestTargetLabel(), HasSubstr("//donner/base"));
}

TEST(RunfileGate, MessageNamesThePathTheTargetAndEveryResolutionAttempted) {
  const std::string message = MissingRunfileMessage(
      "some/data/file.svg", "//some:target", "/runfiles/some/data/file.svg", "/cwd/file.svg");

  EXPECT_THAT(message, HasSubstr("some/data/file.svg"));
  EXPECT_THAT(message, HasSubstr("`data` attribute of //some:target"));
  EXPECT_THAT(message, HasSubstr("runfiles manifest -> /runfiles/some/data/file.svg"));
  EXPECT_THAT(message, HasSubstr("test working directory -> /cwd/file.svg"));
}

TEST(RunfileGate, MessageSaysSoWhenTheManifestMappedNothing) {
  const std::string message =
      MissingRunfileMessage("some/data/file.svg", "//some:target", "", "/cwd/file.svg");

  EXPECT_THAT(message, HasSubstr("runfiles manifest -> (unmapped)"));
}

TEST(RunfileGate, GateLetsAResolvedRunfileThrough) {
  const RequiredRunfile runfile = ReadRequiredRunfile(kPresentRunfile);
  DONNER_REQUIRE_RUNFILE(runfile);

  EXPECT_FALSE(runfile.contents.empty());
}

TEST(RunfileGate, GateFailsRatherThanSkipsOnAnAbsentEntry) {
  EXPECT_FATAL_FAILURE(RequireAbsentRunfile(), "is not in the runfiles tree");
}

}  // namespace donner::tests
