# Design: Teleport IPC Framework

**Status:** Draft (one-pager — concept exploration, not yet green-lit)
**Author:** Claude Opus 4.7
**Created:** 2026-04-21
**Codename:** Teleport

## Summary

**Teleport** is a Donner-native IPC framework built on **C++26 static reflection**
(P2996) to auto-generate serialization, dispatch, and versioning boilerplate from
plain C++ struct/interface declarations. Think of it as how Donner messages
teleport from one process (or test recording) to another — same shape on both
ends, validated on arrival, with no IDL in between.

Teleport targets three workloads that Donner already has or is about to have:

1. **Editor ↔ sandboxed renderer** process split (design doc [0023](0023-editor_sandbox.md)) —
   the first real IPC consumer.
2. **Common renderer types on the wire** (`PathSpline`, `Transformd`, `CssColor`,
   `RenderingInstanceView`, `BoxRectd`, filter graphs) without hand-written codecs.
3. **Record-and-replay tests** — the same codecs serialize a command stream to disk for
   deterministic replay, replacing today's ad-hoc "stash arguments in a struct and re-drive"
   patterns in `AsyncRenderer` / `CompositorController`.

**Scope: Donner-internal / editor infrastructure first. Public surface
later.** V1 is internal only — it ships inside `donner_editor`, the
editor's sandboxed-renderer helper process, and our own test binaries.
**Public library consumers (BCR, the CMake mirror, embedders of
`libdonner`) continue to use the existing single-process Donner** — same
headers, same C++20 baseline, same `SVGDocument::render(...)` call. No
`#include <donner/editor/ipc/...>` in the public tree; no C++26 or reflection
dependency reaches a consumer's build in v1.

This is **phasing, not a permanent boundary.** Internal-first lets us
iterate on the wire format, toolchain baseline, and sharp edges of
C++26 reflection without a public-API compatibility burden. Public
exposure is the likely endpoint — but deferred until (a) the framework
has proven out against real internal workloads, and (b) Donner's public
C++ baseline advances off C++20 so embedders don't have to chase our
toolchain. The most likely trigger for (b) is adopting `std::expected`
from C++23 in the public API, at which point re-exposing the IPC
framework to embedders becomes a policy call ("do we want to?") rather
than a toolchain blocker ("we can't give them C++26 today"). Revisit
scope at each public-baseline bump.

The gold standard we're benchmarking against is **Chromium Mojo**: typed interfaces, zero-copy
message shapes, associated/callback interfaces, versioning discipline, and a strong
"you cannot forge a handle" security model. We think reflection lets us match Mojo's safety and
usability *without* the .mojom IDL, its Python-based bindings generator, or the multi-stage
bazel/gn codegen tooling that comes with it.

**Naming.** "Teleport" is the framework / product name and the C++
namespace (`donner::teleport`). The directory on disk stays
`donner/editor/ipc/` — the *what* (an IPC subsystem) stays generic,
the *how* (Teleport, our reflection-based implementation) gets the
brand. A future second IPC implementation could live alongside
Teleport under the same `ipc/` directory without a rename.

## Goals

- **No IDL, no codegen step.** Schemas are plain C++ headers that the reflection-driven
  framework reads at compile time. A user writes `struct RenderRequest { ... };` with an
  annotation attribute, and gets type-safe serialize/deserialize + a type-erased
  dispatch stub for free.
- **Matches or beats Mojo on safety.** Every message is length-prefixed, every handle is
  capability-typed, every interface has an explicit version, and the receiver validates
  fields before the user handler sees them. No "raw pointer on the wire" escape hatch.
- **Efficient for hot IPC.** FlatBuffers-style "read fields in place" where it helps, but
  optimized for the *IPC* path (short-lived messages, same-endian peer, shared-memory ring
  buffer) rather than the *persistent file* path (alignment padding for random access,
  long-term forwards/backwards compat). Target: per-message overhead ≤ a single small
  allocation and ≤ ~100 ns of framework cost on the send side for typical editor messages.
