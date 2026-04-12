#pragma once
/// @file
///
/// Milestone S2 wire format for the editor sandbox. See
/// docs/design_docs/editor_sandbox.md §"Wire format".
///
/// A wire stream is a sequence of messages: `u32 opcode`, `u32 payload_length`,
/// `u8 payload[payload_length]`. Everything is little-endian, which matches
/// every platform Donner targets today. The first message per stream is a
/// `kStreamHeader` carrying the magic + version so a reader can detect mixed
/// versions up-front.
///
/// **Every** primitive read in `WireReader` is bounds-checked against the
/// remaining buffer; every length field is capped by `kMax*` constants to
/// bound untrusted input. The reader must never crash on adversarial bytes —
/// this is the single most important invariant in the whole sandbox design.

#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace donner::editor::sandbox {

/// Magic identifier ("DRNR" as a little-endian u32).
inline constexpr uint32_t kWireMagic = 0x524E5244u;

/// Wire format version. Bumped on any payload layout change.
inline constexpr uint32_t kWireVersion = 1;

/// Hard caps the reader enforces on variable-length fields. Bounding these
/// turns "parser allocates 18 exabytes" into a graceful `kReadFailed`.
inline constexpr uint32_t kMaxVectorCount = 10'000'000;
inline constexpr uint32_t kMaxStringBytes = 10u * 1024u * 1024u;
inline constexpr uint32_t kMaxFrameBytes = 256u * 1024u * 1024u;
inline constexpr uint32_t kMaxPayloadBytes = kMaxFrameBytes;

/// Opcodes. One per supported `RendererInterface` method, plus control ops.
///
/// Values are stable across patch releases; new opcodes append at the end. Do
/// not renumber existing opcodes without bumping `kWireVersion`.
enum class Opcode : uint32_t {
  kInvalid = 0,

  /// Stream metadata. Always the first message.
  kStreamHeader = 1,

  /// Frame lifecycle.
  kBeginFrame = 10,
  kEndFrame = 11,

  /// Transform stack.
  kSetTransform = 20,
  kPushTransform = 21,
  kPopTransform = 22,

  /// Clip stack. S2 encodes rect + path shapes; masks fall through as `kUnsupported`.
  kPushClip = 30,
  kPopClip = 31,

  /// Isolated compositing layer (opacity + blend mode only).
  kPushIsolatedLayer = 40,
  kPopIsolatedLayer = 41,

  /// Paint state. S2 encodes `PaintServer::None` and `PaintServer::Solid`; any
  /// other paint-server variant (gradient, pattern, resolved reference) emits
  /// `kUnsupported` and the frame is considered lossy.
  kSetPaint = 50,

  /// Mask sub-scope. Between `kPushMask` and `kTransitionMaskToContent`
  /// the stream carries the mask's own drawing commands (what will be
  /// used to derive the alpha mask). Between `kTransitionMaskToContent`
  /// and `kPopMask` the stream carries the masked content itself.
  kPushMask = 42,
  kTransitionMaskToContent = 43,
  kPopMask = 44,

  /// Pattern tile sub-scope. Draw calls between `kBeginPatternTile` and
  /// `kEndPatternTile` are recorded into an offscreen pattern surface
  /// instead of the main framebuffer, then used as the paint source for
  /// the next draw call.
  kBeginPatternTile = 45,
  kEndPatternTile = 46,

  /// Drawing primitives.
  kDrawPath = 60,
  kDrawRect = 61,
  kDrawEllipse = 62,
  kDrawImage = 63,
  kDrawText = 64,

  /// Filter layer sub-scope (transparent pass-through in S2 — the primitive
  /// chain is not yet serialized, so the filter has no visual effect but
  /// preserves the compositing stack).
  kPushFilterLayer = 47,
  kPopFilterLayer = 48,

  /// Placeholder for any method `SerializingRenderer` can't faithfully encode
  /// (text — see `UnsupportedKind`). Payload is a single u32 identifying
  /// which kind was hit, for diagnostics.
  kUnsupported = 1000,
};

/// Tag identifying which unsupported `RendererInterface` method was skipped.
enum class UnsupportedKind : uint32_t {
  kPushFilterLayer = 1,
  kPopFilterLayer = 2,
  kPushMask = 3,
  kTransitionMaskToContent = 4,
  kPopMask = 5,
  kBeginPatternTile = 6,
  kEndPatternTile = 7,
  kDrawImage = 8,
  kDrawText = 9,
  kPaintServerGradient = 10,
  kPaintServerPattern = 11,
  kPaintServerResolvedReference = 12,
  kClipMaskChain = 13,
  kColorNonRgba = 14,
};

/// Append-only byte buffer writer. Cheap by design — a `std::vector<uint8_t>`
/// owner with a few typed helpers. No growth policy beyond the vector's.
class WireWriter {
public:
  /// Total bytes written so far.
  [[nodiscard]] std::size_t size() const { return buffer_.size(); }

  /// Returns a view of the accumulated bytes.
  [[nodiscard]] std::span<const uint8_t> data() const { return buffer_; }

  /// Releases ownership of the accumulated bytes.
  [[nodiscard]] std::vector<uint8_t> take() && { return std::move(buffer_); }

  /// @name Primitives
  /// All writes are little-endian.
  /// @{
  void writeU8(uint8_t v) { buffer_.push_back(v); }
  void writeU32(uint32_t v) { writePod(v); }
  void writeI32(int32_t v) { writePod(v); }
  void writeU64(uint64_t v) { writePod(v); }
  void writeF64(double v) { writePod(v); }
  void writeBool(bool v) { writeU8(v ? 1 : 0); }

