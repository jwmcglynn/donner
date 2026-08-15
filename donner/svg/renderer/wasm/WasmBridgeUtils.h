#pragma once
/// @file

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>

#ifdef __EMSCRIPTEN__
#include <emscripten/heap.h>
#endif

namespace donner::svg::wasm {

/// Return whether a caller-provided byte range is contained in WebAssembly linear memory.
inline bool IsValidInputRange(const char* data, size_t size) {
#ifdef __EMSCRIPTEN__
  const uintptr_t address = reinterpret_cast<uintptr_t>(data);
  const size_t heapSize = emscripten_get_heap_size();
  return address <= heapSize && size <= heapSize - address;
#else
  (void)data;
  (void)size;
  return true;
#endif
}

/// Find a terminator without scanning beyond linear memory or the configured source limit.
inline std::optional<size_t> BoundedCStringLength(const char* data, size_t maximumInputSize) {
  size_t scanSize = maximumInputSize + 1;
#ifdef __EMSCRIPTEN__
  const uintptr_t address = reinterpret_cast<uintptr_t>(data);
  const size_t heapSize = emscripten_get_heap_size();
  if (address >= heapSize) {
    return std::nullopt;
  }
  scanSize = std::min(scanSize, heapSize - address);
#endif

  if (const void* terminator = std::memchr(data, '\0', scanSize)) {
    return static_cast<const char*>(terminator) - data;
  }
  if (scanSize == maximumInputSize + 1) {
    return scanSize;
  }
  return std::nullopt;
}

}  // namespace donner::svg::wasm
