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
#include <fcntl.h>
#include <poll.h>
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
  explicit SocketPeer(const std::string& endpoint, bool initialize = true) {
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
    if (fd >= 0 && initialize && !initializeSession()) disconnect();
  }
  explicit SocketPeer(int descriptor) : fd(descriptor) {
    timeval timeout{.tv_sec = 2, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  }
  bool initializeSession(std::string name = "fixture-agent") {
    const Json request{{"jsonrpc", "2.0"},
                       {"id", "fixture-initialize"},
                       {"method", "initialize"},
                       {"params",
                        {{"protocolVersion", "2025-06-18"},
                         {"capabilities", Json::object()},
                         {"clientInfo", {{"name", name}, {"version", "1.0"}}}}}};
    if (!writeFrames(request.dump() + "\n") || !readFrame().contains("result")) return false;
    if (!writeFrames("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"))
      return false;
    return readFrame().is_null() && lastReadCompleted;
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
    lastReadCompleted = false;
    while (buffered_.size() < 1024 * 1024) {
      const auto newline = buffered_.find('\n');
      if (newline != std::string::npos) {
        const Json result = Json::parse(buffered_.substr(0, newline), nullptr, false);
        buffered_.erase(0, newline + 1);
        lastReadCompleted = true;
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
  bool lastReadCompleted = false;

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
                      if (control.hasPending()) ++wakeCount;
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
  template <typename Predicate>
  bool processUntil(const LocalEditorControl::Handler& handler, Predicate complete) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!complete() && std::chrono::steady_clock::now() < deadline) {
      control.process(handler);
      if (complete()) return true;
      std::unique_lock lock(mutex);
      condition.wait_for(lock, std::chrono::milliseconds(10));
    }
    return complete();
  }
  bool awaitAgents(std::size_t count) {
    std::unique_lock lock(mutex);
    return condition.wait_for(lock, std::chrono::seconds(2),
                              [&] { return control.connectedAgents().size() == count; });
  }
  bool stdioReplyWritten(int id) {
    std::ifstream file(directory / "stdio-output-0");
    const std::string bytes((std::istreambuf_iterator<char>(file)),
                            std::istreambuf_iterator<char>());
    std::size_t start = 0;
    for (auto end = bytes.find('\n'); end != std::string::npos; end = bytes.find('\n', start)) {
      const Json reply = Json::parse(bytes.substr(start, end - start), nullptr, false);
      if (reply.is_object() && reply.value("id", Json(nullptr)) == id) return true;
      start = end + 1;
    }
    return false;
  }
  Json send(std::string bytes) {
    SocketPeer peer(endpoint);
    if (!peer.writeFrames(bytes + "\n")) return Json(nullptr);
    return peer.readFrame();
  }
  int runStdio(std::istringstream& input, std::ostringstream& output, std::ostringstream& errors,
               bool initialize = true) {
    const std::string prefix =
        initialize
            ? "{\"jsonrpc\":\"2.0\",\"id\":\"fixture-bridge-init\",\"method\":\"initialize\","
              "\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{"
              "\"name\":\"fixture-bridge\",\"version\":\"1\"}}}\n"
              "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"
            : "";
    const auto inPath = directory / ("stdio-input-" + std::to_string(stdioSequence));
    const auto outPath = directory / ("stdio-output-" + std::to_string(stdioSequence++));
    {
      std::ofstream file(inPath);
      file << prefix << input.str();
    }
    const int inFd = open(inPath.c_str(), O_RDONLY);
    const int outFd = open(outPath.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (inFd < 0 || outFd < 0) return 99;
    const int result = RunEditorControlStdio(endpoint, inFd, outFd, errors);
    close(inFd);
    close(outFd);
    std::ifstream replies(outPath);
    std::string line;
    while (std::getline(replies, line)) {
      const Json reply = Json::parse(line, nullptr, false);
      if (reply.is_object() && reply.value("id", Json(nullptr)) == "fixture-bridge-init" &&
          reply.contains("result"))
        continue;
      output << line << '\n';
    }
    return result;
  }
  std::size_t stdioSequence = 0;
};

TEST_F(LocalEditorControlTest, PresenceRequiresACompletedNamedHandshakeAndEndsOnDisconnect) {
  start();
  SocketPeer peer(endpoint, false);
  EXPECT_THAT(control.connectedAgents(), testing::IsEmpty());
  const Json initialize{{"jsonrpc", "2.0"},
                        {"id", 1},
                        {"method", "initialize"},
                        {"params",
                         {{"protocolVersion", "2025-06-18"},
                          {"capabilities", Json::object()},
                          {"clientInfo", {{"name", "design-agent"}, {"version", "1"}}}}}};
  ASSERT_THAT(peer.writeFrames(initialize.dump() + "\n"), Eq(true));
  EXPECT_THAT(peer.readFrame()["result"]["protocolVersion"], Eq("2025-06-18"));
  EXPECT_THAT(control.connectedAgents(), testing::IsEmpty());
  ASSERT_THAT(peer.writeFrames("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"),
              Eq(true));
  EXPECT_THAT(peer.readFrame().is_null(), Eq(true));
  ASSERT_THAT(peer.lastReadCompleted, Eq(true));
  ASSERT_THAT(awaitAgents(1), Eq(true));
  const auto connected = control.connectedAgents().front();
  EXPECT_THAT(connected.name, Eq("design-agent"));
  EXPECT_THAT(connected.version, Eq("1"));
  EXPECT_THAT(connected.connectionId, testing::Gt(0u));
  peer.disconnect();
  ASSERT_THAT(awaitAgents(0), Eq(true));
  SocketPeer reconnect(endpoint);
  ASSERT_THAT(awaitAgents(1), Eq(true));
  EXPECT_THAT(control.connectedAgents().front().connectionId, testing::Gt(connected.connectionId));
}

TEST_F(LocalEditorControlTest, UninitializedRequestsCannotReachTheDocumentHandler) {
  start();
  SocketPeer peer(endpoint, false);
  ASSERT_THAT(peer.writeFrames("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/list\"}\n"),
              Eq(true));
  EXPECT_THAT(peer.readFrame()["error"]["code"], Eq(-32002));
  EXPECT_THAT(control.hasPending(), Eq(false));
  EXPECT_THAT(control.connectedAgents(), testing::IsEmpty());
  ASSERT_THAT(peer.writeFrames("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}\n"), Eq(true));
  EXPECT_THAT(peer.readFrame()["result"], Eq(Json::object()));
  EXPECT_THAT(control.connectedAgents(), testing::IsEmpty());
}

TEST_F(LocalEditorControlTest, NegotiatesOnlySupportedVersionsAndRejectsInvalidNames) {
  start();
  for (const std::string version : {"2024-11-05", "2025-03-26", "2025-06-18", "2099-01-01"}) {
    SocketPeer peer(endpoint, false);
    const Json request{{"jsonrpc", "2.0"},
                       {"id", 1},
                       {"method", "initialize"},
                       {"params",
                        {{"protocolVersion", version},
                         {"capabilities", Json::object()},
                         {"clientInfo", {{"name", "client"}, {"version", "1"}}}}}};
    ASSERT_THAT(peer.writeFrames(request.dump() + "\n"), Eq(true));
    EXPECT_THAT(peer.readFrame()["result"]["protocolVersion"],
                Eq(version == "2099-01-01" ? "2025-06-18" : version));
    EXPECT_THAT(control.connectedAgents(), testing::IsEmpty());
  }
  SocketPeer malformed(endpoint, false);
  for (const std::string name : {"", "untrusted\nlabel", "untrusted\tlabel"}) {
    const Json request{{"jsonrpc", "2.0"},
                       {"id", 1},
                       {"method", "initialize"},
                       {"params",
                        {{"protocolVersion", "2025-06-18"},
                         {"capabilities", Json::object()},
                         {"clientInfo", {{"name", name}, {"version", "1"}}}}}};
    ASSERT_THAT(malformed.writeFrames(request.dump() + "\n"), Eq(true));
    EXPECT_THAT(malformed.readFrame()["error"]["code"], Eq(-32602));
    EXPECT_THAT(control.connectedAgents(), testing::IsEmpty());
  }
}

TEST_F(LocalEditorControlTest, IdleConnectionsLosePresenceAfterTheirLease) {
  LocalEditorControl lease(std::chrono::milliseconds(100));
  const std::string path = (directory / "lease.sock").string();
  std::string error;
  ASSERT_THAT(lease.start(path, [&] { condition.notify_all(); }, &error), Eq(true)) << error;
  SocketPeer peer(path);
  ASSERT_THAT(lease.connectedAgents().size(), Eq(1u));
  std::unique_lock lock(mutex);
  EXPECT_THAT(condition.wait_for(lock, std::chrono::seconds(2),
                                 [&] { return lease.connectedAgents().empty(); }),
              Eq(true));
}

TEST_F(LocalEditorControlTest, NativeHeartbeatKeepsIdlePresenceAndNeverLeaksIntoStdout) {
  LocalEditorControl lease(std::chrono::milliseconds(600));
  const std::string path = (directory / "heartbeat.sock").string();
  std::string error;
  ASSERT_THAT(lease.start(path, [&] { condition.notify_all(); }, &error), Eq(true)) << error;
  int pair[2];
  ASSERT_THAT(socketpair(AF_UNIX, SOCK_STREAM, 0, pair), Eq(0));
  std::future<int> bridge;
  SocketPeer peer(pair[0]);
  std::ostringstream errors;
  bridge = std::async(std::launch::async, [&] {
    const int result = RunEditorControlStdio(path, pair[1], pair[1], errors,
                                             {.heartbeatInterval = std::chrono::milliseconds(100),
                                              .requestTimeout = std::chrono::seconds(1)});
    close(pair[1]);
    return result;
  });
  const Json request{{"jsonrpc", "2.0"},
                     {"id", 19},
                     {"method", "initialize"},
                     {"params",
                      {{"protocolVersion", "2025-06-18"},
                       {"capabilities", Json::object()},
                       {"clientInfo", {{"name", "bridge-agent"}, {"version", "1"}}}}}};
  ASSERT_THAT(peer.writeFrames(request.dump() + "\n"), Eq(true));
  EXPECT_THAT(peer.readFrame()["id"], Eq(19));
  ASSERT_THAT(peer.writeFrames("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}\n"),
              Eq(true));
  {
    std::unique_lock lock(mutex);
    ASSERT_THAT(condition.wait_for(lock, std::chrono::seconds(2),
                                   [&] { return lease.connectedAgents().size() == 1; }),
                Eq(true));
  }
  pollfd output{peer.fd, POLLIN, 0};
  EXPECT_THAT(poll(&output, 1, 2000), Eq(0))
      << "Internal heartbeat traffic reached the MCP client or its connection ended";
  EXPECT_THAT(lease.connectedAgents().size(), Eq(1u));
  ASSERT_THAT(shutdown(peer.fd, SHUT_WR), Eq(0));
  EXPECT_THAT(bridge.get(), Eq(0));
  EXPECT_THAT(errors.str(), Eq(""));
  std::unique_lock lock(mutex);
  EXPECT_THAT(condition.wait_for(lock, std::chrono::seconds(2),
                                 [&] { return lease.connectedAgents().empty(); }),
              Eq(true));
}

TEST_F(LocalEditorControlTest, RequestContextCannotBeSpoofedByArgumentsOrReinitialization) {
  start();
  SocketPeer peer(endpoint);
  const auto connection = control.connectedAgents().front();
  ASSERT_THAT(peer.writeFrames("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"echo\",\"params\":{"
                               "\"agent\":{\"connectionId\":999,\"name\":\"forged\"}}}\n"),
              Eq(true));
  ASSERT_THAT(awaitRequest(), Eq(true));
  EXPECT_THAT(
      control.process([&](const Json& request,
                          const LocalEditorControl::AgentSession& agent) -> std::optional<Json> {
        EXPECT_THAT(agent.connectionId, Eq(connection.connectionId));
        EXPECT_THAT(agent.name, Eq("fixture-agent"));
        return Json{{"id", request["id"]}, {"result", "bound"}};
      }),
      Eq(true));
  EXPECT_THAT(peer.readFrame()["result"], Eq("bound"));
  const Json reinitialize{{"jsonrpc", "2.0"},
                          {"id", 2},
                          {"method", "initialize"},
                          {"params",
                           {{"protocolVersion", "2025-06-18"},
                            {"capabilities", Json::object()},
                            {"clientInfo", {{"name", "replacement"}, {"version", "2"}}}}}};
  ASSERT_THAT(peer.writeFrames(reinitialize.dump() + "\n"), Eq(true));
  EXPECT_THAT(peer.readFrame()["error"]["code"], Eq(-32600));
  EXPECT_THAT(control.connectedAgents().front().name, Eq(connection.name));
}

TEST_F(LocalEditorControlTest, CancellationIsScopedToTheOriginatingConnection) {
  start();
  SocketPeer first(endpoint), second(endpoint);
  const auto secondId = control.connectedAgents().back().connectionId;
  ASSERT_THAT(first.writeFrames("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"wait\"}\n"), Eq(true));
  ASSERT_THAT(second.writeFrames("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"wait\"}\n"), Eq(true));
  ASSERT_THAT(first.writeFrames("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/"
                                "cancelled\",\"params\":{\"requestId\":7}}\n"),
              Eq(true));
  EXPECT_THAT(first.readFrame()["error"]["code"], Eq(-32800));
  EXPECT_THAT(first.readFrame().is_null(), Eq(true));
  ASSERT_THAT(first.lastReadCompleted, Eq(true));
  bool secondDispatched = false;
  EXPECT_THAT(processUntil(
                  [&](const Json& request,
                      const LocalEditorControl::AgentSession& agent) -> std::optional<Json> {
                    EXPECT_THAT(agent.connectionId, Eq(secondId));
                    secondDispatched = true;
                    return Json{{"id", request["id"]}, {"result", "other connection preserved"}};
                  },
                  [&] { return secondDispatched; }),
              Eq(true));
  EXPECT_THAT(second.readFrame()["result"], Eq("other connection preserved"));
  ASSERT_THAT(first.writeFrames("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/"
                                "cancelled\",\"params\":{\"requestId\":null}}\n"),
              Eq(true));
  EXPECT_THAT(first.readFrame().is_null(), Eq(true));
  EXPECT_THAT(first.lastReadCompleted, Eq(true));
  EXPECT_THAT(control.connectedAgents().size(), Eq(2u));
}

TEST_F(LocalEditorControlTest, DispatchesOnlyWhenTheUiProcessesTheRequest) {
  start();
  auto response =
      std::async(std::launch::async, [this]() { return send(R"({"id":7,"method":"echo"})"); });
  ASSERT_THAT(awaitRequest(), Eq(true));
  EXPECT_THAT(control.hasPending(), Eq(true));
  EXPECT_THAT(control.process([](const Json& request,
                                 const LocalEditorControl::AgentSession&) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "live editor"}};
  }),
              Eq(true));
  EXPECT_THAT(response.get()["result"], Eq("live editor"));
  EXPECT_THAT(control.hasPending(), Eq(false));
}

