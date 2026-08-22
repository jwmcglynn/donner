# Design: CSS Parser Token Streams - Pull-Based Parsing

**Status:** Implemented. `ComponentValueStream` is the selector cursor, the CSS tokenizer is a
manual pull source, and ordinary CSS names avoid temporary decoding buffers. Coroutine token
generation is rejected by measurement.
**Author:** GPT-5
**Drafted by:** Claude Opus 4.6
**Requested by:** Jeff McGlynn

## Summary

Donner parses CSS with two distinct pull abstractions:

- `Tokenizer` produces one `Token` at a time through `next()` and `isEOF()`. It does not
  materialize a `std::vector<Token>`.
- `ComponentValueStream` provides lookahead and forward movement over an owned component-value
  list for selector parsing.

The parser retains owned `ComponentValue`, declaration, and rule vectors because they are public
parse results. The tokenizer's ordinary-name path copies the source slice directly into `RcString`.
Only names containing escapes or NUL bytes use the decoding buffer.

## Goals

- Keep token production lazy without changing the public CSS API.
- Avoid transient allocations for ordinary identifiers, function names, hash names, dimensions,
  and at-keywords.
- Preserve token values, byte offsets, diagnostics, error recovery, and resource limits.
- Keep timing and allocation measurements reproducible in-tree.

## Non-Goals

- Replacing owned semantic result vectors with borrowed views.
- Retaining references to caller-owned CSS after a public parse function returns.
- Adopting coroutine token generation.
- Introducing parse-lifetime borrowed SVG strings or a final compaction/interning pass. That is a
  separate ownership design because it crosses structured editing and diagnostic lifetimes.

## Current Architecture

`Tokenizer` owns a cursor into its caller-provided input for the duration of parsing. Every token
payload that survives a call to `next()` is an owning `RcString` or another owning value. Ordinary
names enter `RcString` directly, allowing short names to use its inline storage. Escaped and
NUL-containing names preserve the prior incremental `std::vector<char>` decoding behavior.

`ComponentValueStream` wraps `std::span<const ComponentValue>` and provides `peek`, `advance`,
`isEOF`, and whitespace helpers. It does not own or copy the component values. The caller owns the
underlying vector for the stream's entire lifetime.

## Performance

Six alternating parent/optimized runs on the representative benchmark corpus produce these mean
CPU times:

| Workload                           |    Parent | Optimized |  Delta |
| ---------------------------------- | --------: | --------: | -----: |
| Raw medium stylesheet tokenization | 10,085 ns |  4,704 ns | -53.4% |
| Medium style attribute             |  2,898 ns |  1,960 ns | -32.4% |
| Medium stylesheet                  | 29,844 ns | 23,879 ns | -20.0% |
| Complex selector                   |  2,097 ns |  1,547 ns | -26.2% |

The allocation benchmark counts C++ `operator new` calls and requested bytes. It does not measure
process RSS or non-C++ allocators.

| Workload                           | Parent calls / bytes | Optimized calls / bytes | Call delta |
| ---------------------------------- | -------------------: | ----------------------: | ---------: |
| Raw medium stylesheet tokenization |          404 / 5,282 |                24 / 315 |     -94.1% |
| Medium style attribute             |           78 / 8,029 |              19 / 7,248 |     -75.6% |
| Medium stylesheet                  |        705 / 111,554 |           325 / 106,587 |     -53.9% |

Retained fallback cases cover a long ordinary prefix followed by an escape or embedded NUL. Both
take about 165 ns and perform 8 allocation calls / 175 requested bytes on the measured system.

The rejected `co_yield` prototype adds one 248-byte allocation per tokenizer, grows the tokenizer
object from 144 to 152 bytes, slows raw medium tokenization by 28%, and slows the measured public
parser entry points by 11% to 17%.

## Verification and Enforcement

- `//donner/css/parser:parser_tests` covers tokenizer values, offsets, ordinary and long names,
  escapes, embedded NUL replacement, EOF behavior, and parser integration.
- `//donner/css/parser:css_parsing_tests` checks the external CSS parsing corpus.
- `bazel test --config=asan-fuzzer //donner/css/parser:stylesheet_parser_fuzzer` exercises the
  untrusted-input path with ASan and libFuzzer.
- `//donner/benchmarks:css_parse_perf_bench` retains raw and public-entry-point timings.
- `//donner/benchmarks:css_parse_allocation_bench` retains C++ allocation calls and requested
  bytes in a separate binary so instrumentation cannot perturb the timing benchmark.

Run the retained benchmarks with:

```sh
bazel run -c opt //donner/benchmarks:css_parse_perf_bench -- --benchmark_min_time=0.5s
bazel run -c opt //donner/benchmarks:css_parse_allocation_bench
```

## Security and Lifetime

The tokenizer remains linear in input length. Ordinary names perform one scan and one owning
`RcString` construction. Decoded names perform the fast scan followed by the existing incremental
decode path; no recursion or new amplification mechanism is introduced. Parser component-count and
recursion budgets remain unchanged.

No returned CSS object depends on the caller's input lifetime. Token offsets remain byte offsets
into the parse input for diagnostics, while retained token payloads own their bytes.

## Alternatives

- **Coroutine token generation:** Rejected because it adds a heap frame and resume overhead without
  removing an existing token vector.
- **Borrow all token strings until a final intern pass:** Kept out of this design. It requires an
  explicit parse-session owner and finalization boundary that covers diagnostics, structured
  editing, incremental reparsing, and every returned view.

## Implementation References

- `donner/css/parser/details/Tokenizer.cc`
- `donner/css/parser/details/ComponentValueStream.h`
- `donner/benchmarks/CssParsePerfBench.cpp`
- `donner/benchmarks/CssParseAllocationBench.cpp`

The original detailed experiment plan and discussion remain available in git history at
`dcd36aff2:docs/design_docs/0019-css_token_stream.md`.
