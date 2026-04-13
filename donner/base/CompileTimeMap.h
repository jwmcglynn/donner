#pragma once
/// @file CompileTimeMap.h
/// Defines a constexpr-friendly associative container built on top of a
/// perfect-hash layout for fixed key sets.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>

#include "donner/base/Utils.h"

namespace donner {

static constexpr std::uint32_t kEmptySlot = std::numeric_limits<std::uint32_t>::max();
static constexpr std::uint32_t kDirectSlotLimit = kEmptySlot / 2U;
static constexpr std::uint32_t kMaxSeedSearch = 1024U;

/// Mix a base hash value with a seed to produce a new hash. Used to construct
/// perfect-hash bucket seeds during CompileTimeMap construction.
///
/// Uses uint64_t explicitly to ensure correct behavior on 32-bit platforms (e.g., WASM)
/// where std::size_t is 32 bits and the >> 33 shift would be undefined behavior.
constexpr uint64_t mixHash(uint64_t baseHash, std::uint32_t seed) {
  const uint64_t seedMix = static_cast<uint64_t>(seed) * 0x9e3779b97f4a7c15ULL;
  uint64_t value = baseHash ^ seedMix;
  value ^= (value >> 33);
  value *= 0xff51afd7ed558ccdULL;
  value ^= (value >> 33);
  value *= 0xc4ceb9fe1a85ec53ULL;
  value ^= (value >> 33);
  return value;
}

/// Returns true when the provided key type can be hashed in a constexpr
/// context by \ref constexprHashValue.
template <typename Key>
constexpr bool supportsConstexprHash() {
  if constexpr (std::is_integral_v<Key>) {
    return true;
  }
  if constexpr (std::is_enum_v<Key>) {
    return true;
  }
  if constexpr (std::is_same_v<std::remove_cvref_t<Key>, std::string_view>) {
    return true;
  }
  return false;
}

/// Compute a constexpr-friendly hash of \p key for supported key types.
///
/// Returns uint64_t to ensure consistent hashing on both 32-bit and 64-bit platforms.
template <typename Key>
constexpr uint64_t constexprHashValue(const Key& key) {
  if constexpr (std::is_integral_v<Key>) {
    constexpr uint64_t kPrime = 0x9e3779b97f4a7c15ULL;
    return static_cast<uint64_t>(key) * kPrime;
  }
  if constexpr (std::is_enum_v<Key>) {
    using Underlying = std::underlying_type_t<Key>;
    return constexprHashValue(static_cast<Underlying>(key));
  }
  if constexpr (std::is_same_v<std::remove_cvref_t<Key>, std::string_view>) {
    uint64_t value = 14695981039346656037ULL;
    for (char ch : key) {
      value ^= static_cast<unsigned char>(ch);
      value *= 1099511628211ULL;
    }
    return value;
  }
  return 0;
}

/// Perfect-hash metadata used to resolve keys into storage slots.
template <std::size_t N>
struct CompileTimeMapTables {
  /// First-level table storing direct indices or bucket seeds.
  std::array<std::uint32_t, N> primary{};
  /// Secondary slot table addressed with the bucket seed and key hash.
  std::array<std::uint32_t, N> secondary{};
  /// Number of buckets used by the first-level table; zero enables fallback lookup.
  std::uint32_t bucketCount = 0;
};

/// Indicates the result of building a CompileTimeMap.
enum class CompileTimeMapStatus {
  /// Perfect-hash tables were constructed successfully.
  kOk,
  /// Map is available but using the linear fallback path instead of perfect hashing.
  kUsingFallbackHash,
  /// Duplicate keys were detected in the input payload.
  kDuplicateKey,
  /// Perfect-hash seed search failed; map is available via fallback lookup.
  kSeedSearchFailed,
  /// Compile-time hashing is unsupported for this key type when evaluated constexpr.
  kConstexprHashUnsupported,
};

/// Diagnostics describing how a CompileTimeMap was constructed.
struct CompileTimeMapDiagnostics {
  /// Total seed attempts across all buckets.
  std::uint32_t seedAttempts = 0;
  /// Largest bucket size observed while building the table.
  std::uint32_t maxBucketSize = 0;
  /// Index of the bucket that failed to place, or kEmptySlot when successful.
  std::uint32_t failedBucket = kEmptySlot;
  /// Whether constexpr hashing was available for the provided key type.
  bool constexprHashSupported = true;
};

/// Compile-time associative container backed by a perfect hash layout.
template <typename Key, typename Value, std::size_t N, typename Hasher = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
class CompileTimeMap {
public:
  /// @name STL-compatible typedefs
  /// @{
  using key_type = Key;  ///< Type of the keys in the map.
  using mapped_type = Value;  ///< Type of the mapped values.
  using value_type = std::pair<Key, Value>;  ///< Key/value pair type.
  using hasher = Hasher;  ///< Hasher type used to hash keys.
  using key_equal = KeyEqual;  ///< Key equality comparator type.
  using size_type = std::size_t;  ///< Size type.
  /// @}

