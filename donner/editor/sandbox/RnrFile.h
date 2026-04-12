#pragma once
/// @file
///
/// `.rnr` — **Donner Renderer Recording** — is a compact on-disk container
/// for a single sandbox wire stream plus enough metadata to reproduce the
/// original render. The file body is the raw wire bytes from
/// `SandboxHost::renderToBackend` (or `SerializingRenderer::takeBuffer`)
/// appended verbatim — no re-encoding, no compression.
///
/// Layout:
///
/// ```
///   u32   fileMagic      = 'DRNF' (0x464E5244 little-endian)
///   u32   fileVersion    = 1
///   u64   timestampNanos (unix epoch, best-effort)
///   u32   widthPixels
///   u32   heightPixels
///   u32   backendHint    (see BackendHint enum)
///   u32   uriLength
///   u8[]  uri            (UTF-8, no terminator, may be empty)
///   u8[]  wireStream     (rest of file — kStreamHeader..kEndFrame)
/// ```
///
/// The `fileVersion` is distinct from `kWireVersion` in `Wire.h`. A file
/// format bump doesn't necessarily imply a wire protocol change, and vice
/// versa — the file header wraps the wire stream, not the other way around.

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace donner::editor::sandbox {

/// Magic identifier for an `.rnr` file (`DRNF` in ASCII, little-endian u32).
inline constexpr uint32_t kRnrFileMagic = 0x464E5244u;

/// Bumped on any breaking change to the header layout. Does not need to
/// match `Wire.h`'s `kWireVersion`.
inline constexpr uint32_t kRnrFileVersion = 1;

/// Upper bound on a URI stored in a recording header. 64 KiB is plenty for
/// any real path or URL, and cheap to validate on read.
inline constexpr uint32_t kRnrMaxUriBytes = 64u * 1024u;

/// Backend that recorded the stream, for diagnostic round-tripping. The
/// replayer never acts on this — the host picks whatever backend it wants
/// to replay into — but it's useful for bug reports ("this .rnr was
/// captured with backend=tiny_skia").
enum class BackendHint : uint32_t {
  kUnspecified = 0,
  kTinySkia = 1,
  kSkia = 2,
  kGeode = 3,
};

/// Per-file metadata. All fields are POD or trivially serializable. `uri`
/// may be empty when the recording originated from an in-memory string.
struct RnrHeader {
  uint32_t fileVersion = kRnrFileVersion;
  uint64_t timestampNanos = 0;
  uint32_t width = 0;
  uint32_t height = 0;
  BackendHint backend = BackendHint::kUnspecified;
  std::string uri;
};

/// Outcome of an I/O call.
enum class RnrIoStatus {
  kOk,
  kWriteFailed,
  kReadFailed,
  kTruncated,        ///< Stream ended inside the header.
  kMagicMismatch,    ///< First four bytes aren't `DRNF`.
  kVersionMismatch,  ///< Header version newer/older than we understand.
  kUriTooLong,       ///< `uriLength` exceeds `kRnrMaxUriBytes`.
};

/// Writes `header` + `wireBytes` to the given path, creating or replacing
/// the file. `wireBytes` is copied verbatim after the header — callers
/// should pass the exact bytes returned by the sandbox or serializing
/// renderer, including the `kStreamHeader` and `kEndFrame` messages.
[[nodiscard]] RnrIoStatus SaveRnrFile(const std::filesystem::path& path,
                                      const RnrHeader& header,
                                      std::span<const uint8_t> wireBytes);

/// Reads a `.rnr` file into memory. On success, `outHeader` is populated
/// and `outWireBytes` contains the raw wire stream suitable for
/// `ReplayingRenderer::pumpFrame`.
[[nodiscard]] RnrIoStatus LoadRnrFile(const std::filesystem::path& path,
                                      RnrHeader& outHeader,
                                      std::vector<uint8_t>& outWireBytes);

/// Parses a recording from an in-memory buffer instead of disk. Same
/// semantics as `LoadRnrFile`, but without any I/O — useful for unit tests
/// and for round-tripping a recording through a memfd without hitting the
/// filesystem.
[[nodiscard]] RnrIoStatus ParseRnrBuffer(std::span<const uint8_t> buffer,
                                         RnrHeader& outHeader,
                                         std::vector<uint8_t>& outWireBytes);

/// Serializes `header` + `wireBytes` into a contiguous byte vector without
/// touching the filesystem. Always succeeds (just allocates and memcpys).
std::vector<uint8_t> EncodeRnrBuffer(const RnrHeader& header,
                                     std::span<const uint8_t> wireBytes);

}  // namespace donner::editor::sandbox
