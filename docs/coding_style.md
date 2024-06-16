# Coding style {#CodingStyle}

As a baseline, Donner SVG aligns with the [Google C++ coding style](https://google.github.io/styleguide/cppguide.html), with modifications to more closely align with naming conventions in the SVG standard. Additionally, since Donner SVG is designed as an experiment in C++20, coding standards that exist to support backwards-compatibility with older standards may be replaced.

## Includes

Include paths for Donner files are relative to the donner svg directory and use double quotes:

```cpp
// GOOD:
#include "src/base/vector2.h"

// BAD:
#include "vector2.h"
#include <src/base/vector2.h>
```

STL and third-party dependencies do not use this, and use angle brackets:

```cpp
#include <vector>
#include <gmock/gmock.h>
```

## Naming

- **Folders**: Names are lowercase, and one word is preferred. For multiple words, use lower_snake_case.
- **Files**: lower_snake_case
- **Classes**: UpperCamelCase, matching filename. This matches the SVG standard for DOM object naming.
- **Class methods**: lowerCamelCase, aligning with the SVG standard.
  - Constructors, and constructor-like static methods continue to use UpperCamelCase.
- **Free functions**: UpperCamelCase.
- **Member variables**: lowerCamelCaseWithTrailingUnderscore_.
- **Parameters and local variables**: lowerCamelCase
- **Constants**: `k` prefix, and then UpperCamelCase: kExampleConstant

`path/to_the/example_class.h`:

```cpp
#pragma once

#include <string>

#include "src/base/math_utils.h"

class ExampleClass {
public:
  static ExampleClass Create(int inputVariable) {
    const int kConstant = 0;
    UTILS_RELEASE_ASSERT(inputVariable > kConstant);
    
    return ExampleClass();
  }

  ExampleClass() = default;

  int getMember() const {
    return someMember_;
  }

private:
  int someMember_ = 1;
};
```

## Column Limit

The column limit is set at 100.

## Formatting

The `.clang-format` file is the repo root in the source of truth for formatting.

## Tests

Tests are placed in a `tests/` directory near the file they are testing. Test files should have the suffix of `_tests.cc`.

# Code conventions

## Limit use of `auto`

`auto` should only be used when the type is visible on the same source line, or if the type is well-understood, such as for iterators (`auto it = ...`).

Exceptions:

- For `std::optional` types from function calls, assuming the code remains readable.

```cpp
if (auto maybeUnit = parseUnit(str, &charsConsumed)) {
  if (charsConsumed == str.size()) {
    return maybeUnit;
  }
}
```

- For `ParseResult<Type>` _maybeResult_ types - the type should be visible nearby, and the ParseResult pattern is ubiquitous in the codebase.

```cpp
auto maybeResult = NumberParser::Parse(remaining_);
if (maybeResult.hasError()) {
  ParseError err = std::move(maybeResult.error());
  err.offset += currentOffset();
  return err;
}

const NumberParser::Result& result = maybeResult.result();
```

Examples

```cpp
// GOOD:
```
