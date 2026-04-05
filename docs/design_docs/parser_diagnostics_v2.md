# Design: Parser Diagnostics v2 {#ParserDiagnosticsV2}

**Status:** Design
**Author:** Claude Opus 4.6
**Created:** 2026-04-05
**Issue:** https://github.com/jwmcglynn/donner/issues/442

## Summary

Replace the existing `ParseError` / `std::vector<ParseError>*` diagnostics infrastructure with a
unified `ParseDiagnostic` type that carries severity, source ranges (not just a single offset), and
human-readable messages. Introduce `ParseWarningSink` as a first-class abstraction that replaces
the ad-hoc `std::vector<ParseError>* outWarnings` pattern, with implicit zero-cost suppression of
formatting overhead when warnings are disabled. The goal is clang-quality diagnostics: every parser
reports precise source ranges, and a console formatter can render errors with source text and
caret/tilde indicators.

No backward compatibility with the existing `ParseError` API is required.

## Goals

- **Unified diagnostic type** (`ParseDiagnostic`) shared across all parsers: XML, SVG, CSS, path,
  transform, etc.
- **Full source ranges**: every diagnostic carries a `SourceRange` (`[start, end)` half-open
  interval), not just a single `FileOffset`.
- **Severity levels**: distinguish errors (fatal) from warnings (non-fatal).
- **First-class warning collection**: `ParseWarningSink` is always passed to parser entry points.
  When disabled, warning emission is near-zero-cost---formatting overhead is implicitly avoided
  without the caller doing anything special.
- **Console rendering**: a diagnostic renderer that prints source context with caret/tilde
  indicators (similar to clang/rustc output).
- **Comprehensive test coverage**: every parser has tests verifying that reported ranges are
  accurate.

## Non-Goals

- Backward compatibility with the existing `ParseError` struct or `ParseResult<T>` API.
- Structured error codes or an error-code enum system (string reasons remain the primary message;
  codes can be added later if programmatic error handling is needed).
- Fixit suggestions or auto-correction (future work).
- Internationalization of error messages.
- LSP/IDE protocol support or machine-readable JSON diagnostics in this phase.
- Redesigning non-parser error types outside ParseResult/ParseError flows (e.g.
  `ResourceLoaderError`, `UrlLoaderError`).

## Next Steps

1. Land base types (`SourceRange`, `ParseDiagnostic`, `ParseWarningSink`) in `donner/base`.
2. Migrate one parser vertical end-to-end (`PathParser` + `SVGParserContext`) as a proving path.
3. Implement the diagnostic renderer and verify it produces readable output.

## Implementation Plan

- [ ] **Milestone 1: Introduce base diagnostics model in `donner/base`**
  - [ ] Add `SourceRange` with half-open `[start, end)` semantics and parent-offset remapping.
  - [ ] Add `ParseDiagnostic` value type with severity, reason, range.
  - [ ] Add `ParseWarningSink` with no-op and collecting implementations, lazy-format API.
  - [ ] Update `ParseResult<T>` to use `ParseDiagnostic` instead of `ParseError`.
  - [ ] Update `ParseResultTestUtils.h` matchers for the new types.
  - [ ] Delete `ParseError.h` / `ParseError.cc`.
  - [ ] Unit tests for `SourceRange` math, `ParseDiagnostic` invariants, sink behavior.
- [ ] **Milestone 2: Range-correctness migration for parsers**
  - [ ] Update `ParserBase` to produce `ParseDiagnostic` with ranges.
  - [ ] Migrate `NumberParser`, `IntegerParser`, `LengthParser`.
  - [ ] Migrate `PathParser` (key test case: partial results with accurate ranges).
  - [ ] Migrate `TransformParser`, `ViewBoxParser`, `AngleParser`.
  - [ ] Migrate `LengthPercentageParser`, `PreserveAspectRatioParser`, `Number2dParser`,
    `PointsListParser`, `CssTransformParser`, `ListParser`.
  - [ ] Migrate `DataUrlParser` to use `ParseDiagnostic` (remove `DataUrlParserError` enum).
  - [ ] Add range-accuracy tests for each parser.
