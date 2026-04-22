# Teleport IPC — M0.1 spike notes

**Status: BLOCKED on toolchain availability.** The reflection-driven codec
source code is committed and ready to build, but no run-time numbers were
collected on the spike author's host because Bloomberg's `clang-p2996` fork
could not be installed here. See [Blocker](#blocker) below for the path
forward; everything else in this document tells a follow-up developer how to
finish the spike on a host that *can* run the fork.

---

## Blocker

The execution environment for this spike has:

* No Docker daemon (`docker` is not installed).
* `podman` 4.9.3 is installed but cannot start a container — the host kernel
  rejects `clone()` for new user namespaces under the calling UID
  (`cannot clone: Permission denied / cannot re-exec process`). Pulling
  `docker.io/vsavkov/clang-p2996` fails for the same reason.
* Only mainline `clang-18` and `clang-19` are present. Neither understands
  `-freflection-latest`; P2996 is implemented only in Bloomberg's fork (and
  in unreleased EDG previews).
* The host is `aarch64` / Ubuntu 24.04 on Neoverse-N1. The community
  `vsavkov/clang-p2996` image is published only as `linux/amd64`, so even
  with Docker working we would have needed an `--platform=linux/amd64`
  emulation layer — adding more drag for a feasibility spike.
* Building `clang-p2996` from source on this host was estimated at multiple
  hours and tens of GB of disk; that exceeds the 1–3 day spike budget when
  the answer ("can the codec round-trip?") doesn't require this specific
  host to produce it.

Per the design-doc escalation guidance ("STOP and document the blocker
rather than grinding through. M0.1's value is the answer, not the
completion."), the spike was paused here.

**What's needed to unblock:** a developer on a host with working Docker
(linux/amd64) — e.g. a Linux laptop, a CI worker, or any of the team's x86
dev boxes. Steps below assume that.

---

## Reproduction (intended path, not executed here)

### Toolchain — Docker image

```bash
# Pin to a digest, not :latest, so the spike numbers are reproducible.
docker pull vsavkov/clang-p2996:latest
docker image inspect vsavkov/clang-p2996:latest --format '{{.Id}}'
# Record the resulting sha256 in this file under "Pinned digest" before
# committing any timing numbers.
```

> **Pinned digest:** _TODO — fill in once the image has been pulled on a
> working host. Until then, M0.2 (hermetic Bazel pin) cannot proceed._

### Build (Makefile path — recommended for the spike)

```bash
docker run --rm -it -v "$PWD:/work" -w /work/donner/editor/ipc/spike \
    vsavkov/clang-p2996:latest \
    bash -lc 'make && ./teleport_spike'
```

Expected output shape:

```
Teleport spike — RenderRequest round-trip
Encoded: <N> bytes
Round-trip: MATCH
Encode: <ns> ns/op (median over 100k iters)
Decode: <ns> ns/op (median over 100k iters)
Negative test: OK (DecodeError=0)
```

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

## Per-message overhead (placeholder — fill in after a real run)

| Quantity | Value | Notes |
|---|---|---|
| Encoded size of the RenderRequest fixture | _TBD_ | ~`10 + len(svg_source)` bytes for this fixture; measure to confirm. |
| Encode median, 100k iters | _TBD ns/op_ | Record CPU model + frequency governor. |
| Decode median, 100k iters | _TBD ns/op_ | Record CPU model + frequency governor. |
| Compile time, clean → `teleport_spike` linked | _TBD s_ | `time make -B` from inside the container. |

Spike author's host (for context, *not* a measurement source):

* CPU: Ampere Neoverse-N1 (`aarch64`)
* OS: Ubuntu 24.04 LTS, kernel 6.17
* No P2996 toolchain available — see [Blocker](#blocker).

---

## Paper cuts observed while writing the spike

These are predictions based on the public state of `clang-p2996`; revisit
once the spike actually runs.

* **`clangd` coverage.** Mainline `clangd` has no idea what `^^T` or
  `template for` mean and will paint the codec red. The fork ships a
  matching `clangd`; point your editor at
  `/usr/local/clang-p2996/bin/clangd` (path inside the image — adjust if
  building from source). Flagging this so M0.2 can decide whether to
  vendor the fork's `clangd` into `compile_commands.json` discovery.
* **`lldb` coverage.** The fork's `lldb` should print P2996 reflections,
  but stepping through `template for` is reportedly noisy — expect to
  set breakpoints inside `encodeOne` on the type you care about rather
  than relying on step-into.
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

* `make asan` builds `teleport_spike` with `-fsanitize=address,undefined`.
  Run after the basic round-trip works to confirm the negative-decode
  path doesn't trip UBSan (it shouldn't — bounds checks happen before any
  pointer arithmetic).
* libFuzzer was *not* attempted in M0.1. The structure to add it is one
  file (`teleport_codec_fuzzer.cc` calling `Decode<RenderRequest>` on the
  fuzzer input), but the goal of M0.1 is the round-trip answer, not
  fuzzer integration. Leave for M0.2 once the toolchain is hermetic.

---

## Decision data for M0.2

Open questions the next milestone needs to answer, ordered by how badly
they block hermetic Bazel integration:

1. **Pin the fork.** Once `vsavkov/clang-p2996` (or a self-built fork
   commit) produces a green `teleport_spike` run, capture the exact
   image digest / git SHA in this file and register it as a
   `cc_toolchain` in `MODULE.bazel`. Without a pinned digest we cannot
   reproduce timings or guarantee the encode format is stable.
2. **libc++ vs libstdc++.** The fork ships its own libc++. Confirm
   whether linking spike output against the system `libstdc++` is even
   ABI-compatible, or whether we must rebuild dependents under the
   fork's libc++. This is the most likely M0.2 surprise.
3. **`std::expected` availability.** Mainline libc++ has it, but check
   the fork's `<expected>` actually compiles under `-std=c++26` —
   fall back to `tl::expected` or a small in-house `Result<T,E>` if not.
4. **`-fexpansion-statements`.** If `template for` is gated separately
   from `-freflection-latest` in the fork's current head, M0.2's
   toolchain definition needs both flags in `cxxopts`, not just one.
5. **Wasm path (M0.3).** The fork is upstream LLVM-based, so an
   `emscripten`-style wasm build *should* work, but nobody has tried.
   Flag this for M0.3 scoping rather than M0.2.

If any of (1)–(3) come back ugly, M0.2 should explore EDG/MSVC P2996
previews as a fallback before committing the project to the Bloomberg
fork.