- **Transmits Donner's common types natively.** `donner::base::Vector2d`, `Transformd`,
  `PathSpline`, `CssColor`, `BoxRectd`, `RenderingInstanceView`, filter graph nodes, and
  the ECS entity-handle wire format all have first-class codecs. (v1: codecs live
  under `donner/editor/ipc/codecs/` to keep `donner/base/` and `donner/svg/`
  reflection-free for public consumers. When IPC goes public, codecs move next
  to their types — but still with no separate IDL tree.)
- **Record-and-replay as a first-class mode.** Any interface can be routed to a
  `FileTransport` that dumps the same framed messages to disk, with a replay tool that
  re-drives the receiver end. Opens the door to deterministic regression tests for
  `AsyncRenderer` drag traces and editor command streams.
- **Schema evolution with teeth.** Adding, removing, or reordering a field is detected by
  the framework and either (a) allowed with a migration rule, or (b) a compile-time error
  that forces the author to bump the interface version. No silent wire-breaks.
- **Structural versioning, not just numeric.** The framework computes a stable
  content-hash of every interface from its reflected shape and bakes it into the handshake.
  Mismatched peers refuse to connect rather than decode garbage.

## Non-Goals

- **Not a public Donner API in v1.** Embedders of `libdonner` (BCR, CMake
  mirror, external consumers) get zero IPC surface. They continue to use
  the single-process rendering path. Public exposure is deferred to a
  later phase (see Scope above) — v1 is internal-only infrastructure.
- **Not a general-purpose RPC system.** No cross-language bindings (no Python, no
  JavaScript, no Rust consumers). This is C++-to-C++ inside the Donner process tree.
- **Not a network protocol.** Local same-host transports only: pipe, socketpair,
  shared memory, in-process. Internet-facing use is explicitly out of scope and would
  need a different threat model.
- **Not a persistent storage format.** Record/replay files are tied to a build hash and
  are not meant to be readable by a future Donner version. Long-term persistence
  belongs to a different layer (SVG on disk, JSON for configs).
- **Not a drop-in Mojo replacement.** We're not re-implementing Mojo's associated
  interfaces, message pipes-as-handles, or the full Chromium channel-attachment model
  in v1. We pick the subset that pays for itself for Donner's workloads.
- **No "auto-upgrade old recordings."** Replay corpora that predate a schema bump are
  invalidated and regenerated, not migrated.

## Next Steps