  /// Number of entries stored in the map.
  static constexpr size_type kSize = N;
  static_assert(kSize > 0, "CompileTimeMap requires at least one element.");

  /// Constructs a CompileTimeMap from precomputed tables and key/value arrays.
  constexpr CompileTimeMap(const std::array<Key, N>& keys, const std::array<Value, N>& values,
                           CompileTimeMapTables<N> tables, Hasher hasher = Hasher{},
                           KeyEqual keyEqual = KeyEqual{})
      : keys_(keys), values_(values), tables_(tables), hasher_(hasher), keyEqual_(keyEqual) {}

  /// Returns the number of entries tracked by the map.
  constexpr size_type size() const { return kSize; }

  /// Returns true when the map contains no elements.
  constexpr bool empty() const { return kSize == 0; }

  /// Returns a pointer to the mapped value when the key exists, or nullptr otherwise.
  constexpr const Value* find(const Key& key) const {
    const std::optional<size_type> index = lookupIndex(key);
    if (!index.has_value()) {
      return nullptr;
    }
    return &values_[*index];
  }

  /// Returns true when the map contains the provided key.
  constexpr bool contains(const Key& key) const { return find(key) != nullptr; }

  /// Returns a reference to the mapped value or triggers a release assertion if missing.
  constexpr const Value& at(const Key& key) const {
    const Value* const result = find(key);
    UTILS_RELEASE_ASSERT(result != nullptr);
    return *result;
  }

  /// Returns the compile-time key array, preserving insertion order.
  constexpr const std::array<Key, N>& keys() const { return keys_; }

  /// Returns the perfect-hash tables used for lookup.
  constexpr CompileTimeMapTables<N> tables() const { return tables_; }

private:
  constexpr std::optional<size_type> lookupIndex(const Key& key) const {
    if (tables_.bucketCount == 0U) {
      return fallbackLookup(key);
    }

    const size_type bucket = bucketIndex(key);
    if (bucket >= tables_.bucketCount) {
      return std::nullopt;
    }

    const std::uint32_t seedOrIndex = tables_.primary[bucket];
    if (seedOrIndex == kEmptySlot) {
      return std::nullopt;
    }

    if (seedOrIndex < kDirectSlotLimit) {
      return confirmMatch(seedOrIndex, key);
    }

    const std::uint32_t seed = seedOrIndex - kDirectSlotLimit;
    const size_type slotIndex = secondaryIndex(seed, key);
    if (slotIndex >= tables_.secondary.size()) {
      return std::nullopt;
    }
    return confirmMatch(tables_.secondary[slotIndex], key);
  }

  constexpr std::optional<size_type> confirmMatch(size_type index, const Key& key) const {
    if (index >= kSize) {
      return std::nullopt;
    }
    if (keyEqual_(keys_[index], key)) {
      return index;
    }
    return std::nullopt;
  }

  constexpr std::optional<size_type> fallbackLookup(const Key& key) const {
    for (size_type i = 0; i < kSize; ++i) {
      if (keyEqual_(keys_[i], key)) {
        return i;
      }
    }
    return std::nullopt;
  }

  constexpr size_type bucketIndex(const Key& key) const {
    return static_cast<size_type>(hashKey(key) % tables_.bucketCount);
  }

  constexpr size_type secondaryIndex(std::uint32_t seed, const Key& key) const {
    return static_cast<size_type>(mixHash(hashKey(key), seed) % kSize);
  }

  constexpr uint64_t hashKey(const Key& key) const {
    if constexpr (supportsConstexprHash<Key>()) {
      return constexprHashValue(key);
    }
    return static_cast<uint64_t>(hasher_(key));
  }

