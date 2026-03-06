# Tiny-Skia C++ Bootstrap Function Maps

Function-by-function mapping and equivalence notes are split by module to reduce context size.

- `docs/design_docs/tiny-skia_cpp_bootstrap_function_maps/core.md`
- `docs/design_docs/tiny-skia_cpp_bootstrap_function_maps/scan.md`
- `docs/design_docs/tiny-skia_cpp_bootstrap_function_maps/pipeline.md`
- `docs/design_docs/tiny-skia_cpp_bootstrap_function_maps/path64.md`
- `docs/design_docs/tiny-skia_cpp_bootstrap_function_maps/shaders.md`
- `docs/design_docs/tiny-skia_cpp_bootstrap_function_maps/wide.md`

Use these files for ongoing function-level status updates instead of expanding
`docs/design_docs/tiny-skia_cpp_bootstrap.md`.

## Function Status Policy

Use this status set for function-level rows:

- `☐` Not started: no C++ symbol mapped yet.
- `🧩` Stub only: scaffold/placeholder exists, behavior is incomplete.
- `🟡` Implemented/tested: code and tests exist, but Rust line-by-line completeness is not yet
  vetted.
- `🟢` Rust-completeness vetted: function has been compared line-by-line against Rust and judged
  complete for current scope.
- `✅` Verified parity sign-off (reserved): user-requested audit pass completed with explicit
  reviewer sign-off notes.
- `⏸` Blocked: cannot proceed until a dependency/decision is resolved.

`🟢` requires Rust-to-C++ completeness vetting first. Test coverage is expected by default, but
tests alone are not sufficient for `🟢`.

`✅` is a special status and must only be used when explicitly requested by the user in a dedicated
verification stage. It represents an explicit sign-off audit beyond routine implementation/testing.

For existing rows with implementation/test evidence but no recorded line-by-line Rust audit, use
`🟡` until vetted.

## Function Row Template

Use this table format for new or updated per-file sections:

| Rust function/item | C++ function/item | Status | Evidence / Notes |
| --- | --- | --- | --- |
| `example_fn` | `exampleFn` | `🧩` | Stub signature only; behavior TODO |
| `example_impl` | `exampleImpl` | `🟡` | Unit tests and parity vectors added; Rust line-by-line audit pending |
| `example_vetted` | `exampleVetted` | `🟢` | Rust source reviewed line-by-line; all branches/constants mapped |
| `example_signed_off` | `exampleSignedOff` | `✅` | User-requested audit complete; reviewer/date and parity notes recorded |

Keep notes concrete: cite test names and Rust/C++ symbols reviewed. Avoid generic phrases like
`Stub function coverage`.
