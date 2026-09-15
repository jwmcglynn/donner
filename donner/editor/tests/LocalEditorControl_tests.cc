#include "donner/editor/LocalEditorControl.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <sstream>
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
class SocketPeer {
public:
  explicit SocketPeer(const std::string& endpoint) {
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    timeval timeout{.tv_sec = 3, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#ifdef SO_NOSIGPIPE
    int noSignal = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSignal, sizeof(noSignal));
#endif
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) disconnect();
  }
  ~SocketPeer() { disconnect(); }
  SocketPeer(const SocketPeer&) = delete;
  SocketPeer& operator=(const SocketPeer&) = delete;
  void disconnect() {
    if (fd >= 0) close(fd);
    fd = -1;
  }
  bool writeFrames(std::string_view bytes) {
    std::size_t sent = 0;
    while (sent < bytes.size()) {
#ifdef MSG_NOSIGNAL
      constexpr int flags = MSG_NOSIGNAL;
#else
      constexpr int flags = 0;
#endif
      const ssize_t count = ::send(fd, bytes.data() + sent, bytes.size() - sent, flags);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return false;
      sent += static_cast<std::size_t>(count);
    }
    return true;
  }
  Json readFrame() {
    while (buffered_.size() < 1024 * 1024) {
      const auto newline = buffered_.find('\n');
      if (newline != std::string::npos) {
        const Json result = Json::parse(buffered_.substr(0, newline), nullptr, false);
        buffered_.erase(0, newline + 1);
        return result;
      }
      char bytes[4096];
      const ssize_t count = recv(fd, bytes, sizeof(bytes), 0);
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) return Json(nullptr);
      buffered_.append(bytes, static_cast<std::size_t>(count));
    }
    return Json(nullptr);
  }
  int fd = -1;

private:
  std::string buffered_;
};

class LocalEditorControlTest : public testing::Test {
protected:
  std::filesystem::path directory;
  std::string endpoint;
  LocalEditorControl control;
  std::mutex mutex;
  std::condition_variable condition;
  std::size_t wakeCount = 0;

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
                      ++wakeCount;
                      condition.notify_all();
                    },
                    &error),
                Eq(true))
        << error;
  }
  bool awaitRequest(std::size_t count = 1) {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(2),
                              [this, count]() { return wakeCount >= count; });
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

TEST_F(LocalEditorControlTest, DeferredFeedbackDoesNotBlockAnotherClientsCommand) {
  start();
  auto waiting =
      std::async(std::launch::async, [this]() { return send(R"({"id":1,"method":"wait"})"); });
  ASSERT_THAT(awaitRequest(), Eq(true));
  EXPECT_THAT(control.process([](const Json&) -> std::optional<Json> { return std::nullopt; }),
              Eq(false));
  auto command =
      std::async(std::launch::async, [this]() { return send(R"({"id":2,"method":"ping"})"); });
  const bool commandArrived = awaitRequest(2);
  if (!commandArrived) control.stop();
  ASSERT_THAT(commandArrived, Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    if (request["method"] == "wait") return std::nullopt;
    return Json{{"id", request["id"]}, {"result", "command completed"}};
  }),
              Eq(true));
  EXPECT_THAT(command.get()["result"], Eq("command completed"));
  EXPECT_THAT(control.hasPending(), Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "feedback changed"}};
  }),
              Eq(true));
  EXPECT_THAT(waiting.get()["result"], Eq("feedback changed"));
}

TEST_F(LocalEditorControlTest, PartialInputDoesNotBlockAnotherClient) {
  start();
  const int incomplete = socket(AF_UNIX, SOCK_STREAM, 0);
  ASSERT_THAT(incomplete >= 0, Eq(true));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
  ASSERT_THAT(connect(incomplete, reinterpret_cast<sockaddr*>(&address), sizeof(address)), Eq(0));
  ASSERT_THAT(::send(incomplete, "{", 1, 0), Eq(1));
  auto command =
      std::async(std::launch::async, [this]() { return send(R"({"id":2,"method":"ping"})"); });
  const bool commandArrived = awaitRequest();
  close(incomplete);
  if (!commandArrived) control.stop();
  ASSERT_THAT(commandArrived, Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "responsive"}};
  }),
              Eq(true));
  EXPECT_THAT(command.get()["result"], Eq("responsive"));
}

TEST_F(LocalEditorControlTest, PersistentClientCanReceiveACommandBeforeItsDeferredWait) {
  start();
  SocketPeer peer(endpoint);
  ASSERT_THAT(peer.writeFrames("{\"id\":1,\"method\":\"wait\"}\n"
                               "{\"id\":2,\"method\":\"ping\"}\n"),
              Eq(true));
  ASSERT_THAT(awaitRequest(), Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    if (request["method"] == "wait") return std::nullopt;
    return Json{{"id", request["id"]}, {"result", "command"}};
  }),
              Eq(true));
  EXPECT_THAT(peer.readFrame(), Eq(Json({{"id", 2}, {"result", "command"}})));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "feedback"}};
  }),
              Eq(true));
  EXPECT_THAT(peer.readFrame(), Eq(Json({{"id", 1}, {"result", "feedback"}})));
  ASSERT_THAT(peer.writeFrames("{\"id\":3,\"method\":\"ping\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(2), Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "still connected"}};
  }),
              Eq(true));
  EXPECT_THAT(peer.readFrame(), Eq(Json({{"id", 3}, {"result", "still connected"}})));
}

