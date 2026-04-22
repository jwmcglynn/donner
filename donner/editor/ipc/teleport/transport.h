#pragma once
/// @file

#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <ostream>
#include <span>
#include <vector>

namespace donner::teleport {

/// Transport-level framing failures for the minimal Teleport pipe transport.
enum class TransportError {
  kEof,            //!< Clean EOF at a frame boundary.
  kShortRead,      //!< The peer closed or faulted mid-frame.
  kShortWrite,     //!< The frame could not be fully written.
  kFrameTooLarge,  //!< The advertised frame exceeded the hard safety cap.
};

/// Streams \ref TransportError values in human-readable form.
inline std::ostream& operator<<(std::ostream& os, TransportError error) {
  switch (error) {
    case TransportError::kEof: return os << "kEof";
    case TransportError::kShortRead: return os << "kShortRead";
    case TransportError::kShortWrite: return os << "kShortWrite";
    case TransportError::kFrameTooLarge: return os << "kFrameTooLarge";
  }

  return os << "TransportError(" << static_cast<int>(error) << ")";
}

/// Hard cap for a single Teleport frame body.
inline constexpr std::size_t kMaxFrameSizeBytes = 64u * 1024u * 1024u;

namespace detail {

inline std::array<std::byte, sizeof(std::uint32_t)> EncodeFrameSize(std::uint32_t size) {
  return {
      static_cast<std::byte>(size & 0xffu),
      static_cast<std::byte>((size >> 8) & 0xffu),
      static_cast<std::byte>((size >> 16) & 0xffu),
      static_cast<std::byte>((size >> 24) & 0xffu),
  };
}

inline std::uint32_t DecodeFrameSize(const std::array<std::byte, sizeof(std::uint32_t)>& bytes) {
  return static_cast<std::uint32_t>(bytes[0]) | (static_cast<std::uint32_t>(bytes[1]) << 8) |
         (static_cast<std::uint32_t>(bytes[2]) << 16) |
         (static_cast<std::uint32_t>(bytes[3]) << 24);
}

inline std::expected<void, TransportError> ReadExact(int fd, std::byte* dst, std::size_t size,
                                                     bool allowCleanEof) {
  std::size_t totalRead = 0;
  while (totalRead < size) {
    const ssize_t readSize = ::read(fd, dst + totalRead, size - totalRead);
    if (readSize > 0) {
      totalRead += static_cast<std::size_t>(readSize);
      continue;
    }
    if (readSize == 0) {
      if (allowCleanEof && totalRead == 0) {
        return std::unexpected(TransportError::kEof);
      }
      return std::unexpected(TransportError::kShortRead);
    }
    if (errno == EINTR) {
      continue;
    }
    return std::unexpected(TransportError::kShortRead);
  }

  return {};
}

inline std::expected<void, TransportError> WriteAll(int fd, std::span<const std::byte> bytes) {
  std::size_t totalWritten = 0;
  while (totalWritten < bytes.size()) {
    const ssize_t writeSize = ::write(fd, bytes.data() + totalWritten, bytes.size() - totalWritten);
    if (writeSize > 0) {
      totalWritten += static_cast<std::size_t>(writeSize);
      continue;
    }
    if (writeSize < 0 && errno == EINTR) {
      continue;
    }
    return std::unexpected(TransportError::kShortWrite);
  }

  return {};
}

}  // namespace detail

/// Reads length-prefixed frames from a byte stream.
class PipeReader {
public:
  /**
   * Reads a single frame body from \p fd.
   *
   * @param fd Source file descriptor.
   * @return Frame bytes on success, or a \ref TransportError on EOF or a
   *     malformed short frame.
   */
  [[nodiscard]] std::expected<std::vector<std::byte>, TransportError> readFrame(int fd) const {
    std::array<std::byte, sizeof(std::uint32_t)> sizeBytes{};
    auto sizeRead =
        detail::ReadExact(fd, sizeBytes.data(), sizeBytes.size(), /*allowCleanEof=*/true);
    if (!sizeRead) {
      return std::unexpected(sizeRead.error());
    }

    const std::uint32_t frameSize = detail::DecodeFrameSize(sizeBytes);
    if (frameSize > kMaxFrameSizeBytes) {
      return std::unexpected(TransportError::kFrameTooLarge);
    }

    std::vector<std::byte> frame(frameSize);
    if (frame.empty()) {
      return frame;
    }

    auto payloadRead = detail::ReadExact(fd, frame.data(), frame.size(), /*allowCleanEof=*/false);
    if (!payloadRead) {
      return std::unexpected(payloadRead.error());
    }

    return frame;
  }
};

/// Writes length-prefixed frames to a byte stream.
class PipeWriter {
public:
  /**
   * Writes a single frame body to \p fd.
   *
   * @param fd Destination file descriptor.
   * @param frame Frame payload bytes.
   * @return Empty success or a \ref TransportError when the write fails.
   */
  [[nodiscard]] std::expected<void, TransportError> writeFrame(
      int fd, std::span<const std::byte> frame) const {
    if (frame.size() > kMaxFrameSizeBytes) {
      return std::unexpected(TransportError::kFrameTooLarge);
    }

    const auto sizeBytes = detail::EncodeFrameSize(static_cast<std::uint32_t>(frame.size()));
    auto sizeWrite = detail::WriteAll(fd, sizeBytes);
    if (!sizeWrite) {
      return std::unexpected(sizeWrite.error());
    }

    return detail::WriteAll(fd, frame);
  }
};

}  // namespace donner::teleport