- [ ] **Milestone 3: Warning plumbing migration**
  - [ ] Migrate `SVGParserContext` to hold `ParseWarningSink&` instead of `std::vector<ParseError>*`.
  - [ ] Migrate `AttributeParser` to use `ParseDiagnostic`.
  - [ ] Migrate `SVGParser` public API: replace `std::vector<ParseError>* outWarnings` with
    `ParseWarningSink&`.
  - [ ] Standardize subparser remapping with `SourceRange`-aware composition helpers.
  - [ ] Migrate `XMLParser` to produce `ParseDiagnostic`.
  - [ ] Migrate CSS `ColorParser`, `SelectorParser` to produce `ParseDiagnostic`.
  - [ ] Bridge CSS tokenizer `ErrorToken` to `ParseDiagnostic` at parser boundary.
  - [ ] Make `StylesheetParser` report diagnostics via `ParseWarningSink`.
  - [ ] Add range-accuracy tests for CSS/XML parsers.
- [ ] **Milestone 4: Diagnostic rendering utilities**
  - [ ] Implement `DiagnosticRenderer` for single-line and multi-line source highlights.
  - [ ] Add severity labels (error/warning) and optional filename prefixes.
  - [ ] Add golden/snapshot tests for renderer output.

## Proposed Architecture

### Type Hierarchy

```
donner/base/
  FileOffset.h          (unchanged - keep FileOffset)
  SourceRange.h         (NEW - replaces FileOffsetRange, half-open [start, end))
  ParseDiagnostic.h     (NEW - replaces ParseError.h)
  ParseWarningSink.h    (NEW - replaces std::vector<ParseError>*)
  ParseResult.h         (MODIFIED - uses ParseDiagnostic)
```

### High-level data flow

```mermaid
flowchart TD
    A[Source Text] --> B[XMLParser]
    B --> C[SVGParser]
    C --> D[AttributeParser]
    D --> E["SubParsers: Path, Transform, Color, ..."]

    B -->|fatal errors| F["ParseResult&lt;T&gt;"]
    C -->|warnings| G[ParseWarningSink]
    D -->|warnings| G
    E -->|warnings via remapAndMerge| G

    F --> H[Caller]
    G --> H
    H --> I[DiagnosticRenderer]
    I --> J[Console Output with Arrows]

    style G fill:#f9f,stroke:#333
    style I fill:#bbf,stroke:#333
```

### Core Types

#### `SourceRange`

```cpp
// donner/base/SourceRange.h
namespace donner {

/**
 * Half-open source range [start, end) representing a span of characters in a parsed input.
 *
 * Both endpoints carry optional line metadata for multi-line inputs. Supports parent-offset
 * remapping for subparser composition.
 */
struct SourceRange {
  FileOffset start;  ///< Start of the range (inclusive).
  FileOffset end;    ///< End of the range (exclusive).

  /// Create a range from a start offset and a length.
  static SourceRange OffsetAndLength(size_t offset, size_t length);

  /// Create a range covering a single character at the given offset.
  static SourceRange AtOffset(size_t offset);

  /// Create a zero-length insertion point at the given offset.
  static SourceRange Point(FileOffset location);

  /// Remap this range into absolute coordinates given a parent parser's origin offset.
  [[nodiscard]] SourceRange addParentOffset(FileOffset parentOffset) const;

  /// Equality operator.
  bool operator==(const SourceRange&) const = default;

  /// Ostream output.
  friend std::ostream& operator<<(std::ostream& os, const SourceRange& range);
};

}  // namespace donner
```

This replaces `FileOffsetRange` with clearer half-open semantics and the `addParentOffset`
remapping that currently lives in ad-hoc code scattered across `SVGParserContext` and `FileOffset`.

#### `ParseDiagnostic`