TEST_F(LocalEditorControlTest, DisconnectCancelsUndispatchedWorkWithoutAffectingPeers) {
  start();
  SocketPeer disconnected(endpoint);
  ASSERT_THAT(disconnected.writeFrames("{\"id\":1,\"method\":\"ping\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(), Eq(true));
  disconnected.disconnect();
  SocketPeer peer(endpoint);
  ASSERT_THAT(peer.writeFrames("{\"id\":2,\"method\":\"ping\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(2), Eq(true));
  std::vector<int> dispatched;
  EXPECT_THAT(control.process([&](const Json& request) -> std::optional<Json> {
    dispatched.push_back(request["id"].get<int>());
    return Json{{"id", request["id"]}, {"result", "live"}};
  }),
              Eq(true));
  EXPECT_THAT(dispatched, testing::ElementsAre(2));
  EXPECT_THAT(peer.readFrame(), Eq(Json({{"id", 2}, {"result", "live"}})));
}

TEST_F(LocalEditorControlTest, ClientQueueBackpressureRetainsFramesUntilRepliesDrain) {
  start();
  SocketPeer peer(endpoint);
  std::string frames;
  for (int id = 1; id <= 9; ++id) frames += Json({{"id", id}, {"method", "ping"}}).dump() + "\n";
  ASSERT_THAT(peer.writeFrames(frames), Eq(true));
  ASSERT_THAT(awaitRequest(), Eq(true));
  std::vector<int> dispatched;
  auto handle = [&](const Json& request) -> std::optional<Json> {
    dispatched.push_back(request["id"].get<int>());
    return Json{{"id", request["id"]}, {"result", "ok"}};
  };
  EXPECT_THAT(control.process(handle), Eq(true));
  EXPECT_THAT(dispatched, testing::ElementsAre(1, 2, 3, 4, 5, 6, 7, 8));
  for (int id = 1; id <= 8; ++id)
    EXPECT_THAT(peer.readFrame(), Eq(Json({{"id", id}, {"result", "ok"}})));
  ASSERT_THAT(awaitRequest(2), Eq(true));
  EXPECT_THAT(control.process(handle), Eq(true));
  EXPECT_THAT(peer.readFrame(), Eq(Json({{"id", 9}, {"result", "ok"}})));
  EXPECT_THAT(dispatched, testing::ElementsAre(1, 2, 3, 4, 5, 6, 7, 8, 9));
}

TEST_F(LocalEditorControlTest, SlowReaderDoesNotBlockAnotherClientsCommand) {
  start();
  SocketPeer slow(endpoint);
  int receiveBytes = 4096;
  ASSERT_THAT(setsockopt(slow.fd, SOL_SOCKET, SO_RCVBUF, &receiveBytes, sizeof(receiveBytes)),
              Eq(0));
  ASSERT_THAT(slow.writeFrames("{\"id\":1,\"method\":\"large\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(), Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", std::string(8 * 1024 * 1024, 'x')}};
  }),
              Eq(true));
  SocketPeer peer(endpoint);
  ASSERT_THAT(peer.writeFrames("{\"id\":2,\"method\":\"ping\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(2), Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "responsive"}};
  }),
              Eq(true));
  EXPECT_THAT(peer.readFrame(), Eq(Json({{"id", 2}, {"result", "responsive"}})));
}

TEST_F(LocalEditorControlTest, ConnectionLimitRefusesAdditionalPeersAndStopResetsIt) {
  start();
  std::vector<std::unique_ptr<SocketPeer>> peers;
  for (int id = 1; id <= 8; ++id) {
    peers.push_back(std::make_unique<SocketPeer>(endpoint));
    ASSERT_THAT(peers.back()->writeFrames(Json({{"id", id}, {"method", "ping"}}).dump() + "\n"),
                Eq(true));
    ASSERT_THAT(awaitRequest(id), Eq(true));
  }
  SocketPeer excess(endpoint);
  EXPECT_THAT(excess.readFrame().is_null(), Eq(true));
  control.stop();
  peers.clear();
  start();
  SocketPeer restarted(endpoint);
  ASSERT_THAT(restarted.writeFrames("{\"id\":9,\"method\":\"ping\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(9), Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "new listener"}};
  }),
              Eq(true));
  EXPECT_THAT(restarted.readFrame(), Eq(Json({{"id", 9}, {"result", "new listener"}})));
}

TEST_F(LocalEditorControlTest, OversizedResponsePreservesTheRequestId) {
  start();
  SocketPeer peer(endpoint);
  ASSERT_THAT(peer.writeFrames("{\"id\":7,\"method\":\"large\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(), Eq(true));
  control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", std::string(16 * 1024 * 1024, 'x')}};
  });
  const Json response = peer.readFrame();
  ASSERT_THAT(response.is_object(), Eq(true));
  EXPECT_THAT(response["id"], Eq(7));
  EXPECT_THAT(response["error"]["message"].get<std::string>(), HasSubstr("16 MiB"));
}