1. **Confirm the P2996 baseline** on the *current stable LLVM release*
   (LLVM 21.1.6 is today's pinned toolchain — `MODULE.bazel`). If 21.1.6
   already reaches P2996 we're done; if not, bump the pin to the first
   release that does. Clang-trunk is off the table.
2. **Spike the reflection codec** for one hand-picked struct (`RenderRequest`)
   on that pinned toolchain. Confirm per-field cost, compile-time budget, and
   clean hermetic builds on `ubuntu-24.04` + `macos-latest`.
3. **Work the Toolchain Baseline feasibility gates** (see below): libFuzzer
   + sanitizers, CMake mirror story, and a nice-to-have check that latest
   Ubuntu / macOS system toolchains also work.
4. **Promote to a full design doc** (Proposed Architecture + API sketch +
   phased implementation plan) once the spike + toolchain feasibility gates
   are in. Until then this document is exploratory.

## Implementation Plan

- [ ] **Milestone 0.1: Can we write reflection codecs at all?** (exploratory,
      single branch, no merge gate, non-hermetic build)
  - [ ] Build a standalone `teleport_spike` target that reflects one struct
        into a binary message + round-trip decoder and compares the output
        to a hand-written baseline for both correctness and per-message
        overhead. Target Bloomberg's `clang-p2996` fork — either via Docker
        (`vsavkov/clang-p2996` is a known-good community image) or a local
        source build. **No `MODULE.bazel` changes yet** — the spike is
        explicitly non-hermetic and lives behind its own `--config` /
        env-var setup so default `bazel test //...` is untouched.
  - [ ] Document the setup in `donner/editor/ipc/spike/SPIKE_NOTES.md`:
        exact Bloomberg fork commit, compiler flags, `std::meta::*` calls
        used, and any paper cuts (missing primitives, IDE coverage).
- [ ] **Milestone 0.2: Hermetic Bazel toolchain for the fork (native)**
      (gated on M0.1 success)
  - [ ] Add Bloomberg's fork as a second `llvm_toolchain` registration in
        `MODULE.bazel`, pinned to a specific commit, gated by
        `--//donner/editor/ipc:enable_reflection`. The default `bazel build
        //...` path continues to use the mainline LLVM 21.1.6 pin and never
        touches the fork.
  - [ ] Confirm hermetic bazel build passes on `ubuntu-24.04` and
        `macos-latest` with the flag enabled.
  - [ ] Run the spike under `--config=asan-fuzzer` to confirm the
        sanitizer + libFuzzer story holds with reflected codecs.
  - [ ] Decide the CMake-mirror story: exclude the IPC subtree from
        `gen_cmakelists.py` (preferred), or carry a `-std=c++26` knob in
        the mirror. Document which.
- [ ] **Milestone 0.3: Wasm toolchain spike — `EM_LLVM_ROOT` + Bloomberg
      fork** (gated on M0.1 success, timeboxed to 1–2 days)
  - [ ] Build Bloomberg's fork from source with
        `-DLLVM_TARGETS_TO_BUILD="X86;AArch64;WebAssembly"` plus
        libcxx/libcxxabi/compiler-rt for Wasm.
  - [ ] Point Emscripten at it via `EM_LLVM_ROOT`; build a minimal
        "hello reflection" TU that reflects one struct and is callable
        from JS. Success = round-trip encode/decode inside the browser.
  - [ ] Document findings (`donner/editor/ipc/spike/WASM_NOTES.md`):
        which Emscripten flags the fork accepts, any libc++ ABI gaps,
        estimated per-Emscripten-bump maintenance cost.
  - [ ] **Decision point:** based on spike result, either wire up Wasm
        Teleport support in the M0.2 Bazel toolchain work, or commit to
        the graceful-degrade fallback and add Wasm feature gates to
        Teleport-dependent sites (docs 0033 / 0035 trace export,
        record/replay).
- [ ] **Milestone 1: Core framework** (gated on M0 results + full design-doc promotion)
- [ ] **Milestone 2: First real consumer** — wire up to the `editor_sandbox` channel
      from design doc 0023, exercising the full send/receive/version-handshake path
      against a real workload.
- [ ] **Milestone 3: Record/replay transport** — `FileTransport`, replay CLI, and
      deterministic regression tests for one `AsyncRenderer` trace.
- [ ] **Milestone 4: Common-types codec library** — first-class codecs for
      `PathSpline`, `Transformd`, `CssColor`, `RenderingInstanceView`, filter graph nodes.

## Proposed Architecture

Three layers, each testable and swappable:

1. **Schema layer (headers, compile-time).** Plain C++ structs and interfaces with a
   lightweight attribute marker (e.g. `[[donner::teleport::interface(version=3)]]`). The
   reflection-driven codegen sees these via `std::meta::*` and materializes serialize /
   deserialize / dispatch functions at compile time. No external tool in the bazel graph.

2. **Message layer (runtime, allocation-aware).** Framed messages with a fixed header
   (length, interface-hash, method-id, version, flags) and a body that is either
   "flat read-in-place" (for POD-dominated messages on same-endian peers — the common
   case for editor IPC) or "sequential decode into owning types" (for messages with
   variable-length / indirect data like `PathSpline` or nested filter graphs).
   A single 64 KiB ring-buffer-backed shared-memory region for the hot path; a pipe
   fallback for control messages.

3. **Transport layer.** Pluggable: `SharedMemoryTransport` (editor ↔ sandboxed renderer),
   `PipeTransport` (control channel + attachment), `InProcessTransport` (unit tests,
   same-process consumers), `FileTransport` (record/replay — writes and reads the
   identical framed byte stream).

### How it fits Donner

- **Location in the tree:** framework code lives under `donner/editor/ipc/`
  (or similar internal-only path). It is *not* under `donner/base/ipc/` or
  `donner/svg/ipc/` — those would imply public-API availability, which v1
  doesn't have. Visibility is restricted via Bazel `visibility` attributes
  so a stray `//donner/svg:...` dep on IPC code is a build-time error.
- **Editor:** `AsyncRenderer` / `CompositorController` already have a well-defined
  command-queue seam (`donner::editor::CommandQueue`). The IPC framework slots in as
  the on-wire codec for that queue when the renderer becomes an out-of-process peer.
- **Renderer:** Common types live with their definitions under `donner/base`,
  `donner/svg/core`, `donner/svg/renderer/common` — and they stay
  reflection-agnostic in v1. The wire-shape glue lives *next to the IPC
  framework* under `donner/editor/ipc/codecs/`, not next to the types.
  This way `donner/base/Vector2d.h` doesn't grow a C++26 dependency that
  public consumers would have to satisfy. When the baseline bumps and we
  expose IPC publicly, codecs can move next to their types at that point.
- **Tests:** Record/replay integrates with the existing `AsyncRenderer_tests.cc` and
  `CompositorGolden_tests.cc` harnesses. A replay file becomes a new kind of golden.

## Security / Privacy

The framework sits squarely on a **trust boundary** (editor ↔ sandboxed parser-renderer,
see 0023). Threat model inherits everything from the editor sandbox: the *receiver*
treats every message as untrusted.

Defensive measures baked into the framework (not user-written):

- **Length-prefixed framing.** Every read validates frame length against remaining buffer
  before decode, including nested variable-length fields.
- **Total-message and per-field caps.** Hard limits on total message size, per-string
  length, per-array count, and nesting depth. Exceeding any cap drops the message and
  tears down the channel.
- **Handle typing.** No raw FD or pointer on the wire. Handles are opaque capability
  tokens validated by the kernel or the transport. Borrowing Mojo's model here.
- **Interface hash in handshake.** Peers exchange the reflected content-hash of every
  advertised interface at connect time. A hash mismatch is a connection failure, not a
  silent wire-break.
- **No escape hatches.** `memcpy`-style "just give me the bytes" codecs are not exposed;
  every field passes through the reflected decoder with bounds checks.
- **Fuzzing discipline.** Every codec is fuzzed on the receiver path. A codec that
  can crash on an adversarial byte stream is a bug on the same severity as an XML or
  CSS parser crash — it's a release gate.
- **Side channels.** Out of scope for v1 (trust boundary is process-local, and the
  threat model matches Mojo's: confidentiality against a compromised peer is not a
  guarantee). Revisit if the transport ever crosses a privilege boundary.

## Testing and Validation

- **Unit tests** for each codec: round-trip for valid inputs, and explicit
  "this malformed frame must be rejected" tests for every cap and invariant.
- **Fuzzers.** One fuzzer per reflected interface, auto-generated alongside the codec.
  Corpus from replay recordings + OSS-Fuzz-style coverage-guided generation.
- **Schema-drift tests.** Reflection content-hash of every shipped interface is
  checked into the repo; any unreviewed change is a CI failure.
- **Record/replay self-tests.** Every `AsyncRenderer_tests.cc` trace gets a paired
  "replay this recorded file and compare" test. Bit-exact replay is the invariant.
- **Performance gates.** Per-message send/receive overhead measured on `donner_perf_cc_test`
  targets. Budget assertions on the hot-path editor message shape (`RenderRequest`).

## Alternatives Considered

### A. Custom IDL (Mojo-style `.mojom`, protobuf-style `.proto`, or FIDL)

**Pros.**
- No compiler support surprises; tools like `protoc` / `mojom_parser` are mature.
- Cross-language bindings fall out naturally (we don't need them, but they're free).
- Wire format is decoupled from source layout — rename a C++ field without a wire break.
- Easier to hand to external reviewers ("show me the interface" = show me the IDL file).

**Cons.**
- Adds a codegen step to the bazel graph and the CMake mirror. We've already paid that
  cost once for `resvg_test_suite` and once for shader compilation — each one was
  load-bearing pain.
- Two sources of truth: the IDL and the C++ type the application wants to use.
  Every common type (`PathSpline`, `CssColor`, …) needs a hand-written
  IDL↔native marshalling shim, and those shims are the part that keeps breaking.
- IDL changes require editing a non-C++ file, which hurts the "one person can land
  an interface change in a single PR" workflow.

**Why not chosen (tentatively).** The load-bearing argument for an IDL is cross-language
support, which is an explicit non-goal. For a C++-only consumer with C++26 reflection,
the IDL is ceremony without payoff. We accept the compiler-baseline risk in exchange for
eliminating an entire build phase.

### B. Hand-written codecs (status quo extended)

**Pros.** Zero new tech. Easy to understand line-by-line.
**Cons.** Every new message type is a new opportunity for a validation bug — exactly
the thing SecurityBot refuses to rubber-stamp. This doesn't scale past ~3 interfaces.

### C. FlatBuffers / Cap'n Proto directly

**Pros.** Mature, zero-copy, well-fuzzed.
**Cons.** Both optimize for "long-lived artifacts on disk" (schema evolution,
random-access alignment). For the *IPC* hot path — short-lived messages, same-build
peer, shared memory ring buffer — the alignment and offset-table overhead is real.
Also schema lives in a separate `.fbs` / `.capnp` file: same IDL-ceremony problem as A.

## Toolchain Baseline

**Reality check (2026-04).** Mainline clang 21.1.6 (our current
`MODULE.bazel` pin) does **not** implement P2996. As of the current
C++26 cycle the only usable implementations are
[Bloomberg's `clang-p2996` fork](https://github.com/bloomberg/clang-p2996)
and an experimental clang branch; mainline clang has some reflection
plumbing but not enough to drive a real codec spike. This forces a
temporary bootstrap toolchain — a pinned commit of Bloomberg's fork —
rather than "latest stable mainline LLVM."

**Policy.**

- **v0 (M0 spike + initial internal rollout): Bloomberg `clang-p2996` fork**,
  pinned to a specific tag/commit in `MODULE.bazel` via a second
  `llvm_toolchain` registration, **gated behind a build flag**
  (`--//donner/editor/ipc:enable_reflection`). The default `bazel build //...`
  path continues to use the mainline LLVM 21.1.6 pin and never touches the
  fork. Only IPC-framework targets opt in. This keeps the fork's maintenance
  surface contained to a single subtree while the spike proves the framework
  out.
- **v1 (once mainline clang ships P2996 at parity with what we use):** swap
  the pin back to mainline LLVM, drop the build-flag gate, retire the fork
  dependency. The API surface the framework depends on is the standardized
  `std::meta::*` from P2996, so swapping toolchains is a `MODULE.bazel`
  diff, not a rewrite.
- **Never: clang-trunk.** The fork is acceptable because it is *pinned,
  tagged, and stable against a specific P2996 revision*. We do not track
  anyone's trunk.

**Exit criterion (explicit sunset for the fork).** Drop the fork pin the
first release cycle where **all** of the following hold:
1. Mainline clang (tagged stable) implements every `std::meta::*` entry
   point our internal consumers rely on.
2. libFuzzer + ASan + UBSan on mainline clang build and run the reflected
   codecs cleanly (see feasibility gate below).
3. A single-commit `MODULE.bazel` diff that swaps the toolchain passes
   full `bazel test //...` on both `ubuntu-24.04` and `macos-latest`.

Donner's existing Bazel LLVM toolchain infrastructure already supports
pinning by URL/commit — adding a second toolchain registration for the
fork, and selecting between them via a config flag, is mechanically
the same as bumping the `llvm_version` field. We don't chase trunk, and
we don't hand-roll a "bring-your-own-clang" story.

**Scope of the framework's toolchain footprint.** The IPC framework is
**internal-first** (see Scope in Summary). BCR consumers and CMake-mirror
consumers see a C++20 library surface; only Donner's own internal build
pays the C++26 / reflection cost. This keeps the blast radius contained
while we learn what the sharp edges are, and defers the "push C++26 at
every embedder" decision until the public baseline moves forward anyway.

**Wasm surface.** Donner's Wasm editor build uses Emscripten (currently
`emsdk 5.0.5`), which vendors its own clang tracking mainline upstream —
so Wasm inherits mainline's lack of P2996 support. The **goal is
feature parity on every editor surface, native and Wasm, for Teleport**
— but the path there depends on a toolchain spike.

**Wasm policy: spike first, decide after.** There is a known
"swap the compiler under Emscripten" pattern using the
[`EM_LLVM_ROOT`](https://emscripten.org/docs/building_from_source/index.html)
environment variable: build Bloomberg's fork from source with
`-DLLVM_TARGETS_TO_BUILD="...;WebAssembly"` + libcxx/compiler-rt for
Wasm, point `EM_LLVM_ROOT` at the result, let `emcc` drive the build
using the fork's clang. This is how the ecosystem does "custom clang
with Emscripten" today — not a bespoke toolchain we invent. Whether it
*actually* holds together for our workload is the question M0.3
answers.

Real caveats that the spike must surface (each is a potential blocker):

- Emscripten's `-fignore-exceptions` flag and other flags must exist in
  the fork's clang. The fork is based on a recent-enough clang that it
  likely does — confirm.
- libc++ built from the fork must be ABI-compatible with what
  Emscripten's JS glue expects at link time. Plausible but not
  guaranteed.
- Every Emscripten version bump needs re-testing with the fork.
  Ongoing maintenance tax, estimated small-but-nonzero per bump.
- `bazel_dep(name = "emsdk", ...)` must either respect `EM_LLVM_ROOT`
  or we route around it — figure out which in the spike.

**Two branches depending on M0.3 outcome.**

- **If the EM_LLVM_ROOT spike succeeds:** Teleport ships on Wasm at
  parity with native. Record/replay, debugger trace export, and any
  Teleport-based record/replay integration (docs
  [0033](0033-svg_debugger_in_editor.md) /
  [0035](0035-perf_framework_and_analyzer.md)) work on both surfaces.
- **If the spike fails (ABI mismatch, unfixable flag gap, etc.):**
  fall back to graceful degradation on Wasm — core editor rendering,
  live debugger inspection, and the live perf panel keep working (they
  don't need a wire format); features that require serialization are
  disabled on Wasm until mainline clang + Emscripten ship P2996. No
  dual codec system either way — the only escape hatch for a
  Wasm-critical message is a single hand-written codec for that one
  message, as an exception, never a parallel system.

**Parity restored on the public-baseline bump regardless.** When
mainline clang has P2996 and Emscripten picks it up on its normal
cascade, Wasm gets serialization features automatically with no
per-interface migration work — Teleport's source of truth is the
reflected C++ type; the toolchain substitution is invisible to the
interfaces.

**Feasibility gates (M0 spike must answer each).**

- **Hermetic bazel build on Linux + macOS.** The pinned LLVM toolchain
  works out-of-the-box on both `ubuntu-24.04` runners and `macos-latest`.
  Goal: no special setup beyond `bazel build`. If the hermetic toolchain
  covers this, we inherit a working build on every CI lane.
- **libFuzzer / sanitizers.** Every codec is a fuzz target. The pinned
  LLVM's libFuzzer + ASan + UBSan must still compile and run the P2996
  reflection output. A broken sanitizer story is a blocker, not a paper cut.
- **CMake mirror (`gen_cmakelists.py`).** The mirror needs to express
  "this target needs `-std=c++26` + whatever reflection flag the release
  uses" cleanly. Since the framework is internal, the simplest answer is
  that the IPC subtree is excluded from the CMake mirror entirely — a
  BCR/CMake consumer never needs to build it. Confirm that's viable.
- **Nice-to-have: latest-Ubuntu-release system toolchain.** Users who
  choose *not* to use our hermetic LLVM (Homebrew LLVM on macOS,
  `apt install clang` on Ubuntu 24.04 → 25.04 → …) should ideally also
  get a working build, so devcontainer and "just clone and build"
  workflows don't force a hermetic-toolchain download. This is explicitly
  a nice-to-have, not a blocker — the hermetic toolchain is the source
  of truth.

**What to evaluate on 24.04 + macOS specifically** (M0 spike checklist):

- Ubuntu 24.04 system `clang-20` / `clang-21` (via `apt`): does it reach
  P2996? What flags does it need? Is `<experimental/meta>` available or
  do we need `<meta>` gated on the LLVM release?
- Ubuntu 24.04 + hermetic LLVM 21.1.6 (current Bazel pin): baseline.
- macOS `apple-clang` from Xcode Command Line Tools: Apple's clang lags
  upstream and likely can't reach P2996 yet. Document the expected gap
  (users on macOS *must* use the hermetic toolchain — which is already
  the default for `bazel build`, so this is a no-op in practice but
  needs to be spelled out for CMake-mirror users).
- macOS + Homebrew `llvm` (upstream clang N via brew): the realistic
  "system toolchain" escape hatch on macOS. Confirm a known-good brew
  LLVM version.

## Open Questions

- **IDE / tooling support.** clangd, VS Code C++ extension, CLion: how
  badly do they stumble on heavy `std::meta::*` use? A "great on the
  command line, broken in the editor" outcome would hurt day-to-day
  usability. Probably not a blocker (the pinned-LLVM-version clangd
  tracks the compiler), but verify during M0.
- **Debuggability.** When a codec emits garbage, can we inspect reflected
  types in lldb with reasonable fidelity?
- **Compile time.** Reflection-heavy templates historically explode build
  time. Do we need explicit-instantiation boundaries per-interface to
  keep the TU graph sane? This is a measure-in-M0 question.
- **Upgrade cadence.** How often do we expect to bump the LLVM pin to
  track P2996 fixes / perf improvements? If the answer is "every 6 weeks"
  (trunk cadence) we have a dev-infra tax; if it's "every major release"
  (≈ every 6 months) we're fine.
- **GCC / MSVC eventually?** Not required for internal use — the pinned
  toolchain is clang. Becomes a real question at the "go public" phase:
  embedders who build with GCC or MSVC would need equivalent reflection
  support, and the feature's maturity on those compilers will gate that
  phase.

Answering the feasibility gates above (and at least spot-checking the
open questions) is what gates promotion from "one-pager" to "full design
doc + approved implementation plan."

## Future Work

- [ ] **Go-public phase.** Once (a) the framework is proven on internal
      workloads, and (b) Donner's public C++ baseline has moved off C++20
      (most likely trigger: adopting `std::expected` from C++23), re-scope
      the framework as a public API: move codecs next to their types, drop
      the `donner/editor/ipc/` visibility restriction, publish via BCR as
      an opt-in `donner_ipc` module. Gated on GCC/MSVC reflection parity
      if we want to keep supporting those compilers publicly.
- [ ] Cross-process zero-copy for `RenderingInstanceView` / path geometry (shared-memory
      arena backing the ECS view).
- [ ] GPU-handle marshalling (share WebGPU buffers across the process boundary for Geode
      hand-off).
- [ ] Automatic fuzzer harness generation from reflected interfaces.
- [ ] Generic recording viewer — a `donner_svg_tool` subcommand that pretty-prints a
      recording file.