```cpp
// donner/base/ParseDiagnostic.h
namespace donner {

/// Severity level for a parser diagnostic.
enum class DiagnosticSeverity : uint8_t {
  Warning,  ///< Non-fatal issue; parsing continues.
  Error,    ///< Fatal issue; parsing may stop or produce partial results.
};

std::ostream& operator<<(std::ostream& os, DiagnosticSeverity severity);

/**
 * A diagnostic message from a parser, with severity, source range, and human-readable reason.
 *
 * This is the shared diagnostic type used across all donner parsers (XML, SVG, CSS, etc.).
 */
struct ParseDiagnostic {
  /// Severity of this diagnostic.
  DiagnosticSeverity severity = DiagnosticSeverity::Error;

  /// Human-readable description of the problem.
  RcString reason;

  /// Source range that this diagnostic applies to.
  SourceRange range;

  /// Create an error diagnostic at a single offset.
  static ParseDiagnostic Error(RcString reason, FileOffset location);

  /// Create an error diagnostic with a source range.
  static ParseDiagnostic Error(RcString reason, SourceRange range);

  /// Create a warning diagnostic at a single offset.
  static ParseDiagnostic Warning(RcString reason, FileOffset location);

  /// Create a warning diagnostic with a source range.
  static ParseDiagnostic Warning(RcString reason, SourceRange range);

  /// Ostream output operator.
  friend std::ostream& operator<<(std::ostream& os, const ParseDiagnostic& diag);
};

}  // namespace donner
```

#### `ParseWarningSink`

The key design requirement is that **callers should not need to do anything special to avoid
formatting overhead when warnings are disabled**. This is achieved with a template `add` method
that accepts a callable (factory). The callable is only invoked when the sink is enabled, so the
`RcString` formatting inside the lambda body is never executed when warnings are suppressed.

```cpp
// donner/base/ParseWarningSink.h
namespace donner {

/**
 * Collects parse warnings during parsing. Always safe to call `add()` on---when disabled,
 * warnings are silently dropped without invoking the factory callable, implicitly avoiding
 * string formatting overhead.
 *
 * Replaces the `std::vector<ParseError>* outWarnings` pattern.
 *
 * Usage:
 * @code
 * // The lambda is only invoked if the sink is enabled---no formatting overhead when disabled.
 * sink.add([&] {
 *   return ParseDiagnostic::Warning(
 *       RcString::fromFormat("Unknown attribute '%s'", name.c_str()), range);
 * });
 * @endcode
 */
class ParseWarningSink {
public:
  /// Construct a sink that collects warnings.
  ParseWarningSink() = default;

  /// Construct a disabled sink that discards all warnings (no-op).
  static ParseWarningSink Disabled();

  /// Returns true if the sink is enabled (will store warnings).
  bool isEnabled() const { return enabled_; }

  /**
   * Add a warning via a factory callable. The callable is only invoked when the sink is enabled,
   * implicitly avoiding formatting overhead when warnings are disabled.
   *
   * @tparam Factory A callable returning ParseDiagnostic.
   */
  template <typename Factory>
    requires std::invocable<Factory> &&
             std::same_as<std::invoke_result_t<Factory>, ParseDiagnostic>
  void add(Factory&& factory) {
    if (enabled_) {
      warnings_.push_back(std::forward<Factory>(factory)());
    }
  }

  /// Add a pre-constructed warning (for cases where the diagnostic is already built).
  void add(ParseDiagnostic&& warning);

  /// Access the collected warnings.
  const std::vector<ParseDiagnostic>& warnings() const { return warnings_; }

  /// Returns true if any warnings have been added.
  bool hasWarnings() const { return !warnings_.empty(); }

  /// Merge all warnings from another sink into this one.
  void merge(ParseWarningSink&& other);

  /**
   * Merge warnings from a subparser, remapping source ranges using the given parent offset.
   * Replaces SVGParserContext::addSubparserWarning.
   */
  void mergeFromSubparser(ParseWarningSink&& other, FileOffset parentOffset);

private:
  std::vector<ParseDiagnostic> warnings_;
  bool enabled_ = true;
};

}  // namespace donner
```

