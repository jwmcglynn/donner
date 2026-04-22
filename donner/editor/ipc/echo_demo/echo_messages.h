#pragma once
/// @file

#include <string>

namespace donner::teleport::echo_demo {

/// Request payload for the Teleport M1 uppercase echo demo.
struct EchoRequest {
  std::string message;  //!< String echoed back in uppercase.
};

/// Response payload for the Teleport M1 uppercase echo demo.
struct EchoResponse {
  std::string uppercased;  //!< Uppercased form of \ref EchoRequest::message.
};

}  // namespace donner::teleport::echo_demo
