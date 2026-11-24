#pragma once
/// @file
/// Fixed-size Bloom filters over precomputed 32-bit hashes: \ref donner::BloomHash,
/// \ref donner::BloomFilter and \ref donner::CountingBloomFilter.

#include <array>
#include <cassert>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace donner {

/**
 * A 32-bit key hash for \ref BloomFilter and \ref CountingBloomFilter. Compute it once per key
 * with \ref Of and reuse it for every insertion and probe.
 *
 * The value is a pure function of the key bytes and the salt, identical on every platform and
 * standard library, so tests and fuzzer corpora are reproducible.
 */
class BloomHash {
public:
  /**
   * Wraps a precomputed hash value.
   *
   * @param value Hash value; a filter with `2^k` slots probes bits 0 to `k-1` and `k` to `2k-1`.
   */
  constexpr explicit BloomHash(std::uint32_t value) noexcept : value_(value) {}

  /**
   * Hashes \p bytes under \p salt. The hash is 32-bit FNV-1a over the salt's four bytes in
   * little-endian order followed by \p bytes, then the MurmurHash3 `fmix32` finalizer, so that
   * every input bit affects the low bits the filters probe.
   *
   * @param bytes Key bytes; each byte is taken as an unsigned value.
   * @param salt Key kind, so that keys of different kinds with the same spelling (for example a
   *   tag name and a class name) hash differently.
   */
  static constexpr BloomHash Of(std::string_view bytes, std::uint32_t salt) noexcept {
    constexpr std::uint32_t kFnvOffsetBasis = 2166136261u;
    constexpr std::uint32_t kFnvPrime = 16777619u;

    std::uint32_t state = kFnvOffsetBasis;
    for (int shift = 0; shift < 32; shift += 8) {
      state = (state ^ ((salt >> shift) & 0xFFu)) * kFnvPrime;
    }
    for (const char byte : bytes) {
      state = (state ^ static_cast<std::uint8_t>(byte)) * kFnvPrime;
    }

    state ^= state >> 16;
    state *= 0x85EBCA6Bu;
    state ^= state >> 13;
    state *= 0xC2B2AE35u;
    state ^= state >> 16;
    return BloomHash(state);
  }

  /// Returns the hash value.
  constexpr std::uint32_t value() const noexcept { return value_; }

  /// Equality operator.
  friend constexpr bool operator==(BloomHash lhs, BloomHash rhs) noexcept = default;

private:
  std::uint32_t value_;  //!< Hash value.
};

namespace detail {

/**
 * The two probe positions both filters derive from a \ref BloomHash: bits 0 to `kLog2Slots-1` and
 * bits `kLog2Slots` to `2*kLog2Slots-1`. The ranges are disjoint, so both fit in 32 bits only for
 * at most 16 index bits.
 *
 * @tparam kLog2Slots Base-2 logarithm of the slot count.
 */
template <int kLog2Slots>
struct BloomProbes {
  static_assert(kLog2Slots >= 1 && kLog2Slots <= 16,
                "Two disjoint probe ranges of a 32-bit hash allow 1 to 16 index bits");

  /// Number of slots.
  static constexpr std::size_t kSlots = std::size_t{1} << kLog2Slots;

  /// Mask selecting one probe's index bits.
  static constexpr std::uint32_t kIndexMask = static_cast<std::uint32_t>(kSlots - 1);

  /// Returns the first probe position, from the low bits.
  static constexpr std::size_t First(BloomHash hash) noexcept { return hash.value() & kIndexMask; }

  /// Returns the second probe position, from the next `kLog2Slots` bits.
  static constexpr std::size_t Second(BloomHash hash) noexcept {
    return (hash.value() >> kLog2Slots) & kIndexMask;
  }

  /**
   * Approximates the false-positive rate after inserting \p keys distinct uniformly hashed keys
   * into `kSlots` slots with two probes each: `(1 - (1 - 1/kSlots)^(2 * keys))^2`. The estimate
   * treats the two occupancy checks as independent.
   *
   * @param keys Number of keys present.
   */
  static double ExpectedFalsePositiveRate(std::size_t keys) noexcept {
    const double slotStaysEmpty =
        std::pow(1.0 - 1.0 / static_cast<double>(kSlots), 2.0 * static_cast<double>(keys));
    const double slotOccupied = 1.0 - slotStaysEmpty;
    return slotOccupied * slotOccupied;
  }
};

}  // namespace detail

/**
 * A Bloom filter of `2^kLog2Bits` bits with two probes per key, for sets that only grow until
 * \ref clear.
 *
 * \ref mayContain never returns false for an inserted key; the false-positive rate for absent keys
 * is approximated by \ref expectedFalsePositiveRate. Sizing guidance: with 4,096 bits
 * (`kLog2Bits = 12`), 100 keys give about 0.23 percent false positives, 500 keys 4.7 percent and
 * 1,000 keys 15 percent.
 *
 * Storage is inline (`2^kLog2Bits / 8` bytes, at least 8) and every operation except \ref clear
 * takes constant time and never allocates. There is no internal synchronization: concurrent
 * \ref mayContain calls are safe while no thread modifies the filter, and any modification needs
 * exclusive access.
 *
 * @tparam kLog2Bits Base-2 logarithm of the bit count, 1 to 16.
 */
