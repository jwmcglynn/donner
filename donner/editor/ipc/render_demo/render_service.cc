#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "donner/editor/ipc/render_demo/render_messages.h"
#include "donner/editor/ipc/teleport/service_runner.h"

extern "C" char** environ;

namespace {

namespace fs = std::filesystem;
using donner::teleport::render_demo::RenderRequest;
using donner::teleport::render_demo::RenderResponse;

std::atomic<std::uint64_t> g_seq{0};

std::string ResolveDonnerSvgTool() {
  if (const char* override = std::getenv("DONNER_TELEPORT_RENDER_TOOL");
      override && *override) {
    return override;
  }

  // Fallback: walk up from /proc/self/exe looking for a sibling `donner-svg`.
  std::error_code ec;
  fs::path self = fs::read_symlink("/proc/self/exe", ec);
  if (!ec) {
    fs::path dir = self.parent_path();
    for (int i = 0; i < 6 && !dir.empty(); ++i) {
      fs::path candidate = dir / "donner-svg";
      if (fs::exists(candidate, ec)) {
        return candidate.string();
      }
      dir = dir.parent_path();
    }
  }

  return "donner-svg";
}

bool WriteFile(const fs::path& path, std::string_view data) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
  return out.good();
}

bool ReadFileBytes(const fs::path& path, std::vector<std::byte>& out) {
  std::ifstream in(path, std::ios::binary | std::ios::ate);
  if (!in) return false;
  const std::streamsize size = in.tellg();
  if (size < 0) return false;
  in.seekg(0, std::ios::beg);
  out.resize(static_cast<std::size_t>(size));
  if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size)) {
    return false;
  }
  return true;
}

// Spawn donner-svg synchronously with a 10s timeout. Returns 0 on success,
// nonzero on failure (sets *errMsg).
int RunDonnerSvg(const std::string& tool, const fs::path& inputSvg,
                 const fs::path& outputPng, std::int32_t width,
                 std::int32_t height, std::string& errMsg) {
  const std::string widthStr = std::to_string(width);
  const std::string heightStr = std::to_string(height);
  const std::string inStr = inputSvg.string();
  const std::string outStr = outputPng.string();

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(tool.c_str()));
  argv.push_back(const_cast<char*>(inStr.c_str()));
  argv.push_back(const_cast<char*>("--output"));
  argv.push_back(const_cast<char*>(outStr.c_str()));
  argv.push_back(const_cast<char*>("--width"));
  argv.push_back(const_cast<char*>(widthStr.c_str()));
  argv.push_back(const_cast<char*>("--height"));
  argv.push_back(const_cast<char*>(heightStr.c_str()));
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  if (::posix_spawn_file_actions_init(&actions) != 0) {
    errMsg = "posix_spawn_file_actions_init failed";
    return 1;
  }

  // Redirect child stdout/stderr to /dev/null so logging does not interfere
  // with the parent's Teleport stdout pipe (the parent already redirected
  // these to its own descriptors, but donner-svg may still print).
  const int devNull = ::open("/dev/null", O_WRONLY | O_CLOEXEC);
  if (devNull >= 0) {
    ::posix_spawn_file_actions_adddup2(&actions, devNull, STDOUT_FILENO);
    ::posix_spawn_file_actions_adddup2(&actions, devNull, STDERR_FILENO);
    ::posix_spawn_file_actions_addclose(&actions, devNull);
  }

  pid_t pid = -1;
  const int spawnRc =
      ::posix_spawn(&pid, tool.c_str(), &actions, nullptr, argv.data(), environ);
  ::posix_spawn_file_actions_destroy(&actions);
  if (devNull >= 0) ::close(devNull);

  if (spawnRc != 0) {
    errMsg = std::string("posix_spawn(donner-svg) failed: ") +
             std::strerror(spawnRc);
    return 1;
  }

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  int status = 0;
  while (true) {
    const pid_t r = ::waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      if (WIFEXITED(status)) {
        const int code = WEXITSTATUS(status);
        if (code != 0) {
          errMsg = "donner-svg exited with code " + std::to_string(code);
          return 1;
        }
        return 0;
      }
      errMsg = "donner-svg terminated abnormally";
      return 1;
    }
    if (r < 0) {
      if (errno == EINTR) continue;
      errMsg = std::string("waitpid failed: ") + std::strerror(errno);
      return 1;
    }
    if (std::chrono::steady_clock::now() > deadline) {
      ::kill(pid, SIGKILL);
      ::waitpid(pid, &status, 0);
      errMsg = "donner-svg timed out after 10s";
      return 1;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

RenderResponse Handle(const RenderRequest& req) {
  RenderResponse resp;

  const std::string tool = ResolveDonnerSvgTool();
  const std::uint64_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
  const auto pid = ::getpid();
  const fs::path inSvg =
      fs::temp_directory_path() / ("teleport_in_" + std::to_string(pid) + "_" +
                                   std::to_string(seq) + ".svg");
  const fs::path outPng =
      fs::temp_directory_path() / ("teleport_out_" + std::to_string(pid) + "_" +
                                   std::to_string(seq) + ".png");

  std::string errMsg;
  if (!WriteFile(inSvg, req.svg_source)) {
    std::fprintf(stderr, "render_service: failed to write %s\n",
                 inSvg.c_str());
    return resp;
  }

  if (RunDonnerSvg(tool, inSvg, outPng, req.width, req.height, errMsg) != 0) {
    std::fprintf(stderr, "render_service: %s\n", errMsg.c_str());
    std::error_code ec;
    fs::remove(inSvg, ec);
    fs::remove(outPng, ec);
    return resp;
  }

  if (!ReadFileBytes(outPng, resp.png_bytes)) {
    std::fprintf(stderr, "render_service: failed to read %s\n", outPng.c_str());
    resp.png_bytes.clear();
  }

  std::error_code ec;
  fs::remove(inSvg, ec);
  fs::remove(outPng, ec);
  return resp;
}

}  // namespace

int main() {
  return donner::teleport::runService<RenderRequest, RenderResponse>(
      STDIN_FILENO, STDOUT_FILENO, Handle);
}
