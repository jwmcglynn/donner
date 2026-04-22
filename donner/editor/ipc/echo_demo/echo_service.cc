#include <unistd.h>

#include <cctype>
#include <string>
#include <string_view>

#include "donner/editor/ipc/echo_demo/echo_messages.h"
#include "donner/editor/ipc/teleport/service_runner.h"

namespace {

std::string UppercaseAscii(std::string_view input) {
  std::string result(input);
  for (char& ch : result) {
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  }

  return result;
}

}  // namespace

int main() {
  return donner::teleport::runService<donner::teleport::echo_demo::EchoRequest,
                                      donner::teleport::echo_demo::EchoResponse>(
      STDIN_FILENO, STDOUT_FILENO, [](const donner::teleport::echo_demo::EchoRequest& request) {
        return donner::teleport::echo_demo::EchoResponse{
            .uppercased = UppercaseAscii(request.message),
        };
      });
}
