#include "donner/editor/sandbox/SessionCodec.h"

#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace donner::editor::sandbox {

namespace {

void WriteLE32(std::vector<uint8_t>& buf, uint32_t v) {
  const auto offset = buf.size();
  buf.resize(offset + 4);
  std::memcpy(buf.data() + offset, &v, 4);
}

void WriteLE64(std::vector<uint8_t>& buf, uint64_t v) {
  const auto offset = buf.size();
  buf.resize(offset + 8);
  std::memcpy(buf.data() + offset, &v, 8);
}

uint32_t ReadLE32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return v;
}

uint64_t ReadLE64(const uint8_t* p) {
  uint64_t v;
  std::memcpy(&v, p, 8);
  return v;
}

/// Reads exactly `count` bytes from `fd` into `buf`, retrying on EINTR.
/// Returns false on EOF or error.
bool ReadExact(int fd, uint8_t* buf, std::size_t count) {
  std::size_t total = 0;
  while (total < count) {
    const ssize_t n = ::read(fd, buf + total, count - total);
    if (n > 0) {
      total += static_cast<std::size_t>(n);
    } else if (n == 0) {
      return false;  // EOF
    } else {
      if (errno == EINTR) continue;
      return false;  // Error
    }
  }
  return true;
}

/// Writes exactly `count` bytes from `buf` to `fd`, retrying on EINTR.
/// Returns false on error.
bool WriteExact(int fd, const uint8_t* buf, std::size_t count) {
  std::size_t total = 0;
  while (total < count) {
    const ssize_t n = ::write(fd, buf + total, count - total);
    if (n > 0) {
      total += static_cast<std::size_t>(n);
    } else if (n == 0) {
      // Shouldn't happen for write, but treat as error.
      return false;
    } else {
      if (errno == EINTR) continue;
      return false;  // EPIPE or other error
    }
  }
  return true;
}

}  // namespace

std::vector<uint8_t> EncodeFrame(const SessionFrame& frame) {
  std::vector<uint8_t> buf;
  buf.reserve(kSessionFrameHeaderSize + frame.payload.size());

  WriteLE32(buf, kSessionMagic);
  WriteLE64(buf, frame.requestId);
  WriteLE32(buf, static_cast<uint32_t>(frame.opcode));
  WriteLE32(buf, static_cast<uint32_t>(frame.payload.size()));
  buf.insert(buf.end(), frame.payload.begin(), frame.payload.end());

  return buf;
}

bool DecodeFrame(std::span<const uint8_t> bytes, SessionFrame& out, std::size_t& consumed) {
  consumed = 0;

  if (bytes.size() < kSessionFrameHeaderSize) {
    return false;  // Need more data.
  }

  const uint32_t magic = ReadLE32(bytes.data());
  if (magic != kSessionMagic) {
    consumed = SIZE_MAX;  // Protocol error.
    return false;
  }

  const uint64_t requestId = ReadLE64(bytes.data() + 4);
  const uint32_t opcode = ReadLE32(bytes.data() + 12);
  const uint32_t payloadLength = ReadLE32(bytes.data() + 16);

  if (payloadLength > kSessionMaxPayload) {
    consumed = SIZE_MAX;  // Protocol error: payload too large.
    return false;
  }

  const std::size_t totalFrameSize = kSessionFrameHeaderSize + payloadLength;
  if (bytes.size() < totalFrameSize) {
    return false;  // Need more data.
  }

  out.requestId = requestId;
  out.opcode = static_cast<SessionOpcode>(opcode);
  out.payload.assign(bytes.data() + kSessionFrameHeaderSize, bytes.data() + totalFrameSize);
  consumed = totalFrameSize;
  return true;
}

bool ReadNextFrame(int fd, SessionFrame& out, std::string& errorMessage) {
  uint8_t header[kSessionFrameHeaderSize];
  if (!ReadExact(fd, header, kSessionFrameHeaderSize)) {
    errorMessage = "EOF or read error on frame header";
    return false;
  }

  const uint32_t magic = ReadLE32(header);
  if (magic != kSessionMagic) {
    errorMessage = "bad magic: expected 0x534E5244, got 0x";
    // Hex encode the bad magic.
    char hexBuf[9];
    std::snprintf(hexBuf, sizeof(hexBuf), "%08X", magic);
    errorMessage += hexBuf;
    return false;
  }

  const uint64_t requestId = ReadLE64(header + 4);
  const uint32_t opcode = ReadLE32(header + 12);
  const uint32_t payloadLength = ReadLE32(header + 16);

  if (payloadLength > kSessionMaxPayload) {
    errorMessage = "payload length exceeds kSessionMaxPayload";
    return false;
  }

  out.requestId = requestId;
  out.opcode = static_cast<SessionOpcode>(opcode);
  out.payload.resize(payloadLength);

  if (payloadLength > 0) {
    if (!ReadExact(fd, out.payload.data(), payloadLength)) {
      errorMessage = "EOF or read error on frame payload";
      return false;
    }
  }

  return true;
}

bool WriteFrame(int fd, const SessionFrame& frame, std::string& errorMessage) {
  std::vector<uint8_t> encoded = EncodeFrame(frame);
  if (!WriteExact(fd, encoded.data(), encoded.size())) {
    errorMessage = "write error: ";
    errorMessage += std::strerror(errno);
    return false;
  }
  return true;
}

}  // namespace donner::editor::sandbox
