#pragma once
/// @file
/// Reflection-driven Encode<T> / Decode<T> for the Teleport IPC spike (M0.1).
///
/// Targets the Bloomberg `clang-p2996` fork
/// (https://github.com/bloomberg/clang-p2996) implementing P2996 "Reflection
/// for C++26" plus P1306 "Expansion statements" (`template for`). Build with:
///
///     clang++ -std=c++26 -freflection-latest -fexpansion-statements ...
///
/// (Confirm flag names against the fork's current README — see SPIKE_NOTES.md
/// for the exact incantation that worked.)
///
/// Wire format (little-endian on the producer side, byte-for-byte echoed on
/// the consumer side — endianness conversion is M1 work, not M0):
///
///   * Trivially-copyable scalars (`int32_t`, `double`, …): raw bytes,
///     `sizeof(T)` wide.
///   * `std::string`: u32 length prefix, then `length` bytes of payload.
///   * Aggregate / class types: each non-static data member encoded in
///     declaration order. No type tag, no length prefix at the struct
///     level (the schema is implicit and known to both sides — schema-hash
///     handshake is M1).
///
/// All reads bounds-check against the remaining buffer. A malformed length
/// prefix (e.g. claiming more bytes than the buffer holds) yields a
/// `DecodeError` rather than a crash or out-of-bounds read; this is the
/// minimum bar to make the codec safe to feed into libFuzzer in M0.2.

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <experimental/meta>
#include <string>
#include <type_traits>
#include <vector>

namespace donner::teleport {

enum class DecodeError {
  kTruncated,         //!< Buffer ended mid-field.
  kStringTooLarge,    //!< Length prefix exceeded remaining buffer.
};

namespace detail {

class Writer {
 public:
  explicit Writer(std::vector<std::byte>& out) : out_(out) {}

  void writeBytes(const void* src, std::size_t n) {
    const auto* p = static_cast<const std::byte*>(src);
    out_.insert(out_.end(), p, p + n);
  }

  template <class T>
    requires std::is_trivially_copyable_v<T>
  void writeScalar(const T& v) {
    writeBytes(&v, sizeof(T));
  }

 private:
  std::vector<std::byte>& out_;
};

class Reader {
 public:
  Reader(const std::byte* data, std::size_t size) : data_(data), size_(size) {}

  [[nodiscard]] bool readBytes(void* dst, std::size_t n) {
    if (n > size_ - pos_) return false;
    std::memcpy(dst, data_ + pos_, n);
    pos_ += n;
    return true;
  }

  template <class T>
    requires std::is_trivially_copyable_v<T>
  [[nodiscard]] bool readScalar(T& out) {
    return readBytes(&out, sizeof(T));
  }

  [[nodiscard]] std::size_t remaining() const { return size_ - pos_; }

 private:
  const std::byte* data_;
  std::size_t size_;
  std::size_t pos_ = 0;
};

// ---------------------------------------------------------------------------
// Primitive encoders / decoders. Everything else routes through reflection.

template <class T>
inline void encodeOne(Writer& w, const T& v);

template <class T>
[[nodiscard]] inline std::expected<void, DecodeError> decodeOne(Reader& r, T& out);

// std::string: u32 length + bytes.
inline void encodeOne(Writer& w, const std::string& s) {
  const std::uint32_t len = static_cast<std::uint32_t>(s.size());
  w.writeScalar(len);
  w.writeBytes(s.data(), len);
}

inline std::expected<void, DecodeError> decodeOne(Reader& r, std::string& out) {
  std::uint32_t len = 0;
  if (!r.readScalar(len)) return std::unexpected(DecodeError::kTruncated);
  if (len > r.remaining()) return std::unexpected(DecodeError::kStringTooLarge);
  out.resize(len);
  if (len > 0 && !r.readBytes(out.data(), len)) {
    return std::unexpected(DecodeError::kTruncated);
  }
  return {};
}

// Generic fallback: trivially-copyable scalars take the fast path; everything
// else goes through reflection on its non-static data members.
template <class T>
inline void encodeOne(Writer& w, const T& v) {
  if constexpr (std::is_trivially_copyable_v<T> && !std::is_class_v<T>) {
    w.writeScalar(v);
  } else if constexpr (std::is_class_v<T>) {
    // P2996: enumerate non-static data members at compile time, splice each
    // member access at runtime. `template for` (P1306) drives the loop.
    //
    // `nonstatic_data_members_of` returns `std::vector<info>` whose heap
    // allocation cannot persist in a non-transient `constexpr` variable, so
    // `std::define_static_array` promotes it to statically-stored storage
    // (returns `std::span<const info>`). Without this wrapper clang rejects
    // the loop with "pointer to subobject of heap-allocated object is not a
    // constant expression".
    static constexpr auto members = std::define_static_array(
        std::meta::nonstatic_data_members_of(
            ^^T, std::meta::access_context::current()));
    template for (constexpr auto member : members) {
      encodeOne(w, v.[:member:]);
    }
  } else {
    static_assert(sizeof(T) == 0, "Teleport codec: unsupported type");
  }
}

template <class T>
inline std::expected<void, DecodeError> decodeOne(Reader& r, T& out) {
  if constexpr (std::is_trivially_copyable_v<T> && !std::is_class_v<T>) {
    if (!r.readScalar(out)) return std::unexpected(DecodeError::kTruncated);
    return {};
  } else if constexpr (std::is_class_v<T>) {
    static constexpr auto members = std::define_static_array(
        std::meta::nonstatic_data_members_of(
            ^^T, std::meta::access_context::current()));
    template for (constexpr auto member : members) {
      auto sub = decodeOne(r, out.[:member:]);
      if (!sub) return std::unexpected(sub.error());
    }
    return {};
  } else {
    static_assert(sizeof(T) == 0, "Teleport codec: unsupported type");
  }
}

}  // namespace detail

template <class T>
[[nodiscard]] std::vector<std::byte> Encode(const T& value) {
  std::vector<std::byte> out;
  out.reserve(64);
  detail::Writer w(out);
  detail::encodeOne(w, value);
  return out;
}

template <class T>
[[nodiscard]] std::expected<T, DecodeError> Decode(
    const std::byte* data, std::size_t size) {
  detail::Reader r(data, size);
  T value{};
  auto result = detail::decodeOne(r, value);
  if (!result) return std::unexpected(result.error());
  return value;
}

template <class T>
[[nodiscard]] std::expected<T, DecodeError> Decode(
    const std::vector<std::byte>& bytes) {
  return Decode<T>(bytes.data(), bytes.size());
}

}  // namespace donner::teleport