  void writeBytes(std::span<const uint8_t> bytes) {
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
  }

  void writeString(std::string_view s) {
    writeU32(static_cast<uint32_t>(s.size()));
    buffer_.insert(buffer_.end(), s.begin(), s.end());
  }
  /// @}

  /// Writes a raw message header: `u32 opcode, u32 payload_length`. The caller
  /// is responsible for appending `payload_length` bytes immediately after.
  void writeMessageHeader(Opcode opcode, uint32_t payloadLength) {
    writeU32(static_cast<uint32_t>(opcode));
    writeU32(payloadLength);
  }

  /// Reserves a payload-length slot before the payload is encoded, returning a
  /// token the caller hands back to `finishMessage()` once the payload is
  /// complete. This avoids having to buffer the payload twice.
  struct MessageToken {
    std::size_t lengthOffset;
    std::size_t payloadStart;
  };

  [[nodiscard]] MessageToken beginMessage(Opcode opcode) {
    writeU32(static_cast<uint32_t>(opcode));
    const std::size_t lengthOffset = buffer_.size();
    writeU32(0);  // placeholder
    return MessageToken{lengthOffset, buffer_.size()};
  }

  void finishMessage(MessageToken token) {
    const std::size_t payloadBytes = buffer_.size() - token.payloadStart;
    const auto len = static_cast<uint32_t>(payloadBytes);
    std::memcpy(buffer_.data() + token.lengthOffset, &len, sizeof(len));
  }

private:
  template <typename T>
  void writePod(T value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const auto offset = buffer_.size();
    buffer_.resize(offset + sizeof(T));
    std::memcpy(buffer_.data() + offset, &value, sizeof(T));
  }

  std::vector<uint8_t> buffer_;
};

/// Read cursor into an immutable byte span. Every read that could overflow the
/// buffer returns `false` and leaves `failed_` set, so callers can do their
/// work without interleaved error checking and verify success at the end.
class WireReader {
public:
  explicit WireReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}

  [[nodiscard]] bool failed() const { return failed_; }
  [[nodiscard]] std::size_t remaining() const {
    return failed_ ? 0 : (bytes_.size() - pos_);
  }
  [[nodiscard]] std::size_t position() const { return pos_; }

  /// Manually mark the reader failed (e.g., when a higher-level invariant is
  /// violated like an unknown variant tag).
  void fail() { failed_ = true; }

  /// Seeks forward by `n` bytes, e.g. to skip over a payload the caller doesn't
  /// understand. Fails on overflow.
  [[nodiscard]] bool skip(std::size_t n) {
    if (!check(n)) return false;
    pos_ += n;
    return true;
  }

  /// @name Primitive reads
  /// @{
  [[nodiscard]] bool readU8(uint8_t& out) {
    if (!check(1)) return false;
    out = bytes_[pos_++];
    return true;
  }

  [[nodiscard]] bool readU32(uint32_t& out) { return readPod(out); }
  [[nodiscard]] bool readI32(int32_t& out) { return readPod(out); }
  [[nodiscard]] bool readU64(uint64_t& out) { return readPod(out); }
  [[nodiscard]] bool readF64(double& out) { return readPod(out); }

  [[nodiscard]] bool readBool(bool& out) {
    uint8_t v = 0;
    if (!readU8(v)) return false;
    if (v > 1) {
      failed_ = true;
      return false;
    }
    out = (v != 0);
    return true;
  }

  [[nodiscard]] bool readString(std::string& out, uint32_t maxBytes = kMaxStringBytes) {
    uint32_t len = 0;
    if (!readU32(len)) return false;
    if (len > maxBytes || !check(len)) {
      failed_ = true;
      return false;
    }
    out.assign(reinterpret_cast<const char*>(bytes_.data() + pos_), len);
    pos_ += len;
    return true;
  }

  [[nodiscard]] bool readBytes(std::span<uint8_t> out) {
    if (!check(out.size())) return false;
    std::memcpy(out.data(), bytes_.data() + pos_, out.size());
    pos_ += out.size();
    return true;
  }

  /// Reads a `u32` length field and validates it against a per-field cap
  /// before the caller allocates anything. Returns the length on success.
  [[nodiscard]] bool readCount(uint32_t& out, uint32_t maxCount) {
    if (!readU32(out)) return false;
    if (out > maxCount) {
      failed_ = true;
      return false;
    }
    return true;
  }
  /// @}

  /// Reads `u32 opcode, u32 payload_length` from the head of the buffer.
  [[nodiscard]] bool readMessageHeader(Opcode& outOpcode, uint32_t& outPayloadLength) {
    uint32_t rawOp = 0;
    if (!readU32(rawOp) || !readU32(outPayloadLength)) return false;
    if (outPayloadLength > kMaxPayloadBytes) {
      failed_ = true;
      return false;
    }
    outOpcode = static_cast<Opcode>(rawOp);
    return true;
  }

private:
  template <typename T>
  bool readPod(T& out) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (!check(sizeof(T))) return false;
    std::memcpy(&out, bytes_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    return true;
  }

  [[nodiscard]] bool check(std::size_t n) {
    if (failed_) return false;
    if (n > bytes_.size() - pos_) {
      failed_ = true;
      return false;
    }
    return true;
  }

  std::span<const uint8_t> bytes_;
  std::size_t pos_ = 0;
  bool failed_ = false;
};

}  // namespace donner::editor::sandbox