  std::array<Key, N> keys_{};
  std::array<Value, N> values_{};
  CompileTimeMapTables<N> tables_{};
  Hasher hasher_{};
  KeyEqual keyEqual_{};
};

/// Returns true when the provided keys contain duplicates.
template <typename Key, std::size_t N, typename KeyEqual>
constexpr bool hasDuplicateKeys(const std::array<Key, N>& keys, KeyEqual keyEqual) {
  for (std::size_t i = 0; i < N; ++i) {
    for (std::size_t j = i + 1; j < N; ++j) {
      if (keyEqual(keys[i], keys[j])) {
        return true;
      }
    }
  }
  return false;
}

namespace detail {

/// Contains the constructed map and associated build status.
template <typename Key, typename Value, std::size_t N, typename Hasher = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
struct CompileTimeMapResult {
  /// The constructed map instance.
  CompileTimeMap<Key, Value, N, Hasher, KeyEqual> map;
  /// Status describing how the map was built.
  CompileTimeMapStatus status;
  /// Diagnostics collected during map construction.
  CompileTimeMapDiagnostics diagnostics;
};

/// Builds a CompileTimeMap from an array of key/value pairs with full diagnostics.
/// Most users should use the wrapper `makeCompileTimeMap` instead.
template <typename Key, typename Value, std::size_t N, typename Hasher = std::hash<Key>,
          typename KeyEqual = std::equal_to<Key>>
constexpr CompileTimeMapResult<Key, Value, N, Hasher, KeyEqual> makeCompileTimeMapWithDiagnostics(
    const std::array<std::pair<Key, Value>, N>& entries, Hasher hasher = Hasher{},
    KeyEqual keyEqual = KeyEqual{}) {
  const auto keys = [&entries]<std::size_t... I>(std::index_sequence<I...>) constexpr {
    return std::array<Key, N>{entries[I].first...};
  }(std::make_index_sequence<N>{});

  const auto values = [&entries]<std::size_t... I>(std::index_sequence<I...>) constexpr {
    return std::array<Value, N>{entries[I].second...};
  }(std::make_index_sequence<N>{});

  const bool duplicateKeys = hasDuplicateKeys(keys, keyEqual);
  CompileTimeMapStatus status =
      duplicateKeys ? CompileTimeMapStatus::kDuplicateKey : CompileTimeMapStatus::kOk;
  CompileTimeMapDiagnostics diagnostics{};

  auto dispatchHash = [&](const Key& key) constexpr -> uint64_t {
    if constexpr (supportsConstexprHash<Key>()) {
      return constexprHashValue(key);
    }
    return static_cast<uint64_t>(hasher(key));
  };

  auto buildTables = [&]() constexpr -> CompileTimeMapTables<N> {
    CompileTimeMapTables<N> tables{};
    tables.primary.fill(kEmptySlot);
    tables.secondary.fill(kEmptySlot);
    tables.bucketCount = N;

    if (std::is_constant_evaluated() && !supportsConstexprHash<Key>()) {
      diagnostics.constexprHashSupported = false;
      tables.bucketCount = 0;
      return tables;
    }

    std::array<std::size_t, N> bucketCounts{};
    for (std::size_t i = 0; i < N; ++i) {
      const std::size_t bucket = static_cast<std::size_t>(dispatchHash(keys[i]) % tables.bucketCount);
      ++bucketCounts[bucket];
      diagnostics.maxBucketSize =
          std::max(diagnostics.maxBucketSize, static_cast<std::uint32_t>(bucketCounts[bucket]));
    }

    std::array<std::size_t, N> bucketOffsets{};
    std::size_t runningOffset = 0;
    for (std::size_t bucket = 0; bucket < N; ++bucket) {
      bucketOffsets[bucket] = runningOffset;
      runningOffset += bucketCounts[bucket];
    }

    std::array<std::size_t, N> bucketFill{};
    std::array<std::size_t, N> bucketItems{};
    for (std::size_t i = 0; i < N; ++i) {
      const std::size_t bucket = static_cast<std::size_t>(dispatchHash(keys[i]) % tables.bucketCount);
      const std::size_t position = bucketOffsets[bucket] + bucketFill[bucket];
      bucketItems[position] = i;
      ++bucketFill[bucket];
    }

    std::array<std::size_t, N> bucketOrder{};
    for (std::size_t i = 0; i < N; ++i) {
      bucketOrder[i] = i;
    }
    for (std::size_t i = 0; i < N; ++i) {
      std::size_t maxIndex = i;
      for (std::size_t j = i + 1; j < N; ++j) {
        if (bucketCounts[bucketOrder[j]] > bucketCounts[bucketOrder[maxIndex]]) {
          maxIndex = j;
        }
      }
      const std::size_t temp = bucketOrder[i];
      bucketOrder[i] = bucketOrder[maxIndex];
      bucketOrder[maxIndex] = temp;
    }

    std::array<bool, N> usedSlots{};
    for (std::size_t orderIndex = 0; orderIndex < N; ++orderIndex) {
      const std::size_t bucket = bucketOrder[orderIndex];
      const std::size_t count = bucketCounts[bucket];
      if (count == 0) {
        continue;
      }

      if (count == 1) {
        const std::size_t keyIndex = bucketItems[bucketOffsets[bucket]];
        tables.primary[bucket] = static_cast<std::uint32_t>(keyIndex);
        continue;
      }

      const std::size_t offset = bucketOffsets[bucket];
      std::array<std::size_t, N> candidateSlots{};
      bool placed = false;

      for (std::uint32_t seed = 1; seed <= kMaxSeedSearch; ++seed) {
        ++diagnostics.seedAttempts;
        bool collision = false;
        for (std::size_t i = 0; i < count; ++i) {
          const std::size_t keyIndex = bucketItems[offset + i];
          const std::size_t slot = static_cast<std::size_t>(mixHash(dispatchHash(keys[keyIndex]), seed) % N);
          for (std::size_t j = 0; j < i; ++j) {
            if (candidateSlots[offset + j] == slot) {
              collision = true;
              break;
            }
          }
          if (collision || usedSlots[slot]) {
            collision = true;
            break;
          }
          candidateSlots[offset + i] = slot;
        }

        if (collision) {
          continue;
        }

        for (std::size_t i = 0; i < count; ++i) {
          const std::size_t keyIndex = bucketItems[offset + i];
          const std::size_t slot = candidateSlots[offset + i];
          tables.secondary[slot] = static_cast<std::uint32_t>(keyIndex);
          usedSlots[slot] = true;
        }
        tables.primary[bucket] = kDirectSlotLimit + seed;
        placed = true;
        break;
      }

      if (!placed) {
        diagnostics.failedBucket = static_cast<std::uint32_t>(bucket);
        tables.bucketCount = 0;
        break;
      }
    }

    return tables;
  };

  CompileTimeMapTables<N> tables = duplicateKeys ? CompileTimeMapTables<N>{} : buildTables();
  if (duplicateKeys) {
    tables.bucketCount = 0;
  } else if (tables.bucketCount == 0) {
    if (!diagnostics.constexprHashSupported) {
      status = CompileTimeMapStatus::kConstexprHashUnsupported;
    } else if (diagnostics.failedBucket != kEmptySlot) {
      status = CompileTimeMapStatus::kSeedSearchFailed;
    } else {
      status = CompileTimeMapStatus::kUsingFallbackHash;
    }
  }

  return {CompileTimeMap<Key, Value, N, Hasher, KeyEqual>(keys, values, tables, hasher, keyEqual),
          status, diagnostics};
}

}  // namespace detail

/**
 * Primary API for building a CompileTimeMap with compile-time error checking.
 *
 * Usage:
 * ```
 * static DONNER_CONSTEXPR_MAP auto kColors = makeCompileTimeMap(std::to_array<std::pair<...>>({
 *     {"key1"sv, value1},
 *     {"key2"sv, value2},
 * ```
 */
///   }));

#ifdef __EMSCRIPTEN__
// Emscripten's default constexpr step limit (~1M) is too low for large maps (Color.cc has 149
// entries, PropertyRegistry.cc has 71+43). Since wasm_cc_binary transitions don't propagate
// -fconstexpr-steps, degrade to const (runtime init) on WASM builds. The static_assert still
// fires on native builds.
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define DONNER_CONSTEXPR_MAP const
#define makeCompileTimeMap(...)                                                  \
  []() {                                                                        \
    const auto _compiletime_map_result =                                        \
        ::donner::detail::makeCompileTimeMapWithDiagnostics(__VA_ARGS__);       \
    return _compiletime_map_result.map;                                         \
  }()
// NOLINTEND(cppcoreguidelines-macro-usage)
#else
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define DONNER_CONSTEXPR_MAP constexpr
#define makeCompileTimeMap(...)                                                          \
  []() constexpr {                                                                       \
    constexpr auto _compiletime_map_result =                                             \
        ::donner::detail::makeCompileTimeMapWithDiagnostics(__VA_ARGS__);                \
    static_assert(_compiletime_map_result.status == ::donner::CompileTimeMapStatus::kOk, \
                  "CompileTimeMap construction failed. Check for duplicate keys or use " \
                  "detail::makeCompileTimeMapWithDiagnostics for diagnostics.");         \
    return _compiletime_map_result.map;                                                  \
  }()
// NOLINTEND(cppcoreguidelines-macro-usage)
#endif

}  // namespace donner
