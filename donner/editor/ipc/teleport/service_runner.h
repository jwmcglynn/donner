#pragma once
/// @file

#include <cstddef>
#include <expected>
#include <utility>

#include "donner/editor/ipc/spike/teleport_codec.h"
#include "donner/editor/ipc/teleport/transport.h"

namespace donner::teleport {

/**
 * Runs a synchronous request/response Teleport service loop on two pipe file
 * descriptors.
 *
 * @param inFd Request stream (typically the child's stdin).
 * @param outFd Response stream (typically the child's stdout).
 * @param handler Functor invoked for every decoded request.
 * @tparam Request Request message type encoded with \ref Encode.
 * @tparam Response Response message type encoded with \ref Encode.
 * @tparam Handler Callable taking `const Request&` and returning `Response`.
 * @return `0` on clean EOF at the next frame boundary, non-zero on any
 *     transport or decode failure.
 */
template <class Request, class Response, class Handler>
int runService(int inFd, int outFd, Handler&& handler) {
  PipeReader reader;
  PipeWriter writer;

  while (true) {
    auto requestFrame = reader.readFrame(inFd);
    if (!requestFrame) {
      return requestFrame.error() == TransportError::kEof ? 0 : 1;
    }

    auto request = Decode<Request>(*requestFrame);
    if (!request) {
      return 1;
    }

    Response response = handler(request.value());
    const auto responseFrame = Encode<Response>(response);
    auto writeResult = writer.writeFrame(outFd, responseFrame);
    if (!writeResult) {
      return 1;
    }
  }
}

}  // namespace donner::teleport