template <int kLog2Bits>
class BloomFilter {
  using Probes = detail::BloomProbes<kLog2Bits>;
  static constexpr std::size_t kBitsPerWord = 64;
  static constexpr std::size_t kWords =
      Probes::kSlots < kBitsPerWord ? 1 : Probes::kSlots / kBitsPerWord;

public:
  /// Number of bits.
  static constexpr std::size_t kBits = Probes::kSlots;

  /// Creates an empty filter.
  constexpr BloomFilter() noexcept = default;

  /**
   * Adds a key.
   *
   * @param hash The key's hash.
   */
  constexpr void insert(BloomHash hash) noexcept {
    setBit(Probes::First(hash));
    setBit(Probes::Second(hash));
  }

  /**
   * Returns false only if \p hash was not inserted since construction or the last \ref clear.
   *
   * @param hash The key's hash.
   */
  [[nodiscard]] constexpr bool mayContain(BloomHash hash) const noexcept {
    return testBit(Probes::First(hash)) && testBit(Probes::Second(hash));
  }

  /// Removes every key.
  constexpr void clear() noexcept { words_.fill(0); }

  /**
   * Approximates the false-positive rate with \p keys distinct keys inserted.
   *
   * @param keys Number of keys inserted.
   */
  static double expectedFalsePositiveRate(std::size_t keys) noexcept {
    return Probes::ExpectedFalsePositiveRate(keys);
  }

private:
  constexpr void setBit(std::size_t index) noexcept {
    words_[index / kBitsPerWord] |= std::uint64_t{1} << (index % kBitsPerWord);
  }

  constexpr bool testBit(std::size_t index) const noexcept {
    return (words_[index / kBitsPerWord] >> (index % kBitsPerWord)) & 1u;
  }

  std::array<std::uint64_t, kWords> words_{};  //!< Bit array, bit `i` in word `i / 64`.
};

/**
 * A counting Bloom filter of `2^kLog2Slots` 8-bit saturating counters with two probes per key,
 * which supports \ref remove.
 *
 * Invariant: after any sequence of operations in which every `remove(h)` follows a matching
 * `insert(h)`, `mayContain(h)` is true for every `h` inserted more times than removed. A counter
 * that reaches 255 stays at 255: \ref insert does not wrap it and \ref remove never decrements it,
 * so removal cannot create a false negative, at the cost of a false positive on that counter
 * until \ref clear. Sizing guidance matches \ref BloomFilter, counting the keys present: with
 * 4,096 counters (`kLog2Slots = 12`, 4 KiB), 100 keys give about 0.23 percent false positives.
 *
 * Storage is inline (`2^kLog2Slots` bytes) and every operation except \ref clear takes constant
 * time and never allocates. There is no internal synchronization: concurrent \ref mayContain
 * calls are safe while no thread modifies the filter, and any modification needs exclusive
 * access.
 *
 * @tparam kLog2Slots Base-2 logarithm of the counter count, 1 to 16.
 */
template <int kLog2Slots>
class CountingBloomFilter {
  using Probes = detail::BloomProbes<kLog2Slots>;

public:
  /// Number of counters.
  static constexpr std::size_t kSlots = Probes::kSlots;

  /// Counter value that is never decremented.
  static constexpr std::uint8_t kSaturated = 255;

  /// Creates an empty filter.
  constexpr CountingBloomFilter() noexcept = default;

  /**
   * Adds one occurrence of a key.
   *
   * @param hash The key's hash.
   * @return True if either probed counter is saturated after this insertion.
   */
  constexpr bool insert(BloomHash hash) noexcept {
    const bool firstSaturated = increment(Probes::First(hash));
    const bool secondSaturated = increment(Probes::Second(hash));
    return firstSaturated || secondSaturated;
  }

  /**
   * Removes one occurrence of a key. The key must have been inserted more times than removed
   * since construction or the last \ref clear.
   *
   * @param hash The key's hash.
   */
  constexpr void remove(BloomHash hash) noexcept {
    decrement(Probes::First(hash));
    decrement(Probes::Second(hash));
  }

  /**
   * Returns false only if \p hash has no occurrence left since construction or the last
   * \ref clear.
   *
   * @param hash The key's hash.
   */
  [[nodiscard]] constexpr bool mayContain(BloomHash hash) const noexcept {
    return counters_[Probes::First(hash)] != 0 && counters_[Probes::Second(hash)] != 0;
  }

  /// Removes every key and resets saturated counters.
  constexpr void clear() noexcept { counters_.fill(0); }

  /**
   * Approximates the false-positive rate with \p keys distinct keys present, assuming no
   * counters remain saturated after removals.
   *
   * @param keys Number of keys present.
   */
  static double expectedFalsePositiveRate(std::size_t keys) noexcept {
    return Probes::ExpectedFalsePositiveRate(keys);
  }

private:
  constexpr bool increment(std::size_t index) noexcept {
    std::uint8_t& counter = counters_[index];
    if (counter != kSaturated) {
      ++counter;
    }
    return counter == kSaturated;
  }

  constexpr void decrement(std::size_t index) noexcept {
    assert(counters_[index] != 0 && "remove() without a matching insert()");
    if (counters_[index] != kSaturated) {
      --counters_[index];
    }
  }

  std::array<std::uint8_t, kSlots> counters_{};  //!< One saturating counter per slot.
};

}  // namespace donner