**Why a template `add` with a callable?** The current pattern requires callers to explicitly guard
formatting:
```cpp
// OLD: caller must remember to check, easy to forget
if (warnings_) {
  warnings_->push_back(ParseError{RcString::fromFormat("Bad '%s'", name), offset});
}
```

With `ParseWarningSink`, the zero-cost behavior is implicit:
```cpp
// NEW: formatting is automatically skipped when disabled
sink.add([&] {
  return ParseDiagnostic::Warning(RcString::fromFormat("Bad '%s'", name), range);
});
```

The lambda body (including the `RcString::fromFormat` call) is never executed when the sink is
disabled. Callers don't need to check `isEnabled()` or wrap anything in conditionals.

For the common case where the diagnostic string is a literal (no formatting needed), the
direct `add(ParseDiagnostic&&)` overload avoids lambda boilerplate:
```cpp
sink.add(ParseDiagnostic::Warning("Missing attribute", range));
```

#### Updated `ParseResult<T>`

```cpp
// donner/base/ParseResult.h
namespace donner {

template <typename T>
class ParseResult {
public:
  /* implicit */ ParseResult(T&& result);
  /* implicit */ ParseResult(const T& result);
  /* implicit */ ParseResult(ParseDiagnostic&& error);
  /* implicit */ ParseResult(const ParseDiagnostic& error);

  /// Partial result + error.
  ParseResult(T&& result, ParseDiagnostic&& error);

  T& result() &;
  T&& result() &&;
  const T& result() const&;

  ParseDiagnostic& error() &;
  ParseDiagnostic&& error() &&;
  const ParseDiagnostic& error() const&;

  bool hasResult() const noexcept;
  bool hasError() const noexcept;

  template <typename Target, typename Functor>
  ParseResult<Target> map(const Functor& functor) &&;

  template <typename Target, typename Functor>
  ParseResult<Target> mapError(const Functor& functor) &&;

private:
  std::optional<T> result_;
  std::optional<ParseDiagnostic> error_;
};

}  // namespace donner
```

The API shape is identical to today. Only the contained error type changes from `ParseError` to
`ParseDiagnostic`. Fatal errors still flow through `ParseResult<T>`, while non-fatal warnings
flow through `ParseWarningSink`.

### SVGParserContext Changes

`SVGParserContext` currently owns the `std::vector<ParseError>*` and does offset remapping. It will
hold a `ParseWarningSink&` reference instead:

```cpp
class SVGParserContext {
public:
  SVGParserContext(std::string_view input, ParseWarningSink& warningSink,
                   const SVGParser::Options& options);

  /// The warning sink for this parse session.
  ParseWarningSink& warningSink() { return warningSink_; }

  /// Add a warning from a subparser, remapping source ranges to absolute coordinates.
  void addSubparserWarning(ParseDiagnostic&& diag, ParserOrigin origin);

  /// Remap a diagnostic from a subparser back to the original input string.
  ParseDiagnostic fromSubparser(ParseDiagnostic&& diag, ParserOrigin origin);

  // ... rest unchanged (getAttributeLocation, offsetToLine, etc.)

private:
  ParseWarningSink& warningSink_;
  // ...
};
```

### SVGParser Public API

```cpp
class SVGParser {
public:
  // Primary API: caller provides a warning sink.
  static ParseResult<SVGDocument> ParseSVG(
      std::string_view source,
      ParseWarningSink& warningSink,
      Options options = {},
      SVGDocument::Settings settings = {}) noexcept;

  // Convenience: warnings are discarded.
  static ParseResult<SVGDocument> ParseSVG(
      std::string_view source,
      Options options = {},
      SVGDocument::Settings settings = {}) noexcept;
};
```

### Diagnostic Renderer

```cpp
// donner/base/DiagnosticRenderer.h
namespace donner {

class DiagnosticRenderer {
public:
  struct Options {
    int contextLines = 1;         ///< Context lines before/after the diagnostic.
    bool colorize = false;        ///< Enable ANSI color codes.
    std::string_view filename;    ///< Optional filename for the header.
  };

  /// Format a single diagnostic against source text.
  static std::string format(std::string_view source, const ParseDiagnostic& diag,
                            const Options& options = {});

  /// Format all warnings in a sink against source text.
  static std::string formatAll(std::string_view source, const ParseWarningSink& sink,
                               const Options& options = {});
};

}  // namespace donner
```

