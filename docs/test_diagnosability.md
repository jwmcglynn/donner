# Test Diagnosability {#TestDiagnosability}

\tableofcontents

This guide is for developers writing or reviewing Donner tests. A failing test
must explain the mismatch from the log alone. Prefer one assertion that matches
the whole value over several assertions that expose only the first broken field.

The default pattern is:

```cpp
EXPECT_THAT(actual, MatcherThatNamesTheContract(expected));
```

Use `EXPECT_EQ` for scalar values where the expected and actual output is already
complete and clear. Do not use `EXPECT_EQ` to validate arrays, byte buffers,
record sequences, or a cluster of related fields.

## Whole Sequences

Validate vectors, spans, fixed arrays, and other ordered collections with
`ElementsAre`, `ElementsAreArray`, `Pointwise`, `UnorderedElementsAre`, or
`SizeIs` plus a separate targeted condition only when the contents are not part
of the contract.

Prefer:

```cpp
EXPECT_THAT(bounds, ElementsAre(BoxFromXYWHIs(10.0, 15.0, 20.0, 25.0)));
EXPECT_THAT(ids, ElementsAre("r1", "r2"));
EXPECT_THAT(bytes, ElementsAreArray(expectedBytes));
```

Avoid:

```cpp
ASSERT_EQ(bounds.size(), 1u);
EXPECT_EQ(bounds[0], Box2d::FromXYWH(10.0, 15.0, 20.0, 25.0));

ASSERT_EQ(ids.size(), 2u);
EXPECT_EQ(ids[0], "r1");
EXPECT_EQ(ids[1], "r2");
```

The matcher form reports whether the failure is cardinality, ordering, or a
specific element mismatch.

## Field Matchers

For structs, match the named fields that define the contract with `Field` and
`AllOf`.

```cpp
auto SourceByteRangeIs(std::size_t start, std::size_t end) {
  return AllOf(Field("start", &SourceByteRange::start, start),
               Field("end", &SourceByteRange::end, end));
}
```

Then use the record matcher inside sequence matchers:

```cpp
EXPECT_THAT(editor.hoverSourceRanges(),
            ElementsAre(SourceByteRangeIs(2, 5), SourceByteRangeIs(5, 6)));
```

For nested records, compose matchers instead of flattening the assertion site:

```cpp
auto ActiveFlashIs(std::size_t start, std::size_t end,
                   testing::Matcher<float> intensity) {
  return AllOf(Field("byteRange", &ActiveFlash::byteRange,
                     SourceByteRangeIs(start, end)),
               Field("intensity", &ActiveFlash::intensity, intensity));
}
```

## Domain Helpers

Use small domain helpers when the test reads in domain terms. Box helpers are a
good example:

```cpp
auto BoxFromXYWHIs(double x, double y, double width, double height) {
  return BoxEq(Vector2d(x, y), Vector2d(x + width, y + height));
}

auto BoxNear(const Box2d& expected, double tolerance) {
  return BoxEq(Vector2Eq(DoubleNear(expected.topLeft.x, tolerance),
                         DoubleNear(expected.topLeft.y, tolerance)),
               Vector2Eq(DoubleNear(expected.bottomRight.x, tolerance),
                         DoubleNear(expected.bottomRight.y, tolerance)));
}
```

This keeps the test contract visible:

```cpp
EXPECT_THAT(selectionBounds,
            ElementsAre(BoxFromXYWHIs(20.0, 30.0, 140.0, 150.0)));
```

When a helper is useful in more than one test file, promote it to an existing
test utility such as `donner/base/tests/BaseTestUtils.h` or a local package test
utility. Keep highly specific helpers in the test file.

## Custom Matchers

Use `MATCHER`, `MATCHER_P`, or `MATCHER_Pn` when `Field` composition is not
expressive enough or when a mismatch needs a custom explanation.

```cpp
MATCHER_P(OptionalLengthValueIs, expected, "") {
  return testing::ExplainMatchResult(
      testing::Optional(LengthIs(testing::DoubleEq(expected),
                                 Eq(Lengthd::Unit::None))),
      arg, result_listener);
}
```

Use `ExplainMatchResult` inside custom matchers so nested matcher diagnostics
survive. Write to `result_listener` when the matcher performs custom logic that
would otherwise fail as a bare boolean.

## Printers

If a failure prints raw bytes, enum integers, or an opaque type name, add a
`PrintTo` overload in the same namespace as the type for that test binary.

```cpp
void PrintTo(const SourceByteRange& range, std::ostream* os) {
  *os << "SourceByteRange{start=" << range.start
      << ", end=" << range.end << "}";
}
```

For enums, prefer an `operator<<` in production when the type is part of the
debugging surface. Use test-local `PrintTo` only when the printer is only useful
for test diagnostics.

## Pixel And Byte Data

Pixel and byte assertions are array assertions. Match the whole pixel or buffer.

Prefer:

```cpp
EXPECT_THAT(pixel, Rgba(255, 0, 0, 255));
EXPECT_THAT(actualBytes, ElementsAreArray(expectedBytes));
EXPECT_THAT(bitmap.bytes(), PixelBytesEq(expectedBytes, width, height));
```

Avoid:

```cpp
EXPECT_EQ(pixel[0], 255);
EXPECT_EQ(pixel[1], 0);
EXPECT_EQ(pixel[2], 0);
EXPECT_EQ(pixel[3], 255);
```

For large buffers, custom matchers should report the first mismatching index and
the decoded domain coordinate when available, such as pixel `(x, y)` and channel.

## Checklist

Before adding or approving a test assertion, check:

- Can the failure be diagnosed from the log without rerunning locally?
- Does one expectation match the whole array or record sequence?
- Are field names visible in the failure for structs and nested records?
- Does the failure output print useful values for enums and domain types?
- Is a repeated assertion shape promoted to a named matcher or helper?
- Is the helper local, package-level, or shared according to actual reuse?
