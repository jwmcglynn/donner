# Teleport IPC — M0.1 spike notes

**Status: GREEN.** The reflection-driven codec round-trips a `RenderRequest`
end-to-end on Bloomberg's `clang-p2996` fork, with encode/decode timings and
a negative-decode test recorded below. One genuine API-drift fix was needed
between the initial handoff and a clean build (see [Fix applied](#fix-applied)).

Historical context: the original spike author could not execute on their
host (aarch64, no Docker) and handed off written-but-unverified source code
with a documented blocker. This run unblocked that handoff on an
x86_64 + Docker host; the "Blocker" note below is preserved for the record
but no longer applies.

---

## Blocker (historical — resolved 2026-04-22)

The original spike host had:

* No Docker daemon (`docker` was not installed).
* `podman` 4.9.3 installed but the host kernel rejected `clone()` for new
  user namespaces under the calling UID.
* Only mainline `clang-18` / `clang-19` — neither understands
  `-freflection-latest`.
* `aarch64` architecture, while `vsavkov/clang-p2996` ships `linux/amd64`
  (and also `linux/arm64` — see tag list — but the spike author did not
  attempt the arm64 tag).

Resolution: the spike was re-run on an x86_64 Ubuntu 24.04 host with
Docker 28.1.1. See [Measurements](#per-message-overhead) for the numbers.

---

## Fix applied

The handed-off `teleport_codec.h` used:

```cpp
constexpr auto members = std::meta::nonstatic_data_members_of(
    ^^T, std::meta::access_context::current());
template for (constexpr auto member : members) { … }
```

That fails to compile on `clang-p2996` at Bloomberg commit
`f72d85e5a0fd9c960b07b6f1c8875110701cb627` with:

```
error: constexpr variable 'members' must be initialized by a constant expression
note: pointer to subobject of heap-allocated object is not a constant expression
```

Root cause: `nonstatic_data_members_of` returns `std::vector<std::meta::info>`,
and P2996 does not allow heap storage to persist in a non-transient
`constexpr` variable. The fork provides `std::define_static_array` (in
`<meta>`, not `<experimental/meta>` — that header is a thin alias) which
promotes an input range into statically-allocated storage and returns
`std::span<const T>`. The codec now reads:

```cpp
static constexpr auto members = std::define_static_array(
    std::meta::nonstatic_data_members_of(
        ^^T, std::meta::access_context::current()));
template for (constexpr auto member : members) { … }
```

Applied in both `encodeOne` and `decodeOne`. This is exactly the kind of
"API drift" the spike was designed to surface before M0.2 commits to the
fork.

---

## Reproduction

### Toolchain — Docker image

`:latest` does **not** exist on Docker Hub; tags are `:amd64` and `:arm64`
(last pushed 2025-12-02). Use the tag that matches your architecture.

```bash
docker pull vsavkov/clang-p2996:amd64
docker image inspect vsavkov/clang-p2996:amd64 --format '{{.Id}}'
```

> **Pinned digest (amd64):**
> `sha256:6672c4227b09efc7318695c17e8a8f696e193f451b52d885d220f593593dc1c8`
>
> **Underlying Bloomberg commit:**
> `f72d85e5a0fd9c960b07b6f1c8875110701cb627` (reported by `clang++ --version`
> inside the image). M0.2 should pin both when registering the `cc_toolchain`.

### Build (Makefile path — recommended for the spike)

```bash
docker run --rm --platform=linux/amd64 \
    -v "$PWD:/work" -w /work/donner/editor/ipc/spike \
    -e LD_LIBRARY_PATH=/opt/p2996/clang/lib/x86_64-unknown-linux-gnu \
    vsavkov/clang-p2996:amd64 \
    bash -lc 'make && ./teleport_spike'
```

`LD_LIBRARY_PATH` is required because the fork's `libc++.so.1` is not on
the default loader path; the Makefile does not rpath it in. M0.2's Bazel
toolchain should emit an rpath so the binary is runnable without env
plumbing.

Expected output (observed on the reference host below):

```
Teleport spike — RenderRequest round-trip
Encoded: 146 bytes
Round-trip: MATCH
Encode: 84 ns/op (median over 100k iters)
Decode: 36 ns/op (median over 100k iters)
Negative test: OK (DecodeError=1)
```

`DecodeError=1` is `kStringTooLarge` — the spike truncates the encoded
buffer to 12 bytes (past the u32 length prefix of `svg_source` but before
its payload), so the length check `len > r.remaining()` fires. An earlier
draft of this doc predicted `DecodeError=0` (`kTruncated`); that was a
placeholder, corrected after the first real run.

### Build (Bazel path — present for M0.2 continuity, will fail today)

```bash
bazel build --config=teleport_spike //donner/editor/ipc/spike:teleport_spike
```

This is intentionally a hard error under the current `MODULE.bazel` because
the toolchain doesn't exist there yet — wiring it up is exactly what M0.2
will do. The `--config=teleport_spike` shortcut is not yet defined in
`tools/bazel/.bazelrc`; add it (or use the long-form
`--//donner/editor/ipc/spike:enable_spike=True
--repo_env=CC=/path/to/clang-p2996/bin/clang
--repo_env=CXX=/path/to/clang-p2996/bin/clang++`) when M0.2 lands.

### Why a Makefile, not Bazel

Three reasons it stays out of Bazel for M0.1:

1. The Bloomberg fork is not in any Bazel central registry, so wiring it up
   means writing a custom `cc_toolchain` — that is M0.2 work and would
   double the size of this spike.
2. The default `bazel test //...` MUST stay green. Even gated behind a
   `bool_flag`, an enabled-by-default `cc_binary` with `-std=c++26
   -freflection-latest` would explode every CI lane the moment someone
   forgot the gate. The current `target_compatible_with = select(...)`
   keeps the target invisible to wildcard expansion when the flag is off.
3. We want a single-command "does this even round-trip?" answer for any
   developer with Docker, without requiring a Bazel rebuild of the
   toolchain.

---

## Compiler flags (target combination)

Confirmed against the public Bloomberg fork README at the time of writing
(verify against the README of the digest you pin):

```
-std=c++26 -freflection-latest -fexpansion-statements -O2
```

`-freflection-latest` is the umbrella flag the fork uses to opt into the
most recent P2996 revision rather than an older snapshot.
`-fexpansion-statements` enables `template for` (P1306), which the codec
uses to walk struct members. If the fork has renamed either flag (the
P2996 group iterates fast), check the build log — the source itself does
not depend on flag names.

---

## Reflection API surface used

| `std::meta::*` call | Used in | Purpose |
|---|---|---|
| `^^T` (reflection operator) | `teleport_codec.h` | Get a `std::meta::info` for a type. |
| `std::meta::access_context::current()` | `teleport_codec.h` | Required second argument to `nonstatic_data_members_of` in the latest P2996 revision. |
| `std::meta::nonstatic_data_members_of(refl, ctx)` | `teleport_codec.h` | Enumerate fields in declaration order. |
| `[: member :]` (splice) | `teleport_codec.h` | Form a member-access expression at compile time. |
| `template for (constexpr auto m : members) { … }` | `teleport_codec.h` | Iterate over the meta-info range at compile time (P1306). |

Things deliberately *not* used (kept for later milestones):

* `std::meta::identifier_of(member)` — would let us emit field names into
  the wire format for self-describing diagnostics, but the codec is
  implicit-schema by design (M1 adds a schema-hash handshake).
* `std::meta::type_of(member)` + `std::meta::is_class_type` — not needed
  because the encode/decode dispatch is already a `if constexpr` chain on
  the spliced field's type.
* `consteval` annotations on `Encode` / `Decode` themselves — left as
  ordinary inline templates so libFuzzer can drive `Decode` at run time in
  M0.2 without fighting the compiler.

---

## Negative-decode behaviour

`teleport_spike.cc` truncates the encoded buffer to 12 bytes (just past the
`std::uint32_t` length prefix of `svg_source`) and asserts `Decode` returns
`std::unexpected(DecodeError::kStringTooLarge)` rather than UB. This is the
single most important invariant for M0.2 — a crashy decoder cannot be
fuzzed safely, and the whole point of using `std::expected` here is to
make every read-side failure a recoverable error.

If the codec ever needs a new wire-format type (containers, optional
fields), add a parallel negative test for it before checking in.

---

## Per-message overhead

Measured 2026-04-22 on the reference host below. Three consecutive runs
landed inside the `[83, 84]` ns encode band and `[34, 37]` ns decode band,
so the numbers are stable to ±1–2 ns at this fixture size.

| Quantity | Value | Notes |
|---|---|---|
| Encoded size of the RenderRequest fixture | **146 bytes** | `4 (width) + 4 (height) + 4 (u32 str len) + 118 (svg_source payload) + 16 (Vector2d)`. Exact: matches the hand-computed layout. |
| Encode median, 100k iters | **84 ns/op** | Includes the `std::vector<std::byte>` allocation per call (`reserve(64)` then a 146-byte insert — realloc likely fires once). |
| Decode median, 100k iters | **36 ns/op** | No heap growth on the hot path once `svg_source.resize(118)` allocates; steady-state just memcpys. |
| Compile time, clean → linked binary | **1.32 s** (user 1.27 s, sys 0.06 s) | Median of 3 `time make -B` runs inside the container. Reflection + `template for` do not noticeably blow up compile time at this TU size. |

Reference host:

* CPU: 11th Gen Intel Core i5-1135G7 @ 2.40 GHz (4C / 8T, Tiger Lake)
* CPU governor: **powersave** (not `performance` — numbers could be
  pessimistic by a small factor; good enough for a feasibility gate, not
  for a final perf-budget claim)
* OS: Ubuntu 24.04, kernel 6.11.0-25-generic
* Docker: 28.1.1 (Engine + CLI), default `overlay2` storage driver
* Image: `vsavkov/clang-p2996:amd64` at the digest pinned above

For M0.2 budget math, treat encode/decode as ~100 ns + per-byte memcpy
cost; the 100 ns floor is dominated by the single `std::vector` growth on
the encode path and by the first-time `std::string` resize on the decode
path. Both are one-shot allocator hits that the production codec can
eliminate with a caller-supplied scratch buffer.

---

## Paper cuts observed

Verified against the pinned digest above.

* **`nonstatic_data_members_of` needs `std::define_static_array`.** *(Confirmed.)*
  The obvious `constexpr auto members = nonstatic_data_members_of(...)`
  pattern does not compile — the returned `std::vector` is heap-backed and
  not a valid non-transient constexpr value. Wrap with
  `std::define_static_array` (declared in `<meta>`) to get a
  statically-stored `std::span<const info>`. This was the sole code
  change between the handoff and a green run.
* **`libc++.so.1` not on default loader path.** *(Confirmed.)* The fork
  installs its libc++ under
  `/opt/p2996/clang/lib/x86_64-unknown-linux-gnu/`, but the Makefile does
  not emit an rpath, so `./teleport_spike` fails with
  `libc++.so.1: cannot open shared object file` unless
  `LD_LIBRARY_PATH` is set. M0.2's Bazel `cc_toolchain` should inject
  `-Wl,-rpath,<libc++ dir>` (or equivalent `features`) so binaries are
  self-contained.
* **Sanitizer runtimes are not shipped in the image.** *(Confirmed, new
  finding.)* `find /opt/p2996 -name 'libclang_rt*'` returns only
  `libclang_rt.builtins.a` — no `asan`, `ubsan`, `tsan`, `msan`, or
  `fuzzer`. Consequently `make asan` fails with
  `ld: cannot find /opt/p2996/clang/lib/clang/21/lib/.../libclang_rt.asan.a`.
  This is a **serious M0.2 blocker** because libFuzzer integration (M0.2
  goal #2 in the design doc) requires `-fsanitize=fuzzer,address`. Options:
  (a) rebuild compiler-rt from the fork's tree and layer it into a child
  Docker image; (b) fall back to mainline clang-20+ for the fuzz build
  and skip P2996 there (safe because the fuzzer only drives `Decode`,
  which doesn't require P2996 at the call site once the schema-generated
  dispatch is pre-baked — but verify this assumption before committing);
  (c) use an EDG/MSVC P2996 preview that bundles sanitizers. Flag item
  #6 in [Decision data for M0.2](#decision-data-for-m02).
* **`^^T` vs `decltype`.** The fork accepts `^^T` as the reflection
  operator in the same token position as `decltype`; no surprises there.
* **`<experimental/meta>` is a thin alias.** The real header is `<meta>`;
  `<experimental/meta>` just `#include`s it. Either works, but M0.2
  should standardize on `<meta>` since that's the final P2996 header name.
* **`clangd` coverage.** *(Still a prediction — not exercised here since
  the spike was built and run non-interactively.)* Mainline `clangd` has
  no idea what `^^T` or `template for` mean. The image does ship a
  matching `clangd` at `/opt/p2996/clang/bin/clangd`; M0.2 should decide
  whether to surface it in `compile_commands.json` discovery.
* **`lldb` coverage.** *(Still a prediction — no interactive debug was
  attempted.)* The fork's `lldb` is at `/opt/p2996/clang/bin/lldb` if it
  ships; verify in M0.2.
* **Missing primitives.** Spike skips containers (`std::vector`,
  `std::optional`, `std::variant`) on purpose; the codec needs explicit
  overloads for each before they round-trip. Don't extend the generic
  fallback to call `data()`/`size()` ad-hoc — that path is how
  reflection-codecs typically grow security holes.
* **Endianness.** Wire format is whatever the producer's host emits.
  Cross-arch IPC (M2+) needs an explicit byte-order pass; spike
  intentionally does not.

---

## Sanitizer / fuzzer status

* `make asan` **does not work** on the pinned image — see paper cut above;
  the sanitizer runtimes are not in the distribution. A clean ASan/UBSan
  run over the negative-decode path is therefore deferred to M0.2 with one
  of the fallbacks listed in that paper cut.
* libFuzzer was *not* attempted in M0.1 (by design; the goal is the
  round-trip answer, not fuzzer integration). Adding it is still a
  one-file scaffold — `teleport_codec_fuzzer.cc` calling
  `Decode<RenderRequest>` on the fuzzer input — but it now depends on
  resolving the sanitizer-runtime gap first.

---

## Decision data for M0.2

Open questions the next milestone needs to answer, ordered by how badly
they block hermetic Bazel integration. Updated after the 2026-04-22 run.

1. **[RESOLVED] Pin the fork.** Image digest and Bloomberg commit
   captured at the top of this file. M0.2 can register both in
   `MODULE.bazel` as the `cc_toolchain` source of truth.
2. **[PARTIAL] libc++ vs libstdc++.** The spike links against the
   fork's `libc++.so.1` cleanly, but the loader path is not default (see
   paper cut). `std::expected`, `std::span`, `std::define_static_array`
   all work. Open question: can Donner's existing dependencies (all
   libstdc++ today) be mixed into a single binary with the fork's libc++?
   That's the real M0.2 risk — a unified toolchain decision is likely
   needed.
3. **[RESOLVED] `std::expected` availability.** The fork's libc++ has
   `<expected>` under `-std=c++26` and it compiles without special flags.
   No fallback needed.
4. **[RESOLVED] `-fexpansion-statements`.** Both
   `-freflection-latest` and `-fexpansion-statements` are still required
   as separate flags on this commit; building with only the former fails
   on `template for`. M0.2's toolchain must emit both in `cxxopts`.
5. **[NEW] API stability of `nonstatic_data_members_of`.** The spike
   required the `std::define_static_array` wrapper — this is the exact
   kind of change that moves between P2996 revisions. M0.2 should vendor
   or snapshot a tested-known-good revision rather than track `latest`.
6. **[BLOCKED] Sanitizer runtimes.** The image has no `asan`/`ubsan`/
   `fuzzer` runtimes. libFuzzer integration (M0.2 goal) cannot proceed
   against this image as-is. Decision point: rebuild compiler-rt into a
   child image, or run fuzzing under mainline clang with a non-P2996
   decoder shim. Pick one before M0.2 kickoff.
7. **[DEFERRED] Wasm path (M0.3).** The fork is upstream LLVM-based so an
   `emscripten` build *should* work, but nobody has tried. Flag for M0.3
   scoping rather than M0.2.

(1), (3), (4) green means the go/no-go question is answered: **the codec
is feasible.** (2), (5), (6) are real M0.2 work but none is a hard
architectural block — each has at least one credible mitigation.
