# CompileTimeMap usage examples

This guide shows how to build constexpr lookup tables with `CompileTimeMap` and
reuse them across translation units.

## Quickstart

The primary API is `makeCompileTimeMap`, which creates a compile-time map with automatic error checking:

```cpp
#include "donner/base/CompileTimeMap.h"

using namespace std::string_view_literals;

namespace {
static constexpr auto kMimeTypes = makeCompileTimeMap(
    std::to_array<std::pair<std::string_view, std::string_view>>({
        {"svg"sv, "image/svg+xml"sv},
        {"png"sv, "image/png"sv},
        {"jpg"sv, "image/jpeg"sv},
    }));
}  // namespace

void Serve(std::string_view extension) {
  if (auto const* value = kMimeTypes.find(extension)) {
    SendHeader(*value);
    return;
  }
  SendNotFound();
}
```

Key points:
- Construction happens at compile time; lookups are zero-allocation and use perfect hashing
- Automatically checks for construction errors at compile time (fails compilation on duplicate keys)
- `find` returns `nullptr` on misses, while `at` asserts if the key is absent
- Keys/values are stored by value; prefer `std::string_view` or enums for lightweight copies


## Enum keys

```cpp
enum class TokenKind { kStart, kEnd, kData };

constexpr auto kTokenNames = donner::makeCompileTimeMap(
    std::array{
        std::pair{TokenKind::kStart, std::string_view{"start"}},
        std::pair{TokenKind::kEnd, std::string_view{"end"}},
        std::pair{TokenKind::kData, std::string_view{"data"}},
    });

constexpr std::string_view ToString(TokenKind kind) {
  return kTokenNames.at(kind);
}
```

The default hasher supports enums; custom hash/equality functors can be provided as
optional template parameters when needed.

## Migration tips

- Replace static lookup helpers by lifting the existing key/value pairs into a
  `std::array` and passing them to `makeCompileTimeMap`.
- Prefer `std::string_view` keys when the source data is immutable; `RcString` and
  integral keys are also supported.
- Keep maps in an anonymous namespace when they are only needed within a translation
  unit; expose them via `constexpr` accessors when sharing between components.
