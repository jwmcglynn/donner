# Coding style {#CodingStyle}

\tableofcontents

As a baseline, Donner SVG aligns with the [Google C++ coding style](https://google.github.io/styleguide/cppguide.html), with modifications to more closely align with naming conventions in the SVG standard. Additionally, since Donner SVG is designed as an experiment in C++20, coding standards that exist to support backwards-compatibility with older standards may be replaced.

## Files

### Naming

- **Folders**: Names are lowercase, and one word is preferred. For multiple words, use lower_snake_case.
- **Files**: UpperCamelCase, matching the C++ class in the file contents, i.e. `PathSpline.cc` or `SVGPathElement.h`.
  - The filename should match the principal class or struct in the file.
  - Use the `.cc` extension for source files, and `.h` for header files.
  - Use the `_tests.cc` suffix for test files, and `_fuzzer.cc` for fuzzing files.
  - NOTE: Main files may not follow these patterns, e.g. `debugger_main.h`.

Examples

- `donner/base/Vector2.h`
- `donner/svg/SVGPathElement.h`

### Header files

- Use `#pragma once` for header guards.
- Use `/// @file` at the top of the file, and optionally provide a brief description of the file contents.

```cpp
#pragma once
/// @file
// Rest of the file content...
```

- All code is under the `donner` namespace, with sub-namespaces for different components, e.g. `donner::svg`.

```cpp
namespace donner::svg {

// Code here

}  // namespace donner::svg
```

## Documentation

1. **Struct and class documentation**:
   - Use `/**` style multi-line comments for detailed descriptions.
   - Provide comprehensive explanations, offering new details that are not immediately obvious from the code. Include usage context and examples where relevant.

   ```cpp
   /**
    * Container for a spline, which is a series of points connected by lines and curves.
    *
    * This is used to represent the `d` attribute of the \ref SVGPathElement (\ref xml_path), see
    * https://www.w3.org/TR/SVG2/paths.html#PathData. To parse SVG path data into a PathSpline, use the
    * \ref PathParser.
    */
   class PathSpline {
     // Class implementation
   };
   ```

2. **Member documentation**:

   - Use `//!<` for inline member documentation, or `/**` for long descriptions.
   - For struct members, either provide brief single-line descriptions or detailed explanations if relevant.

   ```cpp
   struct Command {
     CommandType type;   //!< Type of command.
     size_t pointIndex;  //!< Index of the first point used by this command.
   };
   ```

   ```cpp
   enum class ClipPathUnits {
    // ...
    /**
    * The clip path is defined in object space, where (0, 0) is the top-left corner of the element
    * that references the clip path, and (1, 1) is the bottom-right corner. Note that this may result
    * in non-uniform scaling if the element is not square.
    */
    ObjectBoundingBox,
    // ...
   };
   ```

3. **Method documentation**:

   - Use `/**` style multi-line comments for methods.
   - Omit the `@brief` prefix; the first line will be the brief by default.
   - Describe the purpose and behavior of the method.
   - Mention return values when relevant, but don't always use explicit `@return` tags.

   ```cpp
   /**
    * Get a point on the spline.
    *
    * @param index Spline index.
    * @param t Position on spline, between 0.0 and 1.0.
    */
   Vector2d pointAt(size_t index, double t) const;
   ```

4. **Documentation content**:
   - Include tables for complex explanations (e.g., attribute descriptions).
   - Provide notes about limitations or unsupported features.
   - Cross-reference other parts of the code using `\ref`.

   ```cpp
   /**
    * @see \ref SVGRadialGradientElement, \ref SVGStopElement
    */
   ```

## Includes

Include paths for Donner files are relative to the Donner SVG directory and use double quotes:

```cpp
// GOOD:
#include "donner/base/Vector2.h"

// BAD:
#include "Vector2.h"
#include <donner/base/Vector2.h>
```

STL and third-party dependencies do not use this, and use angle brackets:

```cpp
#include <vector>
#include <gmock/gmock.h>
```

## Symbol naming

- **Classes**: UpperCamelCase, matching filename. This matches the SVG standard for DOM object naming.
- **Class methods**: lowerCamelCase, aligning with the SVG standard.
  - Constructors, and constructor-like static methods continue to use UpperCamelCase.
- **Free functions**: UpperCamelCase.
- **Member variables**: lowerCamelCaseWithTrailingUnderscore_.
- **Parameters and local variables**: lowerCamelCase
- **Constants**: `k` prefix, and then UpperCamelCase: `kExampleConstant`

Example in context:

```cpp
#pragma once
/// @file

#include <string>

#include "donner/base/MathUtils.h"

/// NOTE: Documentation removed for brevity.
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

The column limit is set at 100 characters.

## Formatting

The `.clang-format` file in the repo root is the source of truth for formatting.

## Tests

Tests are placed in a `tests/` directory near the file they are testing. Test files should have the suffix of `_tests.cc`.

## Code conventions

### Const correctness

Apply `const` whenever possible, for any methods that don't modify the object's state.

```cpp
Vector2d pointAt(size_t index, double t) const;
```

### struct and class

- Use `struct` for data classes and `class` for classes with logic. `struct` classes may have simple methods, typically for read-only access.
- Implement comparison operators using `= default` when possible.
- Use `/* implicit */` to indicate intentional implicit constructors.
- Provide `operator<<` for data classes to streamline debugging.

```cpp
/// NOTE: Documentation removed for brevity.

struct StructExample {
  float x = 0.0f;
  float y = 0.0f;

  bool operator==(const StructExample& rhs) = default;
  friend std::ostream& operator<<(std::ostream& os, const StructExample& example);

  float computeThing() const;
};

class ClassExample {
  /* implicit */ ClassExample(OtherType value);

  bool operator==(const ClassExample& rhs) = default;

  friend std::ostream& operator<<(std::ostream& os, const ClassExample& example);

  OtherType value() const;
  void setValue(OtherType value);

private:
  OtherType value_;
};
```

### Properties (getters and setters)

- Use `thing()` and `setThing(...)` naming.

```cpp
/// Get the element id, the value of the "id" attribute.
RcString id() const;

/**
  * Set the element id, the value of the "id" attribute.
  *
  * @param id New id to set.
  */
void setId(std::string_view id);
```

- Use `std::optional` for potentially absent values, and to handle unsetting a value.

```cpp
std::optional<Lengthd> x1() const;
void setX1(std::optional<Lengthd> x1);
```

### enum class

- Use `enum class` for all enums, and provide an `operator<<` for debugging.

```cpp
/// NOTE: Documentation removed for brevity.
enum class ClipPathUnits {
  UserSpaceOnUse,
  ObjectBoundingBox,
  Default = UserSpaceOnUse,
};

inline std::ostream& operator<<(std::ostream& os, ClipPathUnits units) {
  switch (units) {
    case ClipPathUnits::UserSpaceOnUse: return os << "ClipPathUnits::UserSpaceOnUse";
    case ClipPathUnits::ObjectBoundingBox: return os << "ClipPathUnits::ObjectBoundingBox";
  }

  UTILS_UNREACHABLE();
}
```

### Asserts

- Debug-only: `#include <cassert>` and use `assert(condition && "Message")`.
- Release: `UTILS_RELEASE_ASSERT(condition)` or `UTILS_RELEASE_ASSERT_MSG(condition, "Message")`.

### "donner/base/Utils.h"

- `if (UTILS_PREDICT_TRUE(condition))` for hinting the compiler about likely branches.
- `if (UTILS_PREDICT_FALSE(condition))` for hinting the compiler about unlikely branches.
- `UTILS_UNREACHABLE()` for unreachable code paths.
- `UTILS_RELEASE_ASSERT(condition)` for release-mode assertions.
- `UTILS_RELEASE_ASSERT_MSG(condition, "Message")` for release-mode assertions with a message.
- `#if UTILS_EXCEPTIONS_ENABLED()` for code that should only be compiled when exceptions are enabled.

### Strings

- Use `std::string_view` for non-owning references.
- Use `RcString` for owning references.
- Use `RcStringOrRef` for passing either a non-owning `std::string_view` or an owning `RcString` which can be add-ref'd.

For passing strings as parameters:

- `void fn(std::string_view str)` for non-owning references.
- `void fn(const RcString& str)` for owning references.
- `void fn(const RcStringOrRef& str)` for API surfaces which may extend the lifetime of the `RcString`. This is typically seen at public API surfaces.

For common string helpers, see `donner/base/StringUtils.h`:

- `StringUtils::EqualsLowercase("ExAmPle", "example")` - to compare a string against a lowercase string.
- `StringUtils::StartsWith("Example", "Ex")` - to check if a string starts with a prefix.
- `StringUtils::EndsWith("Example", "ple")` - to check if a string ends with a suffix.
- Case insensitive matching is configurable:
  - `StringUtils::StartsWith<StringComparison::IgnoreCase>("Hello", "he")`
- `StringUtils::Split("a,b,c", ',')` - to split a string by a delimiter.

```cpp
for (std::string_view part : StringUtils::Split("a,b,c", ',')) {
  // ...
}
```

### Limit use of auto

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

- For `ParseResult<Type>` _maybeResult_ types - the type should be visible nearby, and the `ParseResult` pattern is ubiquitous in the codebase.

```cpp
auto maybeResult = NumberParser::Parse(remaining_);
if (maybeResult.hasError()) {
  ParseError err = std::move(maybeResult.error());
  err.location = err.location.addParentOffset(currentOffset());
  return err;
}

const NumberParser::Result& result = maybeResult.result();
```

### Operator overloading and comparisons

#### Use of `operator<=>` when possible

- Use the C++20 spaceship operator (`operator<=>`) when possible for automatic generation of comparison operators.
- Be aware that `operator<=>` may not work as intended with pointers or when custom comparison logic is needed.
- **Note**: Due to a bug in gtest, `operator==` must also be supplied when using `operator<=>`.

#### Writing custom comparison operators

- When the default behavior of `operator<=>` is not suitable (e.g., when comparing pointers or references), explicitly define comparison operators.
- Ensure that the comparison logic accurately reflects the semantics of the class.

Example:

```cpp
// Custom equality operator for a class that holds a pointer.
constexpr friend bool operator==(const OptionalRef& lhs, const OptionalRef& rhs) {
  return lhs.hasValue() == rhs.hasValue() &&
         (!lhs.hasValue() || lhs.value() == rhs.value());
}
```

- Avoid relying on `operator<=>` in cases where member-wise comparison does not align with the intended behavior.

#### Providing `operator<<` for debugging

- Implement `operator<<` for classes to streamline debugging and logging.
- The output should be clear and provide meaningful information about the object's state.

Example:

```cpp
friend std::ostream& operator<<(std::ostream& os, const OptionalRef& opt) {
  if (opt) {
    return os << *opt;
  } else {
    return os << "nullopt";
  }
}
```

### Use of `constexpr`

- Use `constexpr` for functions and methods that can be evaluated at compile-time.
- This can improve performance and enable compile-time checks.
- Apply `constexpr` to constructors and operators where appropriate.

Example:

```cpp
constexpr OptionalRef() noexcept = default;
constexpr OptionalRef(const OptionalRef& other) noexcept = default;
```

### Asserting preconditions

- Use `assert` statements to enforce preconditions in debug builds.
- Ensure that `assert` messages are informative.

Example:

```cpp
constexpr const T& value() const {
  assert(ptr_ && "OptionalRef::value() called on empty OptionalRef");
  return *ptr_;
}
```

To assert in release builds, use `UTILS_RELEASE_ASSERT` or `UTILS_RELEASE_ASSERT_MSG`.

```cpp
#include "donner/base/Utils.h"

UTILS_RELEASE_ASSERT(!str.empty());
UTILS_RELEASE_ASSERT_MSG(str.size() < 100, "String is too long");
```

### Method naming conventions

- **Class methods**: Use `lowerCamelCase`, aligning with the SVG standard.
  - Example: `hasValue()`, `reset()`, `value()`

### Implicit constructors

- Use `/* implicit */` comment to indicate intentional implicit constructors, especially when they take a single argument.
- This clarifies the intent and aids in code reviews.

Example:

```cpp
/* implicit */ OptionalRef(std::nullopt_t nullopt) noexcept {}
```

### General coding practices

- Be cautious when dealing with raw pointers and references.
- Ensure that lifetime and ownership semantics are clear and properly documented.
- When overloading operators, make sure they are consistent with the class's semantics and do not introduce unexpected behavior.

<div class="section_buttons">

| Previous                                       |                                     Next |
| :--------------------------------------------- | ---------------------------------------: |
| [System architecture](SystemArchitecture.html) | [ECS Architecture](EcsArchitecture.html) |

</div>