TEST_F(LocalEditorControlTest, AggregateResponseBudgetDisconnectsOnlyTheOverloadedPeer) {
  start();
  std::vector<std::unique_ptr<SocketPeer>> peers;
  for (int id = 1; id <= 3; ++id) {
    peers.push_back(std::make_unique<SocketPeer>(endpoint));
    int receiveBytes = 4096;
    ASSERT_THAT(
        setsockopt(peers.back()->fd, SOL_SOCKET, SO_RCVBUF, &receiveBytes, sizeof(receiveBytes)),
        Eq(0));
    ASSERT_THAT(peers.back()->writeFrames(Json({{"id", id}, {"method", "large"}}).dump() + "\n"),
                Eq(true));
    ASSERT_THAT(awaitRequest(id), Eq(true));
  }
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", std::string(12 * 1024 * 1024, 'x')}};
  }),
              Eq(true));
  EXPECT_THAT(peers.back()->readFrame().is_null(), Eq(true));
  SocketPeer command(endpoint);
  ASSERT_THAT(command.writeFrames("{\"id\":4,\"method\":\"ping\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(4), Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "bounded"}};
  }),
              Eq(true));
  EXPECT_THAT(command.readFrame(), Eq(Json({{"id", 4}, {"result", "bounded"}})));
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
TEST_F(LocalEditorControlTest, NativeStdioForwardsRequestsAndSuppressesNotificationReplies) {
  start();
  std::istringstream input(
      "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}\n");
  std::ostringstream output, errors;
  auto client = std::async(
      std::launch::async, [&]() { return RunEditorControlStdio(endpoint, input, output, errors); });
  ASSERT_THAT(awaitRequest(1), Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    EXPECT_THAT(request["method"], Eq("notifications/initialized"));
    return Json(nullptr);
  }),
              Eq(true));
  ASSERT_THAT(awaitRequest(2), Eq(true));
  EXPECT_THAT(control.process([](const Json& request) -> std::optional<Json> {
    return Json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", "live editor"}};
  }),
              Eq(true));
  EXPECT_THAT(client.get(), Eq(0));
  EXPECT_THAT(errors.str(), Eq(""));
  const Json response = Json::parse(output.str());
  EXPECT_THAT(response["id"], Eq(7));
  EXPECT_THAT(response["result"], Eq("live editor"));
}

TEST_F(LocalEditorControlTest, NativeStdioReportsMismatchedIdsWithoutReplayingRequests) {
  start();
  std::istringstream input("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}\n");
  std::ostringstream output, errors;
  auto client = std::async(
      std::launch::async, [&]() { return RunEditorControlStdio(endpoint, input, output, errors); });
  ASSERT_THAT(awaitRequest(), Eq(true));
  control.process([](const Json&) -> std::optional<Json> {
    return Json{{"jsonrpc", "2.0"}, {"id", 8}, {"result", "wrong request"}};
  });
  EXPECT_THAT(client.get(), Eq(0));
  const Json response = Json::parse(output.str());
  EXPECT_THAT(response["id"], Eq(7));
  EXPECT_THAT(response["error"]["message"].get<std::string>(), HasSubstr("ID"));
  EXPECT_THAT(control.hasPending(), Eq(false));
}

TEST_F(LocalEditorControlTest, NativeStdioRejectsUnboundedMalformedAndInvalidProtocolInput) {
  const std::vector<std::string> inputs{
      "{\"jsonrpc\":\"2.0\",\"nested\":" + std::string(65, '[') + "0" + std::string(65, ']') +
          "}\n",
      "{\"value\":\"" + std::string(1024 * 1024, 'x') + "\"}\n",
      "{\"jsonrpc\":\"2.0\",\"id\":7}",
      "[]\n",
      "{\"jsonrpc\":\"2.0\",\"id\":[]}\n",
      "{\"jsonrpc\":\"1.0\",\"id\":7}\n",
  };
  for (const auto& bytes : inputs) {
    std::istringstream input(bytes);
    std::ostringstream output, errors;
    EXPECT_THAT(RunEditorControlStdio(endpoint, input, output, errors), Eq(2));
    EXPECT_THAT(Json::parse(output.str()).contains("error"), Eq(true));
  }
}

TEST_F(LocalEditorControlTest, NativeStdioRequiresPrivateEndpointPermissions) {
  start();
  ASSERT_THAT(chmod(directory.c_str(), 0755), Eq(0));
  std::istringstream input("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}\n");
  std::ostringstream output, errors;
  EXPECT_THAT(RunEditorControlStdio(endpoint, input, output, errors), Eq(0));
  EXPECT_THAT(Json::parse(output.str())["error"]["message"].get<std::string>(),
              HasSubstr("private"));
  EXPECT_THAT(control.hasPending(), Eq(false));
  ASSERT_THAT(chmod(directory.c_str(), 0700), Eq(0));
}

#endif
}  // namespace
}  // namespace donner::editor