Renderer behavior:
- **Single-line range**: caret at start + tildes for span width.
- **Zero-length (point)**: caret at insertion point.
- **Multi-line span**: first/last line emphasis with bounded context.
- **Resilient fallback**: best-effort output if source text is unavailable or range is malformed.

Example output:

```text
warning: Invalid paint server value
  --> line 4, col 12
4 | <path fill="url(#)"/>
  |             ^~~~~~
```

### Migration Pattern for Parsers

**Before (return error):**
```cpp
return ParseError{RcString("Unexpected character"), FileOffset::Offset(pos)};
```

**After (return error):**
```cpp
return ParseDiagnostic::Error("Unexpected character",
    SourceRange::OffsetAndLength(pos, 1));
```

**Before (emit warning):**
```cpp
context.addWarning(ParseError{RcString::fromFormat("Bad '%s'", name), offset});
```

**After (emit warning, lazy):**
```cpp
context.warningSink().add([&] {
  return ParseDiagnostic::Warning(
      RcString::fromFormat("Bad '%s'", name), range);
});
```

## API / Interfaces

### Public API Surface

| Type | Header | Role |
|------|--------|------|
| `SourceRange` | `donner/base/SourceRange.h` | Half-open `[start, end)` source span |
| `ParseDiagnostic` | `donner/base/ParseDiagnostic.h` | Shared diagnostic value type |
| `DiagnosticSeverity` | `donner/base/ParseDiagnostic.h` | Error vs Warning enum |
| `ParseWarningSink` | `donner/base/ParseWarningSink.h` | Warning collector/sink |
| `ParseResult<T>` | `donner/base/ParseResult.h` | Result-or-error (uses `ParseDiagnostic`) |
| `DiagnosticRenderer` | `donner/base/DiagnosticRenderer.h` | Console rendering utility |
| `FileOffset` | `donner/base/FileOffset.h` | Single source position (unchanged) |

### Removed Types

| Type | Replacement |
|------|-------------|
| `ParseError` | `ParseDiagnostic` |
| `FileOffsetRange` | `SourceRange` |
| `DataUrlParserError` | `ParseDiagnostic` returned via `ParseResult` |

## Performance

- **Zero-cost when disabled**: `ParseWarningSink::Disabled()` creates a sink where the template
  `add(factory)` method short-circuits before invoking the factory callable. No `RcString`
  formatting, no allocations, no virtual dispatch.
- **Implicit for callers**: Unlike the old pattern where callers had to remember to check
  `if (warnings_)`, the lazy-factory API makes zero-cost suppression automatic.
- **No additional allocations on success path**: `ParseResult<T>` still uses `std::optional`.
  `ParseDiagnostic` is slightly larger than `ParseError` (adds severity + one extra `FileOffset`
  for range end), but this only matters on error paths.
- **Reuse existing line-offset indexing**: `SVGParserContext` already maintains `LineOffsets`;
  the same data is reused for `SourceRange` construction.

## Security / Privacy

Parsers process untrusted SVG/XML/CSS input, so diagnostics must avoid introducing amplification
or data-leak risks.

```mermaid
flowchart LR
    A[Untrusted input text] --> B[Parser]
    B --> C[Diagnostic range + reason]
    C --> D[Renderer]
    D --> E[Logs / console output]
```

- Clamp/validate ranges before rendering to prevent out-of-bounds access.
- Truncate rendered source excerpts and reason length in logging paths.
- Avoid echoing unrelated large input regions in diagnostics.
- Add negative tests for malformed ranges and very long lines.

## Testing and Validation

### Unit tests (`donner/base`)

- `SourceRange` construction, offset math, parent-offset remapping, edge cases (empty, single-char,
  end-of-string).