TEST_F(LocalEditorControlTest, DeferredFeedbackWaitDoesNotBlockUiProcessing) {
  start();
  auto response =
      std::async(std::launch::async, [this]() { return send(R"({"id":7,"method":"echo"})"); });
  ASSERT_THAT(awaitRequest(), Eq(true));
  EXPECT_THAT(control.process([](const Json&, const LocalEditorControl::AgentSession&)
                                  -> std::optional<Json> { return std::nullopt; }),
              Eq(false));
  EXPECT_THAT(control.hasPending(), Eq(true));
  EXPECT_THAT(control.process(
                  [](const Json&, const LocalEditorControl::AgentSession&) -> std::optional<Json> {
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
  EXPECT_THAT(control.process([](const Json&, const LocalEditorControl::AgentSession&)
                                  -> std::optional<Json> { return std::nullopt; }),
              Eq(false));
  auto command =
      std::async(std::launch::async, [this]() { return send(R"({"id":2,"method":"echo"})"); });
  EXPECT_THAT(
      processUntil(
          [](const Json& request, const LocalEditorControl::AgentSession&) -> std::optional<Json> {
            if (request["method"] == "wait") return std::nullopt;
            return Json{{"id", request["id"]}, {"result", "command completed"}};
          },
          [&] { return command.wait_for(std::chrono::seconds(0)) == std::future_status::ready; }),
      Eq(true));
  EXPECT_THAT(command.get()["result"], Eq("command completed"));
  EXPECT_THAT(control.hasPending(), Eq(true));
  EXPECT_THAT(control.process([](const Json& request,
                                 const LocalEditorControl::AgentSession&) -> std::optional<Json> {
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
      std::async(std::launch::async, [this]() { return send(R"({"id":2,"method":"echo"})"); });
  const bool commandArrived = awaitRequest();
  close(incomplete);
  if (!commandArrived) control.stop();
  ASSERT_THAT(commandArrived, Eq(true));
  EXPECT_THAT(control.process([](const Json& request,
                                 const LocalEditorControl::AgentSession&) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "responsive"}};
  }),
              Eq(true));
  EXPECT_THAT(command.get()["result"], Eq("responsive"));
}

TEST_F(LocalEditorControlTest, PersistentClientCanReceiveACommandBeforeItsDeferredWait) {
  start();
  SocketPeer peer(endpoint);
  ASSERT_THAT(peer.writeFrames("{\"id\":1,\"method\":\"wait\"}\n"
                               "{\"id\":2,\"method\":\"echo\"}\n"),
              Eq(true));
  ASSERT_THAT(awaitRequest(), Eq(true));
  EXPECT_THAT(control.process([](const Json& request,
                                 const LocalEditorControl::AgentSession&) -> std::optional<Json> {
    if (request["method"] == "wait") return std::nullopt;
    return Json{{"id", request["id"]}, {"result", "command"}};
  }),
              Eq(true));
  EXPECT_THAT(peer.readFrame(), Eq(Json({{"id", 2}, {"result", "command"}})));
  EXPECT_THAT(control.process([](const Json& request,
                                 const LocalEditorControl::AgentSession&) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "feedback"}};
  }),
              Eq(true));
  EXPECT_THAT(peer.readFrame(), Eq(Json({{"id", 1}, {"result", "feedback"}})));
  ASSERT_THAT(peer.writeFrames("{\"id\":3,\"method\":\"echo\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(2), Eq(true));
  EXPECT_THAT(control.process([](const Json& request,
                                 const LocalEditorControl::AgentSession&) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", "still connected"}};
  }),
              Eq(true));
  EXPECT_THAT(peer.readFrame(), Eq(Json({{"id", 3}, {"result", "still connected"}})));
}

TEST_F(LocalEditorControlTest, DisconnectCancelsUndispatchedWorkWithoutAffectingPeers) {
  start();
  SocketPeer disconnected(endpoint);
  ASSERT_THAT(disconnected.writeFrames("{\"id\":1,\"method\":\"echo\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(), Eq(true));
  disconnected.disconnect();
  SocketPeer peer(endpoint);
  ASSERT_THAT(peer.writeFrames("{\"id\":2,\"method\":\"echo\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(2), Eq(true));
  std::vector<int> dispatched;
  EXPECT_THAT(control.process([&](const Json& request,
                                  const LocalEditorControl::AgentSession&) -> std::optional<Json> {
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
  for (int id = 1; id <= 9; ++id) frames += Json({{"id", id}, {"method", "echo"}}).dump() + "\n";
  ASSERT_THAT(peer.writeFrames(frames), Eq(true));
  ASSERT_THAT(awaitRequest(), Eq(true));
  std::vector<int> dispatched;
  auto handle = [&](const Json& request,
                    const LocalEditorControl::AgentSession&) -> std::optional<Json> {
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
  EXPECT_THAT(control.process([](const Json& request,
                                 const LocalEditorControl::AgentSession&) -> std::optional<Json> {
    return Json{{"id", request["id"]}, {"result", std::string(8 * 1024 * 1024, 'x')}};
  }),
              Eq(true));
  SocketPeer peer(endpoint);
  ASSERT_THAT(peer.writeFrames("{\"id\":2,\"method\":\"echo\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(2), Eq(true));
  EXPECT_THAT(control.process([](const Json& request,
                                 const LocalEditorControl::AgentSession&) -> std::optional<Json> {
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
    ASSERT_THAT(peers.back()->writeFrames(Json({{"id", id}, {"method", "echo"}}).dump() + "\n"),
                Eq(true));
    EXPECT_THAT(control.connectedAgents().size(), Eq(static_cast<std::size_t>(id)));
  }
  SocketPeer excess(endpoint);
  EXPECT_THAT(excess.readFrame().is_null(), Eq(true));
  control.stop();
  peers.clear();
  wakeCount = 0;
  start();
  SocketPeer restarted(endpoint);
  ASSERT_THAT(restarted.writeFrames("{\"id\":9,\"method\":\"echo\"}\n"), Eq(true));
  ASSERT_THAT(awaitRequest(1), Eq(true));
  EXPECT_THAT(control.process([](const Json& request,
                                 const LocalEditorControl::AgentSession&) -> std::optional<Json> {
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
  control.process(
      [](const Json& request, const LocalEditorControl::AgentSession&) -> std::optional<Json> {
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
  }
  int dispatched = 0;
  EXPECT_THAT(
      processUntil(
          [&](const Json& request, const LocalEditorControl::AgentSession&) -> std::optional<Json> {
            ++dispatched;
            return Json{{"id", request["id"]}, {"result", std::string(12 * 1024 * 1024, 'x')}};
          },
          [&] { return dispatched == 3; }),
      Eq(true));
  EXPECT_THAT(peers.back()->readFrame().is_null(), Eq(true));
  SocketPeer command(endpoint);
  ASSERT_THAT(command.writeFrames("{\"id\":4,\"method\":\"echo\"}\n"), Eq(true));
  bool commandDispatched = false;
  EXPECT_THAT(
      processUntil(
          [&](const Json& request, const LocalEditorControl::AgentSession&) -> std::optional<Json> {
            commandDispatched = true;
            return Json{{"id", request["id"]}, {"result", "bounded"}};
          },
          [&] { return commandDispatched; }),
      Eq(true));
  EXPECT_THAT(command.readFrame(), Eq(Json({{"id", 4}, {"result", "bounded"}})));
}

TEST_F(LocalEditorControlTest, ShutdownCancelsPendingWorkAndRemovesOnlyOwnedSocket) {
  start();
  auto response =
      std::async(std::launch::async, [this]() { return send(R"({"id":7,"method":"echo"})"); });
  ASSERT_THAT(awaitRequest(), Eq(true));
  control.stop();
  EXPECT_THAT(std::filesystem::exists(endpoint), Eq(false));
  EXPECT_THAT(control.process([](const Json&, const LocalEditorControl::AgentSession&)
                                  -> std::optional<Json> { return Json::object(); }),
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
      "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"echo\"}\n");
  std::ostringstream output, errors;
  auto client = std::async(std::launch::async, [&]() { return runStdio(input, output, errors); });
  ASSERT_THAT(awaitRequest(1), Eq(true));
  EXPECT_THAT(control.process([](const Json& request,
                                 const LocalEditorControl::AgentSession&) -> std::optional<Json> {
    return Json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", "live editor"}};
  }),
              Eq(true));
  EXPECT_THAT(client.get(), Eq(0));
  EXPECT_THAT(errors.str(), Eq(""));
  const Json response = Json::parse(output.str());
  EXPECT_THAT(response["id"], Eq(7));
  EXPECT_THAT(response["result"], Eq("live editor"));
}

TEST_F(LocalEditorControlTest, NativeStdioReadsCommandsWhileFeedbackIsWaiting) {
  start();
  std::istringstream input(
      "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"wait\"}\n"
      "{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"echo\"}\n");
  std::ostringstream output, errors;
  auto client = std::async(std::launch::async, [&]() { return runStdio(input, output, errors); });
  ASSERT_THAT(awaitRequest(), Eq(true));
  bool commandSeen = false;
  auto handle = [&](const Json& request,
                    const LocalEditorControl::AgentSession&) -> std::optional<Json> {
    if (request["method"] == "wait") return std::nullopt;
    commandSeen = true;
    return Json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", "responsive"}};
  };
  const bool delivered = processUntil(handle, [&] { return stdioReplyWritten(2); });
  if (!delivered) {
    control.stop();
    (void)client.get();
    FAIL() << "A feedback wait prevented the adapter from delivering another command reply";
  }
  EXPECT_THAT(commandSeen, Eq(true));
  control.process(
      [](const Json& request, const LocalEditorControl::AgentSession&) -> std::optional<Json> {
        return Json{{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", "feedback"}};
      });
  EXPECT_THAT(client.get(), Eq(0));
  std::istringstream replies(output.str());
  std::string line;
  ASSERT_THAT(static_cast<bool>(std::getline(replies, line)), Eq(true));
  EXPECT_THAT(Json::parse(line)["id"], Eq(2));
  ASSERT_THAT(static_cast<bool>(std::getline(replies, line)), Eq(true));
  EXPECT_THAT(Json::parse(line)["id"], Eq(1));
}

TEST_F(LocalEditorControlTest, NativeStdioReportsMismatchedIdsWithoutReplayingRequests) {
  start();
  std::istringstream input("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"echo\"}\n");
  std::ostringstream output, errors;
  auto client = std::async(std::launch::async, [&]() { return runStdio(input, output, errors); });
  ASSERT_THAT(awaitRequest(), Eq(true));
  control.process([](const Json&, const LocalEditorControl::AgentSession&) -> std::optional<Json> {
    return Json{{"jsonrpc", "2.0"}, {"id", 8}, {"result", "wrong request"}};
  });
  EXPECT_THAT(client.get(), Eq(1));
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
    EXPECT_THAT(runStdio(input, output, errors, false), Eq(2));
    EXPECT_THAT(Json::parse(output.str()).contains("error"), Eq(true));
  }
}

TEST_F(LocalEditorControlTest, NativeStdioAcceptsPeerEofAfterAllInputAndRepliesAreComplete) {
  SocketPeer listener(socket(AF_UNIX, SOCK_STREAM, 0));
  ASSERT_THAT(listener.fd >= 0, Eq(true));
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, endpoint.c_str(), endpoint.size() + 1);
  ASSERT_THAT(bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)), Eq(0));
  ASSERT_THAT(chmod(endpoint.c_str(), 0600), Eq(0));
  ASSERT_THAT(listen(listener.fd, 1), Eq(0));
  auto server = std::async(std::launch::async, [&] {
    pollfd incoming{listener.fd, POLLIN, 0};
    if (poll(&incoming, 1, 2000) != 1) return false;
    SocketPeer peer(accept(listener.fd, nullptr, nullptr));
    if (peer.fd < 0) return false;
    const Json request = peer.readFrame();
    if (!request.is_object() || !request.contains("id")) return false;
    return peer.writeFrames(
        Json({{"jsonrpc", "2.0"}, {"id", request["id"]}, {"result", "complete"}}).dump() + "\n");
  });
  std::istringstream input("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\"}\n");
  std::ostringstream output, errors;
  EXPECT_THAT(runStdio(input, output, errors, false), Eq(0));
  EXPECT_THAT(server.get(), Eq(true));
  EXPECT_THAT(errors.str(), Eq(""));
  EXPECT_THAT(Json::parse(output.str()),
              Eq(Json({{"jsonrpc", "2.0"}, {"id", 7}, {"result", "complete"}})));
}

TEST_F(LocalEditorControlTest, NativeStdioRequiresPrivateEndpointPermissions) {
  start();
  ASSERT_THAT(chmod(directory.c_str(), 0755), Eq(0));
  std::istringstream input("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"echo\"}\n");
  std::ostringstream output, errors;
  EXPECT_THAT(runStdio(input, output, errors), Eq(1));
  EXPECT_THAT(Json::parse(output.str())["error"]["message"].get<std::string>(),
              HasSubstr("private"));
  EXPECT_THAT(control.hasPending(), Eq(false));
  ASSERT_THAT(chmod(directory.c_str(), 0700), Eq(0));
}

#endif
}  // namespace
}  // namespace donner::editor
