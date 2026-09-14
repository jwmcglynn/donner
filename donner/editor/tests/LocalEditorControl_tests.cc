#include "donner/editor/LocalEditorControl.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <string>
#include <vector>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace donner::editor {
namespace {
using Json = nlohmann::json;
using testing::Eq;
using testing::HasSubstr;

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
class LocalEditorControlTest : public testing::Test {
protected:
  std::filesystem::path directory;
  std::string endpoint;
  LocalEditorControl control;
  std::mutex mutex;
  std::condition_variable condition;
  bool wake = false;

  void SetUp() override {
    char pattern[] = "/tmp/donner-control-test-XXXXXX";
    char* created = mkdtemp(pattern);
    ASSERT_THAT(created != nullptr, Eq(true));
    directory = created;
    endpoint = (directory / "editor.sock").string();
  }
  void TearDown() override {
    control.stop();
    std::error_code error;
    std::filesystem::remove_all(directory, error);
  }
  void start() {
    std::string error;
    ASSERT_THAT(control.start(
                    endpoint,
                    [this]() {
                      std::lock_guard lock(mutex);
                      wake = true;
                      condition.notify_all();
                    },
                    &error),
                Eq(true))
        << error;
  }
  bool awaitRequest() {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(2), [this]() { return wake; });
  }
  Json send(std::string bytes) {
    const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return Json();
    timeval timeout{.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
      close(fd);
      return Json();
    }
    bytes += '\n';
    std::size_t offset = 0;
    while (offset < bytes.size()) {
      const ssize_t count = ::send(fd, bytes.data() + offset, bytes.size() - offset, 0);
      if (count <= 0) {
        close(fd);
        return Json();
      }
      offset += static_cast<std::size_t>(count);
    }
    std::string response;
    char buffer[4096];
    while (response.size() < 1024 * 1024 && response.find('\n') == std::string::npos) {
      const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
      if (count <= 0) break;
      response.append(buffer, static_cast<std::size_t>(count));
    }
    close(fd);
    return Json::parse(response, nullptr, false);
  }
};

TEST_F(LocalEditorControlTest, DispatchesOnlyWhenTheUiProcessesTheRequest) {
  start();
  auto response =
      std::async(std::launch::async, [this]() { return send(R"({"id":7,"method":"ping"})"); });
  ASSERT_THAT(awaitRequest(), Eq(true));
  EXPECT_THAT(control.hasPending(), Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "live editor"}};
  }),
              Eq(true));
  EXPECT_THAT(response.get()["result"], Eq("live editor"));
  EXPECT_THAT(control.hasPending(), Eq(false));
}

TEST_F(LocalEditorControlTest, DeferredFeedbackWaitDoesNotBlockUiProcessing) {
  start();
  auto response =
      std::async(std::launch::async, [this]() { return send(R"({"id":7,"method":"ping"})"); });
  ASSERT_THAT(awaitRequest(), Eq(true));
  EXPECT_THAT(control.process([](const Json&) -> std::optional<Json> { return std::nullopt; }),
              Eq(false));
  EXPECT_THAT(control.hasPending(), Eq(true));
  EXPECT_THAT(control.process([](const Json&) -> std::optional<Json> {
    return Json{{"result", "comment added"}};
  }),
              Eq(true));
  EXPECT_THAT(response.get()["result"], Eq("comment added"));
}

TEST_F(LocalEditorControlTest, ShutdownCancelsPendingWorkAndRemovesOnlyOwnedSocket) {
  start();
  auto response =
      std::async(std::launch::async, [this]() { return send(R"({"id":7,"method":"ping"})"); });
  ASSERT_THAT(awaitRequest(), Eq(true));
  control.stop();
  EXPECT_THAT(std::filesystem::exists(endpoint), Eq(false));
  EXPECT_THAT(control.process([](const Json&) -> std::optional<Json> { return Json::object(); }),
              Eq(false));
  EXPECT_THAT(response.wait_for(std::chrono::seconds(2)), Eq(std::future_status::ready));
}

TEST_F(LocalEditorControlTest, RejectsPublicDirectoryAndAnExistingEndpoint) {
  ASSERT_THAT(chmod(directory.c_str(), 0755), Eq(0));
  std::string error;
  EXPECT_THAT(control.start(endpoint, []() {}, &error), Eq(false));
  EXPECT_THAT(error, HasSubstr("0700"));
  ASSERT_THAT(chmod(directory.c_str(), 0700), Eq(0));
  start();
  LocalEditorControl second;
  EXPECT_THAT(second.start(endpoint, []() {}, &error), Eq(false));
  EXPECT_THAT(std::filesystem::is_socket(endpoint), Eq(true));
}

TEST_F(LocalEditorControlTest, RejectsDeepJsonBeforeSchedulingUiWork) {
  start();
  const std::string payload =
      "{\"nested\":" + std::string(65, '[') + "0" + std::string(65, ']') + "}";
  const Json response = send(payload);
  ASSERT_THAT(response.is_object(), Eq(true));
  EXPECT_THAT(response["error"]["message"].get<std::string>(), HasSubstr("nesting"));
  EXPECT_THAT(control.hasPending(), Eq(false));
}

TEST_F(LocalEditorControlTest, FeedbackIsPrivateAtomicAndDoesNotFollowSymlinks) {
  start();
  const auto other = directory / "other.txt";
  {
    std::ofstream file(other);
    file << "unchanged";
  }
  ASSERT_THAT(symlink(other.c_str(), (endpoint + ".comments.json").c_str()), Eq(0));
  EXPECT_THAT(control.readFeedback().has_value(), Eq(false));
  std::string error;
  const Json saved{{"version", 1}, {"comments", Json::array()}};
  ASSERT_THAT(control.writeFeedback(saved, &error), Eq(true)) << error;
  ASSERT_THAT(control.readFeedback().has_value(), Eq(true));
  EXPECT_THAT(*control.readFeedback(), Eq(saved));
  struct stat info{};
  ASSERT_THAT(stat((endpoint + ".comments.json").c_str(), &info), Eq(0));
  EXPECT_THAT(info.st_mode & 0077, Eq(0));
  std::ifstream original(other);
  std::string value;
  original >> value;
  EXPECT_THAT(value, Eq("unchanged"));
}
#endif
}  // namespace
}  // namespace donner::editor