- `ParseDiagnostic` invariants: severity, copy/move, factory methods.
- `ParseWarningSink`: no-op vs collecting behavior, lazy-factory suppression (verify factory is not
  invoked when disabled), merge and subparser remapping.

### Parser tests

Every parser gets range-accuracy tests. Example pattern:

```cpp
TEST(PathParser, ErrorRangeAccuracy) {
  auto result = PathParser::Parse("M 100 100 h 2!");
  ASSERT_THAT(result, AllOf(
      ParseErrorIs("Failed to parse number: Unexpected character"),
      DiagnosticRangeIs(SourceRange::OffsetAndLength(13, 1))));
}
```

New test matchers:

```cpp
// Matches a ParseDiagnostic by message.
MATCHER_P(ParseErrorIs, messageMatcher, "");

// Matches the source range of a diagnostic.
MATCHER_P(DiagnosticRangeIs, expectedRange, "");

// Matches severity of a diagnostic.
MATCHER_P(DiagnosticSeverityIs, expectedSeverity, "");
```

### Golden/snapshot tests

Renderer output is tested with inline golden strings to catch formatting regressions:

```cpp
TEST(DiagnosticRenderer, SingleLineRange) {
  auto diag = ParseDiagnostic::Error("Unexpected character",
                                      SourceRange::OffsetAndLength(24, 1));
  EXPECT_EQ(DiagnosticRenderer::format(source, diag, {.filename = "test.svg"}),
            "error: Unexpected character\n"
            "  --> test.svg:1:25\n"
            "   |\n"
            " 1 | <path d=\"M 100 100 h 2!\" />\n"
            "   |                         ^\n");
}
```

### Fuzz / negative tests

- Extend parser fuzz harnesses to assert no crashes with malformed ranges.
- Add renderer robustness tests for adversarial range values (past end-of-string, reversed
  start/end, very long lines).

## Alternatives Considered

### 1. Keep `ParseError`, just add an `endOffset` field

- Pros: smallest API diff.
- Cons: warnings remain ad hoc; harder to share renderer and severity model.
- **Rejected**: doesn't address the warning plumbing problem.

### 2. Keep `ParseError` and add `ParseWarning` as a separate type

- Pros: explicit type distinction.
- Cons: duplicated infrastructure (two types, two sets of matchers, two collection patterns).
- **Rejected**: a severity field on a unified type is simpler.

### 3. `std::expected`-based `ParseResult`

- Pros: standard library type.
- Cons: C++23 (Donner targets C++20); doesn't support partial-result pattern.
- **Rejected**.

### 4. Virtual `ParseWarningSink` interface

- Pros: extensible (custom sink implementations).
- Cons: virtual dispatch overhead on every `add()` call; harder to inline the enabled check.
- **Rejected**: concrete class with template `add` gives zero-cost inlining without virtual
  dispatch. A virtual interface can be added later if custom sinks are needed.

### 5. Global thread-local diagnostics collector

- Pros: minimal signature churn.
- Cons: hidden state, poor testability, unsafe in concurrent scenarios.
- **Rejected**.

## Open Questions

1. **Should `SourceRange` replace `FileOffsetRange` globally?** `FileOffsetRange` is currently used
   in `XMLNode::getAttributeLocation`. We could either rename it or keep both and convert at
   boundaries.

2. **Should the renderer live in `donner/base` or a separate utility library?** It depends on
   whether non-parser code (e.g. CLI tools) needs it. Recommendation: `donner/base` for now since
   it only depends on base types.

3. **Default truncation limits for renderer output?** Need to decide on max source-excerpt width
   and max reason length for logging paths.

## Future Work

- [ ] Structured error codes for programmatic error handling.
- [ ] Fixit suggestions ("did you mean ...?").
- [ ] Multi-line range rendering in the renderer.
- [ ] LSP-compatible diagnostic output (JSON) for editor integration.
- [ ] Machine-readable diagnostic serialization for CI tooling.
- [ ] Parser feature metrics (warning/error counts by parser type).
