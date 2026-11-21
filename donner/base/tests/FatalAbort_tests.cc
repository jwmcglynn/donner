#include "donner/base/FatalAbort.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <array>
#include <csignal>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace donner {
namespace {

using testing::ElementsAre;
using testing::Eq;
using testing::StrEq;

std::mutex gPublishedMutex;
std::vector<std::string> gPublished;

void RecordPublishedRole(const char* role) {
  const std::lock_guard lock(gPublishedMutex);
  gPublished.emplace_back(role != nullptr ? role : "<cleared>");
}

std::string Format(std::span<char> buffer, const char* site, const char* role,
                   std::initializer_list<FatalAbortValue> values) {
  const std::size_t length = FormatFatalAbortMessage(
      buffer, site, role, std::span<const FatalAbortValue>(values.begin(), values.size()));
  EXPECT_THAT(length, Eq(std::string(buffer.data()).size()));
  return buffer.data();
}

TEST(FatalAbortTest, FormatsSiteRoleAndValues) {
  std::array<char, 256> buffer{};
  EXPECT_THAT(Format(buffer, "owner.stopped", "gpu-owner", {{"stopped", 1}, {"delta", -3}}),
              StrEq("[donner-fatal] Aborted at site=owner.stopped thread=gpu-owner stopped=1 "
                    "delta=-3"));
}

TEST(FatalAbortTest, TruncatesToTheBufferAndStaysTerminated) {
  std::array<char, 40> buffer{};
  EXPECT_THAT(Format(buffer, "a.site.with.a.long.name", "pthread", {{"value", 123456789}}),
              StrEq("[donner-fatal] Aborted at site=a.site.w"));
}

TEST(FatalAbortTest, ThreadRoleDefaultsUntilSetPerThread) {
  std::string defaultRole;
  std::string setRole;
  std::thread([&] {
    defaultRole = FatalAbortThreadRole();
    SetFatalAbortThreadRole("render-worker");
    setRole = FatalAbortThreadRole();
  }).join();
  EXPECT_THAT(defaultRole, StrEq("native-thread"));
  EXPECT_THAT(setRole, StrEq("render-worker"));
  EXPECT_THAT(FatalAbortThreadRole(), StrEq("native-thread"));
}

TEST(FatalAbortTest, PublishedRoleIsClearedWhenItsThreadExits) {
  gPublished.clear();
  SetFatalAbortRolePublisherForTesting(&RecordPublishedRole);
  std::thread([] {
    SetFatalAbortThreadRole("application-pthread");
    SetFatalAbortThreadRole("gpu-owner");
  }).join();
  std::thread([] {}).join();
  SetFatalAbortRolePublisherForTesting(nullptr);
  EXPECT_THAT(gPublished, ElementsAre("application-pthread", "gpu-owner", "<cleared>"));
}

TEST(FatalAbortTest, EmitsTheNamedSiteBeforeAborting) {
  EXPECT_EXIT(
      {
        SetFatalAbortThreadRole("application-pthread");
        FatalAbort("test.site", {{"first", 1}, {"second", -2}});
      },
      testing::KilledBySignal(SIGABRT),
      "donner-fatal. Aborted at site=test.site thread=application-pthread first=1 second=-2");
}

}  // namespace
}  // namespace donner
