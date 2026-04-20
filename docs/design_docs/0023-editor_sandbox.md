# Design: Editor Sandbox, Renderer-IPC, and Record/Replay

**Status:** Implementing — S1–S4 + S6.1 landed, S6.2 pending, **S7–S11 in flight (single PR to remove parser from host entirely)**
**Author:** Claude Opus 4.6
**Created:** 2026-04-11
**Last updated:** 2026-04-20

## Progress snapshot

| Milestone | State | Notes |
|-----------|-------|-------|
| S1 — byte-in / PNG-out child | ✅ Landed (#506/#518) | `donner_parser_child`, `SandboxHost`, fuzzer-corpus crash test. |
| S2 — RendererInterface wire format | ✅ Landed (#506/#518) | `Wire.h`, `SerializingRenderer`, `ReplayingRenderer`, `SandboxCodecs`, round-trip + structural fuzzer tests. libFuzzer harness in flight (#528). |
| S3 — partial pipeline + partial address bar | ✅ Landed (#506/#518) | `PipelinedRenderer` (in-process, thread-level), slim `app::EditorApp`, `gui/EditorWindow` shell, `SvgSource` (file + curl HTTPS), slim address bar on the MVP shell only. **Note**: despite the original doc text, `SandboxHost` today spawns *per render*, not long-lived; the full editor (`donner/editor/main.cc`) still parses in-process. Both gaps close in S7 / S9. |
| S4 — record/replay + frame inspector | ✅ Landed (#506/#518) | `.rnr` file format, `FrameInspector`, scrub-slider replay, `sandbox_inspect`/`sandbox_replay` CLIs. Structural-diff CLI in flight (#527). |
| S5 — C++26 reflection | ⏸ Parked | Gated on toolchain support; hand-rolled codecs still in place. |
| S6.1 — portable hardening | ✅ Landed (#506/#518) | `SandboxHardening` (env gate, chdir, FD sweep, `setrlimit` caps, Linux seccomp-bpf fail-open). Tests Linux-only via `target_compatible_with` (5f66ea4f) + in-source `GTEST_SKIP` defense-in-depth (469bf576). |
| S6.2 — platform jails | ⏳ Pending | macOS `sandbox_init`, Linux seccomp flipped to `KILL_PROCESS`, optional per-UID isolation. |
| **S7 — long-lived `SandboxSession`** | ⏳ **In flight** | Replaces per-render `posix_spawn` with a persistent child, reader/writer threads, respawn-on-crash, and a `std::future`-shaped request API. Foundation for S8–S11. |
| **S8 — editor-API wire protocol** | ⏳ **In flight** | The IPC boundary is the editor's public API — pointer/keyboard events, `Load`, `Undo`, `SetTool` — not the DOM. A new `donner_editor_backend` binary owns the parser, `SVGDocument`, selection, undo, tool dispatch, and driver. Host sends events; backend ships `Frame{render wire, selection chrome, source writebacks, diagnostics}` bundles back. |
| **S9 — Host editor becomes a thin client** | ⏳ **In flight** | `donner/editor/main.cc` is refactored to own only text editor, viewport, address bar, chrome; the document + mutation logic move into the backend binary. Banned-pattern lint forbids `SVGParser::ParseSVG` outside the backend. `donner_editor_gui_main.cc` is deleted. |
| **S10 — Unified address bar (desktop + WASM)** | ⏳ **In flight** | One ImGui widget with URL input, history, load/reload/stop, file picker, drag-and-drop, status chip. Desktop routes to `SvgSource` + sandbox; WASM routes to `emscripten_fetch` + in-process parser (browser is the sandbox). |
| **S11 — `ResourcePolicy` + curl diagnostics** | ⏳ **In flight** | Typed policy gating every desktop fetch (schemes, host allow/deny, size/time caps, sub-resource policy, first-use host prompt). Clear actionable error when `http(s)://` is typed but `curl` is not on `PATH`. WASM build ignores the policy — browser enforces it. |

## Summary

Donner's editor today parses and renders untrusted SVG in-process. As the editor
grows a user-facing **address bar** that loads arbitrary `https://` URLs and local
`file://` paths, the parser — Donner's largest fuzzer-exposed surface — becomes
the single most attractive target in the binary. A parser crash or memory
corruption would take the editor (and the user's unsaved work) with it.

This design puts the parser, DOM, and `RendererDriver` in a **separate
sandboxed child process**, mirroring the browser process model. The clever
part: we already have `RendererInterface` — a pure-virtual, value-typed,
stateful-but-LIFO command surface with 28 methods — and `RendererRecorder` is
already planned as a tee wrapper. **Those two facts together mean the
`RendererInterface` is already an RPC protocol in disguise.** We make it
literal:

1. Sandbox process runs parser + `RendererDriver` + a `SerializingRenderer`
   that encodes each virtual call as a wire message.
2. Editor host runs `ReplayingRenderer` that decodes the wire stream and
   invokes the **real** backend (Skia, TinySkia, Geode).
3. The on-wire format is the same format `RendererRecorder` writes, so
   **record/replay is free** — it's just "tee the IPC socket into a file."
4. Because every render command is addressable by index in the stream, the
   editor gets a **frame inspector**: pause the IPC pump, show the recorded
   command list in an ImGui panel, scrub a slider, replay commands `[0..N)`
   to a headless backend, blit the result.

The C++26-reflection angle is specifically about the marshalling layer: with
reflection, we can derive the serializer for each method's parameter struct
mechanically instead of writing 28 hand-rolled `Encode*`/`Decode*` pairs. Today
that's aspirational (tree is on C++20); we land the hand-rolled version first
and plan the reflection migration as a follow-up that deletes boilerplate
rather than changes behavior.

Address bar is the motivating UX: "type a URL, see it render, crash the child
process if it's malicious, recover transparently." It is the forcing function
for the sandbox; the sandbox is the forcing function for the IPC layer; the IPC
layer is the forcing function for record/replay and the frame inspector. The
whole stack falls out of one decision.

## Goals

- **Isolate parsing**: `SVGParser::ParseSVG` runs in a child process with no
  filesystem, network, or IPC access beyond its stdin/stdout pipes. A crash or
  memory corruption in the parser never takes the editor down.
- **Address bar**: editor gains a URL bar that accepts `https://`, `http://`,
  and `file://` URIs. Loads are always routed through the sandbox.
- **RendererInterface as IPC**: the 28-method `RendererInterface` becomes the
  wire protocol between sandbox and host. No other IPC surface exists between
  the two processes.
- **RendererRecorder → file format**: promote the planned in-memory
  `RendererRecorder` into a serializable format (`.rnr`, "Renderer Recording")
  that round-trips across the IPC boundary and to disk.
- **Frame inspector**: ImGui panel inside the editor that shows the command
  stream for the current frame, allows pausing before `endFrame()`, scrubbing
  to command index `N`, and inspecting individual command arguments.
- **Determinism**: replaying a `.rnr` file against the same backend at the
  same viewport must be pixel-identical to the original render.
- **No C++26 dependency on day one**: hand-rolled marshallers ship first;
  reflection is a follow-up that preserves the wire format.

## Non-Goals

- **Not a full browser sandbox.** We're not using `seccomp-bpf`, AppArmor, or
  SELinux policies in the initial milestone — just OS process separation,
  closed file descriptors, and a dropped working directory. Hardening is
  staged.
- **Not cross-origin CSS/font/image fetches.** The sandbox child parses SVG
  *bytes*; it never initiates network requests. The host fetches the bytes
  over HTTPS and hands them to the sandbox. Sub-resource URL fetching (e.g.
  `<image href="https://...">`) is Future Work.
- **Not WASM-compatible.** The sandbox uses Unix pipes and `posix_spawn`; the
  editor's WASM build (M6) continues to run the parser in-process, with the
  browser's own sandbox as the trust boundary. The IPC abstraction exists,
  but `SandboxedParseSVG` is a direct call in WASM builds.
- **Not Windows in milestone 1.** Editor M3 targets macOS + Linux; the
  sandbox follows the same platform cut. Windows is Future Work.
- **Not a replacement for the fuzzer.** `SVGParser_fuzzer` continues to find
  bugs at the parser level. The sandbox is defense-in-depth, not defense-in-first.
- **Not a general-purpose IPC framework.** This is one protocol
  (`RendererInterface`), not a reusable RPC system. No service discovery, no
  versioning handshake beyond a magic-number header, no bidirectional method
  calls (the sandbox is strictly a producer of commands).

## Next Steps

- [ ] Land Editor M2 (mutation seam, `AsyncSVGDocument` command queue) —
  sandbox depends on the host-side command queue as the insertion point.
- [ ] Land `RendererRecorder` as specified in
  [renderer_interface_design.md:242](./0003-renderer_interface_design.md) — the
  in-memory variant is the foundation for the wire format.
- [x] Prototype **Milestone S1** (byte-level sandbox loopback, no IPC yet):
  parse in a child process, return rendered PNG bytes over stdout. This
  proves the process model before committing to the RendererInterface wire
  protocol. *Landed in #506; S2 superseded it with the framed wire format.*

## Implementation Plan

- [x] **Milestone S1: Byte-in, PNG-out child process (proof of model)** — landed in #506/#518
  - [x] New binary `//donner/editor/sandbox:donner_parser_child`, no ImGui,
        no GL, no renderer backend. Reads SVG bytes from stdin, parses,
        renders to an in-memory bitmap via `RendererTinySkia::createOffscreenInstance()`,
        writes PNG bytes to stdout, exits.
  - [x] Host-side `SandboxHost` class in `//donner/editor:sandbox_host` that
        `posix_spawn`s the child with `stdin`/`stdout` piped, writes SVG
        bytes, reads PNG bytes, returns them. Synchronous API.
  - [x] Crash test: feed `SVGParser_fuzzer` corpus through `SandboxHost`;
        host must never crash, only observe non-zero exit codes.
        (`SandboxHost_tests.cc`)
  - [x] **Decision gate**: measured process spawn vs render; kept the
        long-lived child model from S3 on. No pool required for p50 corpus.

- [x] **Milestone S2: RendererInterface wire format** — landed in #506/#518
  - [x] Define `donner::editor::sandbox::Wire` — framed format in `Wire.h`.
        Header: 4-byte magic `DRNR`, 4-byte version, 4-byte length, 4-byte
        opcode, payload. Variable-length fields length-prefixed.
  - [x] Enumerate opcodes in `Wire.h` — one per `RendererInterface` virtual
        method plus `kStreamHeader`, `kBeginFrame`/`kEndFrame`, `kError`.
  - [x] `SerializingRenderer : RendererInterface` writes opcode + marshalled
        payload to a buffer (`SerializingRenderer.cc`).
  - [x] `ReplayingRenderer` reads a buffer, decodes, and forwards to a
        wrapped `RendererInterface&` (`ReplayingRenderer.cc`).
  - [x] **Round-trip test** in `WireFormat_tests.cc` and
        `RecordReplay_tests.cc`: `RendererTinySkia → serialize → deserialize
        → RendererTinySkia` is byte-identical.
  - [x] Hand-rolled marshallers for `PathShape`, `PaintParams`,
        `ResolvedClip`, `ImageParams`, `TextParams`, `FilterGraph`,
        `StrokeParams`, `ResolvedPaintServer`, `ResolvedMask` in
        `SandboxCodecs.cc` (~2 kLOC — the number S5 reflection will delete).
  - [~] Fuzz the deserializer. New target `sandbox_wire_fuzzer` that feeds
        random bytes into `ReplayingRenderer` and asserts it never crashes
        — only returns `kError`. This is non-negotiable: the deserializer is
        the trust boundary. *In review in #528* — `SandboxWire_fuzzer.cc`
        drives `ReplayingRenderer::pumpFrame` against a `RendererTinySkia`
        sink; initial seed corpus produced from `SerializingRenderer` on a
        trivial SVG.

- [x] **Milestone S3: Long-lived child + address bar** — landed in #506/#518
  - [x] Long-lived `donner_parser_child` that reads a `ParseRequest` and
        streams a `SerializingRenderer` command sequence over stdout until
        `kEndOfFrame`, then loops.
  - [x] Host-side `SandboxHost::render(...)` +
        `PipelinedRenderer` that pumps commands into the host's real
        renderer via `ReplayingRenderer` (`PipelinedRenderer_tests.cc`).
  - [x] **Address bar UI** in `EditorApp` / `EditorWindow`:
        - ImGui `InputText` at the top of the viewport.
        - Accepts `https://`, `http://`, `file://`, and bare paths via
          `SvgSource`.
        - Dispatch: file reads on the host; HTTPS fetched by shelling out
          to the system `curl` CLI (no `libcurl` link dep). The sandbox
          only ever sees raw bytes.
        - Status chip: `Loading…`, `Rendered`, `Crashed (sandbox)`,
          `Parse error`, etc.
  - [x] **Crash recovery**: if the sandbox child exits non-zero, show the
        error chip, keep the previously-rendered document on screen, and
        respawn the child on the next navigation.
  - [ ] Wire the address bar into `fuzz_replay_cli` from editor M5: every
        fuzzer corpus entry gets fed through the address bar code path,
        asserting host-process liveness. **Still TODO** (waiting on M5).

- [x] **Milestone S4: Record/replay + frame inspector** — landed in #506/#518
  - [x] `.rnr` file format in `RnrFile.{h,cc}` — Wire framing + header
        block (viewport, backend hint, timestamp, originating URL).
        Round-trip test in `RecordReplay_tests.cc`.
  - [x] Recording path: `SerializingRenderer` output is persisted
        directly to disk via `SaveRnrFile`. Replay reads it back and pumps
        it through `ReplayingRenderer` — pixel-identical to an in-process
        render (`RecordReplayEndToEndTest.RoundTripPreservesPixels`).
  - [x] CLI replay: `//donner/editor/sandbox:sandbox_replay_main` loads a
        `.rnr` file and replays it through the host backend.
  - [x] **Frame inspector pane**: `FrameInspector.{h,cc}` decodes a wire
        stream into `std::vector<Command>` with opcode, depth, and
        per-opcode summary strings. `FrameInspector::ReplayPrefix` is the
        scrub-slider primitive and is tested against direct-render
        parity for full and partial prefixes.
  - [x] CLI inspection: `//donner/editor/sandbox:sandbox_inspect_main`
        dumps a decoded wire stream as text.
  - [~] Structural-diff mode: load two `.rnr` files, show a diff of their
        command streams side-by-side. *In review in #527* — new
        `sandbox_diff` CLI plus a reusable `SandboxDiff` library that
        compares `RnrHeader` fields and aligns the two decoded command
        streams with a plain LCS. Four unit tests cover identical,
        differing draw call, header mismatch, and inserted command.

- [ ] **Milestone S5: C++26 reflection migration (aspirational, gated)**
  - [ ] Bump `.bazelrc` `--config=c++26` (new config, not default). Verify
        with the current `latest_llvm` toolchain. If not yet available,
        park this milestone.
  - [ ] Replace hand-rolled `Encode<PathShape>` / `Decode<PathShape>` etc.
        with a single `Encode<T>` / `Decode<T>` template that iterates
        `std::meta::nonstatic_data_members_of(^^T)`.
  - [ ] **Wire format must not change.** This milestone is a code reduction,
        not a protocol change. Gate on the round-trip test from S2 continuing
        to pass byte-for-byte.
  - [ ] Measure LOC delta. Target: delete at least 60% of the hand-rolled
        marshaller code from S2.

- **Milestone S6: Sandbox hardening (defense in depth)**
  - [x] **S6.1 — portable hardening** (landed in #506/#518, Linux gating
        refined in `5f66ea4f`, in-source skip guards in `469bf576`):
    - `SandboxHardening.{h,cc}` — `ApplyHardening()` called from the
      child's `main()` before any untrusted input is read.
    - Environment gate: child refuses to start unless `DONNER_SANDBOX=1`.
    - `chdir("/")` to strip relative-path authority.
    - FD sweep closes all inherited descriptors above stderr.
    - `setrlimit` caps: `RLIMIT_AS` (1 GiB default), `RLIMIT_CPU` (30 s),
      `RLIMIT_FSIZE` (0 = no regular-file writes), `RLIMIT_NOFILE` (16).
    - Linux: `seccomp-bpf` allowlist installed in **fail-open** mode —
      denied syscalls return `-EACCES` today. Allowlist was widened in
      `e7ae6d90` after initial CI failures on x86_64 runners.
    - Tests: `SandboxHardening_tests.cc` covers env gate, rlimits, and
      subprocess integration. `sandbox_hardening_tests`, `sandbox_host_tests`,
      and `sandbox_pipeline_tests` targets are gated to Linux in
      `tests/BUILD.bazel` via `target_compatible_with @platforms//os:linux`;
      the two seccomp/envp subprocess tests also carry in-source
      `GTEST_SKIP()` guards as defense-in-depth so the file is still
      readable on macOS checkouts.
  - [ ] **S6.2 — platform jails** (pending):
    - [ ] Linux: flip seccomp-bpf from fail-open (`-EACCES`) to
          `SECCOMP_RET_KILL_PROCESS` once the allowlist has baked.
    - [ ] macOS: `sandbox_init` with a deny-all profile allowing only
          stdin/stdout pipes.
    - [ ] Run child under its own UID if the editor is launched with
          sufficient privilege (optional, off by default).
    - [ ] No `/proc` access, no filesystem access, no network — assert
          via integration tests on Linux (open a file, expect `EACCES`).

## S7–S11: the editor API is the process boundary (single-PR scope)

S1–S4 proved the sandbox model on a side binary. The real editor
(`donner/editor/main.cc`) still calls `SVGParser::ParseSVG` directly on
the main thread for every load and every source-pane edit. S7–S11 fix
that in a **single PR** with a crisp goal statement:

> **The editor's public API is the only IPC boundary. An
> `EditorBackend` child process owns the parser, `SVGDocument`,
> selection, undo timeline, tool dispatch, and renderer driver; the
> host process owns the text editor, viewport, address bar, chrome
> overlays, and status UI. The host *calls the editor API remotely*;
> the backend *never reaches out* to the host for anything except the
> frame-response bundle and occasional async pushes.**

This is a deliberately stronger claim than "move the parser out." It
says the IPC shape mirrors what `EditorApp` already exposes today to its
caller (`loadFromString`, `applyMutation`, `hitTest`, `undo`, etc.) —
just serialized, not the underlying DOM. The key consequence: there is
no `DomMirror`, no `EntityHandle`, no host-side parsed-tree reflection.
Selection, hit-test, tree traversal, writeback accounting — all of it
lives inside the backend's `SVGDocument`, and the host never sees past
the API.

### Mental model

```
┌────────────────────────────────────────┐          ┌──────────────────────────────────────┐
│ Host process (UI client)               │          │ EditorBackend child (untrusted)      │
│                                        │          │                                      │
│  Address bar ───┐                      │          │  donner::editor::EditorApp           │
│  Text editor ───┤                      │          │   ├── AsyncSVGDocument + SVGParser   │
│  Viewport   ─── │─ events ───┐         │ events   │   ├── Tools (SelectTool, …)          │
│  Status     ────┘            │         ├─────────▶│   ├── UndoTimeline                   │
│  Chrome ◀── overlay payload ─│─ Frame ◀┤          │   ├── AttributeWriteback             │
│                              │         │  Frame   │   └── RendererDriver                 │
│                              │         │          │         │                            │
│                              │         │          │         ▼                            │
│                              │         │          │    SerializingRenderer               │
│                              │         │          │                                      │
└──────────────────────────────┴─────────┘          └──────────────────────────────────────┘
```

### What each process owns

| Concern | Owner | Notes |
|---|---|---|
| **Parser + `SVGDocument` + entt registry** | Backend | Never materialized on host. |
| **Selection** | Backend | Host never holds `svg::SVGElement`. It draws chrome from a per-frame `SelectionOverlay` payload (bboxes + handle hints). |
| **Hit test / tools (`SelectTool`, path tools)** | Backend | A `PointerEvent` routed to the backend is processed by the active tool and updates the document + selection atomically. Host's mouse handler just forwards events. |
| **Undo / redo timeline** | Backend | `Undo()` / `Redo()` are API messages. The backend replays against its document, ships back a frame + source-writeback if the undone change touched the source. |
| **Attribute writeback (canvas → source)** | Backend (compute) + Host (apply) | Backend computes the `{source_range, new_text}` patch when a drag commits and ships it as part of the next `Frame`. Host splices it into its `TextEditor` so the source pane stays in sync. |
| **Render driver + wire stream** | Backend | Existing `SerializingRenderer` continues to produce the DRNR stream. The host `ReplayingRenderer` stays on host. `.rnr` is literally the rendering wire stream, unchanged. |
| **Text editor (`TextEditor`, `TextBuffer`, `TextPatch`)** | Host | Cursor, scroll, text-pane undo — all host-local. On text edits the host sends `ReplaceSource(bytes)` or `ApplySourcePatch(range, new_bytes)` to the backend. |
| **Address bar, resource policy, fetchers** | Host | Orthogonal to backend; host fetches bytes and hands them off as `LoadBytes(bytes)`. |
| **Frame inspector / `.rnr` record + replay** | Host (UI) + Backend (producer) | Inspector pane decodes the frame's render wire locally. Export writes the same wire to disk. Replay is unchanged — it's a `.rnr` file playing through `ReplayingRenderer` on the host. |

### Why one PR

Splitting this across four PRs would leave the editor in hybrid states
where *some* operations go over IPC and others don't, with no
enforcement mechanism preventing new host-side parse calls. A single PR
lets the banned-pattern lint land on the first commit and makes every
call site visibly trace through the new API.

The PR is large but structurally simple: almost every editor file loses
its direct document access and gains an `EditorBackendClient&`. The
backend side is *just the existing `donner::editor::EditorApp` class
run in a new binary*, driven by a request loop that unpacks API
messages. No new editor behavior — just a new process boundary.

## S7: Long-lived `SandboxSession` (transport layer)

### Why `SandboxHost` must become stateful

`SandboxHost::renderToBackend` today spawns `donner_parser_child` with
`posix_spawn`, writes the SVG bytes, reads stdout to EOF, and waits for
the child to exit. One render = one process. That's a non-starter for
an editor where every click or keystroke is an IPC round-trip: a 50 ms
`posix_spawn` for each pointer-move event would kill drag latency.

S7 replaces the one-shot model with a `SandboxSession` — a transport
class that owns a single persistent child and multiplexes framed
request/response bytes over its stdin/stdout pipes. **The session
doesn't know or care what payload the messages carry.** The editor-API
layer (S8) sits on top, encoding/decoding the session payloads.

Binary: the persistent child is a new binary, `donner_editor_backend`,
that runs a `donner::editor::EditorApp` inside the child and the
request loop described in S8. The existing one-shot `donner_parser_child`
stays for the CLI tools (`sandbox_render_main`, `sandbox_replay_main`)
and the fuzzer harnesses; nothing in the full editor uses it after S9.

### Shape

```cpp
namespace donner::editor::sandbox {

class SandboxSession {
 public:
  explicit SandboxSession(std::string childBinaryPath,
                          SandboxSessionOptions opts = {});
  ~SandboxSession();  // sends kShutdown, joins reader/writer, SIGKILLs on timeout.

  // Submits a request to the child. Returns a future that resolves with
  // the wire-formatted response when the child finishes that request.
  // Requests are serviced in FIFO order; the caller can have multiple
  // requests in flight but the sandbox pipeline processes them sequentially.
  [[nodiscard]] std::future<WireResponse> submit(WireRequest request);

  // True iff the child is currently alive. A false reading here does not
  // guarantee requests will fail — the session respawns transparently on
  // the next submit.
  [[nodiscard]] bool childAlive() const;

  // Snapshot of the most recent abnormal exit, or nullopt if the current
  // child has been running cleanly. Used by the editor's status chip.
  [[nodiscard]] std::optional<ExitInfo> lastExit() const;
};

}  // namespace donner::editor::sandbox
```

Internals:

- **Writer thread**: drains an inbox `SPSC` queue; frames each request
  and writes the bytes to the child's stdin. Blocks on the kernel pipe
  when the child falls behind (backpressure).
- **Reader thread**: decodes message framing from the child's stdout;
  dispatches each response to the `std::promise` for the matching
  request id. Also surfaces async `kDiagnostic` messages to a callback
  the editor wires to its console pane.
- **Respawn policy**: if the reader sees `read() == 0` (child exited)
  or an exit classified as `kCrashed`, the session:
  1. marks all in-flight futures with `SandboxStatus::kCrashed`,
  2. rebuilds the `posix_spawn` + pipes,
  3. marks `childAlive() = true` and resumes accepting new requests.
  The respawn is transparent to callers — they see their current
  request fail and can retry on the next frame.

### Back-compat

`SandboxHost::renderToBackend` stays as a thin shim over
`SandboxSession::submit(kRenderRequest{…}).get()` so existing tests and
the `sandbox_render_main` / `sandbox_replay_main` CLIs keep working
without changes. The one-shot semantics continue to be available via an
explicit `SandboxSession::renderOnce` helper that spawns a throwaway
session — useful for the fuzzer harnesses.

### Tests

- `SandboxSession_tests.cc`:
  - N successive renders share a single child (check `child_pid` stable
    across calls).
  - Child crash during render → caller sees `kCrashed` → next render
    succeeds against a freshly-spawned child.
  - Concurrent-in-flight requests serialize correctly (submit A, submit
    B, await A, await B → both succeed, B's response follows A's).
  - Clean shutdown: session destructor sends `kShutdown` and the child
    exits with `kExitOk` within 100 ms.

## S8: Editor-API wire protocol

The protocol is shaped by the editor's existing public API, not by the
DOM underneath. Every request corresponds to something an editor UI
already does today — "I clicked at (x,y)", "load these bytes", "undo".
Every response is *the same frame bundle*: the render wire stream plus
whatever extra info the host needs to update its chrome / source pane /
status strip.

### Session-layer framing

Unchanged from the S7 `SessionProtocol.h` framing:

```
u32 magic     = 'DRNS'
u64 requestId
u32 opcode    (SessionOpcode, below)
u32 payloadLength
u8  payload[payloadLength]
```

### Request opcodes (host → backend)

| Opcode | Payload | Behavior |
|---|---|---|
| `kHandshake` | `{protocolVersion, buildId}` | First message. Backend replies with `kHandshakeAck` or hangs up. |
| `kSetViewport` | `{width, height}` | Initial + on window resize. Backend re-renders and returns a fresh `Frame`. |
| `kLoadBytes` | `{bytes, optional<origin_uri>}` | Fresh-document load. Clears undo. Reply is a `Frame` or an `Error{kParseError, diagnostics}`. |
| `kReplaceSource` | `{bytes, preserveUndoOnReparse}` | Full-regen text-pane edit. Pipes into `EditorApp::applyMutation(ReplaceDocumentCommand{...})`. |
| `kApplySourcePatch` | `{sourceStart, sourceEnd, newBytes}` | Structured-edit fast path (M5). Pipes into `SetAttributeCommand` when the backend classifies it as an attribute-local edit; falls back to `ReplaceSource` with diagnostics otherwise. |
| `kPointerEvent` | `{phase, documentPoint, buttons, modifiers, toolState}` | phase ∈ {Down, Move, Up, Enter, Leave, Cancel}. Routed into the backend's active tool; the tool mutates selection / transform / state. |
| `kKeyEvent` | `{phase, keyCode, modifiers, textInput}` | Document-scoped shortcuts only (Delete, Esc, arrow-nudge, Ctrl+Z/Y). UI-only shortcuts (Ctrl+L focus address bar, Tab pane switch) never leave the host. |
| `kWheelEvent` | `{documentPoint, deltaX, deltaY, modifiers}` | Pan/zoom when the viewport tool isn't the Select tool. |
| `kSetTool` | `{toolKind}` | SelectTool / RectTool / PathTool / etc. Backend's active tool is the only place new editor logic lives. |
| `kUndo` / `kRedo` | `{}` | Pass-through to `EditorApp::undo()` / `redo()`. Reply includes a `SourceReplaceAll` if the restored state's source differs from the host's buffer. |
| `kExport` | `{format}` | Request the current source bytes (SVG) or a rendered raster. Reply is `kExportResponse{bytes}`. |
| `kShutdown` | `{}` | Graceful exit. Backend replies with `kShutdownAck` then exits. |

### Response opcodes (backend → host)

| Opcode | Payload | When sent |
|---|---|---|
| `kHandshakeAck` | `{protocolVersion, buildId, backendCapabilities}` | Once, after `kHandshake`. Buildid mismatch → host closes session. |
| `kFrame` | `{frameId, renderWire, selectionOverlay, sourceWritebacks, statusChip, parseDiagnostics}` | **The default response** to every request that mutates the document or viewport. One frame per logical user action. |
| `kSourceReplaceAll` | `{bytes}` | Sent alongside a `kFrame` when the backend's source has diverged from the host's (e.g. after `Undo`, after an unsolicited canonicalization). Host's `TextEditor` must adopt these bytes as the new baseline. |
| `kExportResponse` | `{format, bytes}` | Only in reply to `kExport`. |
| `kToast` | `{severity, message}` | Unsolicited async push. Host shows a one-line chip. |
| `kDialogRequest` | `{kind, payload}` | "Please show a save dialog for me" — the host owns the OS integration. |
| `kDiagnostic` | `{string}` | Developer-channel logging. |
| `kError` | `{SessionErrorKind, message}` | Fatal-per-request protocol error or a user-visible load/save failure. |
| `kShutdownAck` | `{}` | Terminal. |

### `Frame` payload

This is the backend's universal response shape. Sent in reply to every
mutating request (pointer event, load, undo, …) and optionally pushed
unsolicited if the backend has an async update (e.g. the fuzzer harness
pushing frames on a timer). The fields:

```
u64 frameId
u32 renderWireLength
u8  renderWire[renderWireLength]    // DRNR stream, same as today.

// Selection overlay the host must draw on top of the render. Bboxes are
// in document space; host applies its viewport transform.
u32 selectionCount
Selection selections[selectionCount] {
  f64 worldBBox[4]                  // minX, minY, maxX, maxY.
  u8  hasTransform
  f64 worldTransform[6]             // For rotated/skewed selection rects.
  u8  handleMask                    // Which resize handles to draw (bitmask).
}

// Marquee (drag-select rectangle), present only while a drag is in flight.
u8  hasMarquee
f64 marqueeRect[4]                  // Document space.

// Source writebacks the host should splice into its TextEditor. Multiple
// when a single user action affects several attributes (e.g. resize).
u32 writebackCount
Writeback writebacks[writebackCount] {
  u32 sourceStart                   // Byte offset in host's current buffer.
  u32 sourceEnd
  u32 newTextLength
  u8  newText[newTextLength]
  // Reason tag, for the host to decide whether to coalesce into a single
  // TextEditor undo entry.
  u8  reasonKind                    // 0=attributeEdit, 1=canonicalization, 2=elementRemoval
}

// Status chip overrides. When none, the host falls back to a default chip.
u8  statusKind                      // AddressBarStatus enum value.
u32 statusMessageLength
u8  statusMessage[statusMessageLength]

// Parse diagnostics. Empty on success.
u32 parseDiagnosticCount
Diagnostic diagnostics[parseDiagnosticCount] {
  u32 line
  u32 column
  u32 messageLength
  u8  message[messageLength]
}

// Cursor hint: source offset corresponding to the topmost element
// under a recent pointer event. Lets the host highlight the matching
// <tag> span in the source pane without a second round trip.
u8  hasCursorHint
u32 cursorHintSourceOffset
```

All fields are length-prefixed. The decoder on the host caps every
length field against the S2 `kMax*` constants.

### The backend's request loop

The backend binary (`donner_editor_backend`) runs a single thread that:

1. Reads a framed request from stdin.
2. Dispatches via a switch on `SessionOpcode`:
   - Input events → `EditorApp::applyMutation` / tool dispatch.
   - `kLoadBytes` / `kReplaceSource` → `AsyncSVGDocument::loadFromString`.
   - `kUndo` → `EditorApp::undo()`.
   - `kExport` → serialize + reply.
3. After the mutation settles, drains `applyPending*Writeback()` queues
   into `Frame.writebacks`.
4. Runs `RendererDriver` with a `SerializingRenderer` to emit
   `Frame.renderWire`.
5. Encodes the `Frame` and writes it to stdout.
6. Loops.

Only one request is processed at a time. Concurrent requests from the
host queue up (the session's writer thread feeds them FIFO). This keeps
the backend's document state race-free without locks.

### Why this is smaller than a DOM-mirror protocol

In the DOM-mirror approach the host carried a full in-memory tree, and
*every* read went through the mirror. That required ~9 new wire
message types, a ~1600-LOC tree codec, and a host-side
`DonnerController` analogue.

In the API-boundary approach the host carries **no document state**
beyond per-frame chrome hints. The codec is mechanical: each opcode is
a small value struct, and there is no tree to serialize. Rough
estimates:

| Layer | DOM-mirror approach | API approach |
|---|---|---|
| Wire message types | 9 | 13 (but each one is trivial) |
| Codec LOC | ~1600 | ~600 |
| Host-side tree types | `DomMirror`, `MirrorElement`, `EntityHandle` (~800) | `SelectionOverlay` struct (~60) |
| Editor refactor | `svg::SVGElement` → `EntityHandle` across ~50 call sites | Document-touching editor code moves *wholesale* into the backend binary; host loses the reference entirely (~30 call sites, deleted) |

### Fuzzing

Two new fuzzers replace the mirror-era plan:

1. **`editor_backend_request_fuzzer`**: feeds random bytes into the
   backend's request decoder. Asserts the backend never crashes — it
   may reject malformed requests via `kError`, but `SIGSEGV` / `SIGABRT`
   is unacceptable. Seed corpus = recorded editor sessions from the
   existing test suite.
2. **`frame_response_fuzzer`**: feeds random bytes into the host's
   `Frame` decoder + `EditorBackendClient::onFrame` handlers. Same
   non-crash invariant; this is the trust boundary on the host side.

`sandbox_wire_fuzzer` (#528) still covers the inner render wire; it
becomes a component fuzzer for the `renderWire` field inside `kFrame`.

## S9: Host editor becomes a thin client

### The new `donner_editor_backend` binary

A new Bazel target `//donner/editor/sandbox:donner_editor_backend`
links:

- `donner/editor:editor_lib` — the existing `donner::editor::EditorApp`,
  `AsyncSVGDocument`, tools, undo, writeback, change-classifier —
  **unchanged**.
- `donner/editor/sandbox:serializing_renderer` — the already-landed
  `SerializingRenderer` producing the DRNR stream.
- `donner/editor/sandbox:session_codecs` (new) — encoders/decoders
  for the S8 messages.
- `donner/editor/sandbox:hardening` — existing `ApplyHardening()`
  so the backend inherits the same S6 jail.

`main()` applies hardening, runs the request-loop from §S8, and
`EditorApp` is a regular C++ object inside the binary. No ECS or
parser code needs to know that it's running under IPC.

### The host-side `EditorBackendClient`

```cpp
namespace donner::editor {

class EditorBackendClient {
 public:
  EditorBackendClient(sandbox::SandboxSession& session);
  ~EditorBackendClient();

  // High-level requests — the host UI calls these directly. Each returns
  // a future that resolves when the corresponding Frame arrives.
  std::future<FrameResult> loadBytes(std::span<const uint8_t> bytes,
                                     std::optional<std::string> originUri);
  std::future<FrameResult> replaceSource(std::string bytes,
                                         bool preserveUndoOnReparse);
  std::future<FrameResult> applySourcePatch(uint32_t start, uint32_t end,
                                            std::string newText);
  std::future<FrameResult> pointerEvent(const PointerEventPayload&);
  std::future<FrameResult> keyEvent(const KeyEventPayload&);
  std::future<FrameResult> wheelEvent(const WheelEventPayload&);
  std::future<FrameResult> setTool(ToolKind);
  std::future<FrameResult> setViewport(int width, int height);
  std::future<FrameResult> undo();
  std::future<FrameResult> redo();
  std::future<ExportResult> exportSource();

  // Callbacks for unsolicited backend pushes.
  using ToastCallback = std::function<void(ToastPayload)>;
  using DialogRequestCallback = std::function<void(DialogRequestPayload)>;
  void setToastCallback(ToastCallback);
  void setDialogRequestCallback(DialogRequestCallback);

  // Runtime state surfaced from the last Frame.
  [[nodiscard]] const SelectionOverlay& selection() const;
  [[nodiscard]] const svg::RendererBitmap& latestBitmap() const;
  [[nodiscard]] std::optional<ParseDiagnostic> lastParseError() const;
};

}  // namespace donner::editor
```

The client consumes exactly one `SandboxSession` and owns the
host-side replay surface (`ReplayingRenderer` + `Renderer`) so each
incoming `Frame` produces a `RendererBitmap` for the viewport.

### `FrameResult`

```cpp
struct FrameResult {
  bool ok = false;
  uint64_t frameId = 0;
  // Host view of the bitmap produced by replaying Frame.renderWire.
  svg::RendererBitmap bitmap;
  // Updated selection chrome — host's OverlayRenderer draws from this.
  SelectionOverlay selection;
  // Source writebacks that arrived with this frame; host's TextEditor
  // applies them to stay in sync with the backend.
  std::vector<SourceWriteback> writebacks;
  // Optional full-source replacement (after undo/redo across divergent
  // source states).
  std::optional<std::string> sourceReplaceAll;
  // Status chip override for the address bar (optional).
  std::optional<AddressBarStatusChip> statusChip;
  std::vector<ParseDiagnostic> diagnostics;
};
```

### What moves out of `donner/editor/*.cc` into the backend

The following files/classes are **moved bodily into the backend
target** and deleted from the host editor library:

- `EditorApp.{h,cc}` — the full class.
- `AsyncSVGDocument.{h,cc}`
- `CommandQueue.{h,cc}`, `EditorCommand.h`
- `ChangeClassifier.{h,cc}`
- `SelectTool.{h,cc}`, `Tool.h`, `PinchEventMonitor*`
- `UndoTimeline.{h,cc}`
- `AttributeWriteback.{h,cc}`
- `RenderPaneGesture.{h,cc}` — tool gestures are backend-side now.
- `SelectionAabb.{h,cc}`, `ViewportGeometry.{h,cc}`, `ViewportState.{h,cc}`
  for the pieces that depend on the document; the parts that only
  depend on viewport math stay host-side.

### What stays (host-side editor library)

- `TextEditor.{h,cc}`, `TextBuffer.h`, `TextEditorCore.{h,cc}`,
  `TextPatch.{h,cc}` — text editor is pure source-text manipulation.
- `ImGuiClipboard.{h,cc}`, `InMemoryClipboard.h`, `ClipboardInterface.h`.
- `OverlayRenderer.{h,cc}` — but it takes a `const SelectionOverlay&`
  rather than an `EditorApp&`.
- `TracyWrapper.h`.
- `gui/EditorWindow.{h,cc}` (the RAII GLFW+ImGui wrapper).
- **New**: `EditorBackendClient.{h,cc}`, `AddressBar.{h,cc}`,
  `ResourcePolicy.{h,cc}`.

### The `main.cc` refactor

`donner/editor/main.cc` today hosts the document directly via
`AsyncSVGDocument`. Post-S9:

```cpp
int main(int argc, char** argv) {
  auto gatekeeper = ResourceGatekeeper(DefaultDesktopPolicy());
  auto session = sandbox::SandboxSession({
      .childBinaryPath = runfiles::Resolve("donner_editor_backend"),
  });
  EditorBackendClient backend(session);
  AddressBar addressBar;
  TextEditor textEditor;

  if (argc > 1) {
    auto decision = gatekeeper.resolve(argv[1]);
    // dispatch a fetch + backend.loadBytes(...)
  }

  EditorWindow window({.title = "Donner Editor"});
  while (!window.shouldClose()) {
    window.pollEvents();
    window.beginFrame();
    drawAddressBar(addressBar, backend, gatekeeper, ...);
    drawTextEditorPane(textEditor, backend, ...);
    drawViewport(backend.latestBitmap(), backend.selection(), ...);
    drawStatusChip(backend, ...);
    window.endFrame();
  }
}
```

The loop is ~100 lines. All the former editor complexity lives in
the backend; the host is a relay and a renderer of frames the backend
ships back.

### The WASM build

WASM can't `posix_spawn`. Instead:

- `EditorBackendClient` has two implementations behind the same header:
  `EditorBackendClient_Session.cc` (desktop, talks to `SandboxSession`)
  and `EditorBackendClient_InProcess.cc` (WASM, calls an in-process
  `donner::editor::EditorApp` directly).
- The **in-process WASM variant statically links the backend
  library** — same `EditorApp`, same tools, same codecs — but skips
  the wire encode/decode. It's the "backend running in the same
  address space as the client" case.
- The browser is the sandbox. `SVGParser::ParseSVG` runs in the WASM
  module, which is exactly what the browser's sandboxing was
  designed for.

This means:

- The WASM build compiles the banned-pattern lint-exempt
  `EditorBackendClient_InProcess.cc` (which *is* allowed to call into
  the backend library, which in turn calls `SVGParser::ParseSVG`).
- The desktop build compiles `EditorBackendClient_Session.cc` (no
  parser access).
- Both builds ship the same `EditorBackendClient` header surface, so
  `donner/editor/main.cc` is shared verbatim between desktop and WASM.

### `donner_editor_gui_main.cc` deletion

After the `main.cc` refactor, the slim `gui/donner_editor_gui` binary
is strictly a subset of what `donner/editor/main.cc` now does. The
files disappear:

- `donner/editor/gui/donner_editor_gui_main.cc` — deleted.
- `donner/editor/app/EditorApp.{h,cc}` — deleted. Its `EditorApp`
  (slim, REPL-backing) is collapsed into `EditorBackendClient` since
  the client has the same "bytes in → frame out" shape.
- `donner/editor/app/EditorRepl.{h,cc}` — kept, but rewired to drive
  `EditorBackendClient` rather than the slim `app::EditorApp`.
- `donner/editor/sandbox/PipelinedRenderer.{h,cc}` — deleted. Its
  role was "in-process producer/consumer wire"; the new
  `EditorBackendClient_InProcess` path is the same idea, simpler.
- `donner/editor/sandbox/SvgSource.{h,cc}` — kept; moves into the
  host fetch path behind `ResourceGatekeeper`.

### Banned-pattern lint

New rule in `build_defs/check_banned_patterns.py`:

```python
_Rule(
    pattern=re.compile(r"SVGParser::ParseSVG"),
    description="SVGParser::ParseSVG call outside allowed hosts",
    remediation=(
        "Desktop editor host code must route every parse through "
        "EditorBackendClient (which talks to donner_editor_backend). "
        "Only the backend binary, the parser/engine itself, the "
        "EditorBackendClient_InProcess path (WASM), and tests may "
        "call SVGParser::ParseSVG directly."
    ),
    exempt_path_prefixes=(
        "donner/svg/",                              # Parser + engine.
        "donner/editor/sandbox/parser_child_main.cc",   # Existing one-shot child.
        "donner/editor/sandbox/editor_backend_main.cc", # The new backend binary.
        "donner/editor/backend_lib/",                # Backend-side editor code (moved from donner/editor).
        "donner/editor/EditorBackendClient_InProcess.cc",  # WASM path.
        "donner/editor/sandbox/tests/",
        "donner/editor/tests/",
        "examples/",
        "tools/",
    ),
)
```

The desktop editor's production code path (`donner/editor/*.cc` minus
the exempt list) has zero `SVGParser::ParseSVG` calls after S9. CI
enforces.

### Tests

- `EditorBackendClient_tests.cc`: round-trip every API message through
  both implementations (`_Session` and `_InProcess`); assert identical
  `FrameResult`s.
- `editor_backend_integration_tests.cc`: spawn a real backend,
  reproduce the scenarios covered by today's `EditorApp_tests.cc`
  (hit-test, multi-select, drag writeback, undo) through the wire.
- `editor_backend_request_fuzzer.cc`: see §S8 fuzzing.
- Existing `EditorApp_tests.cc` continues to live in
  `donner/editor/backend_lib/tests/` (moved alongside the class).

## S10: Unified address bar (desktop + WASM)

### Widget

A reusable `donner::editor::AddressBar` lives in the editor's core, not
in the platform shell:

```cpp
namespace donner::editor {

class AddressBar {
 public:
  AddressBar();

  // Call once per frame, after ImGui::Begin. Returns true if the user
  // triggered a navigation (Enter, Load, drag-drop, file-picker pick).
  // The navigation payload is retrievable via consumeNavigation().
  bool draw();

  struct Navigation {
    std::string uri;            // `file://`, `https://`, or bare path.
    std::vector<uint8_t> bytes; // Populated iff the UI shortcutted the
                                // fetch (file drop / picker on WASM); empty
                                // for "please call SvgSource".
  };
  [[nodiscard]] std::optional<Navigation> consumeNavigation();

  // Feedback from the editor: the most recent status chip state.
  void setStatus(StatusChip chip);

  // History — LRU of the last 16 URIs, session-only.
  void pushHistory(std::string uri);
};

}  // namespace donner::editor
```

- Ctrl/Cmd+L focuses the input.
- Enter loads; Esc cancels in-flight fetch (sends `kCancel` to the
  sandbox or aborts the emscripten_fetch handle).
- The file picker button opens a native dialog on desktop (via `nfd`,
  a dev-only dep) and an `<input type=file>` on WASM — WASM already
  has the plumbing (`gPendingBrowserUploadPath`) so this is a UI
  reskin on that build.
- Drag target accepts files (GLFW drop callback on desktop, HTML5
  drop on WASM) and URL strings (desktop only; WASM can't drag
  arbitrary URLs from the browser chrome).
- Status chip: `Loading…`, `Rendered`, `Crashed (sandbox)`,
  `Parse error (line N)`, `Fetch error`, `Policy denied`. Colors
  match the existing slim-shell chip.

### Layout

`donner/editor/main.cc` has a top dock area that today hosts the file
menu + document title. The address bar slots in as the first child of
that area, full-width, with the status chip right-aligned. The frame
inspector (previously only in `gui_main.cc`) becomes a bottom-dock
pane toggled from the View menu.

### Fetch plumbing

Two `SvgFetcher` implementations behind one interface:

```cpp
class SvgFetcher {
 public:
  virtual ~SvgFetcher() = default;
  virtual FetchHandle fetch(const ResolvedUri& uri, FetchCallback cb) = 0;
  virtual void cancel(FetchHandle h) = 0;
};
```

- **`SvgFetcherDesktop`** — wraps the existing `SvgSource` +
  `ResourceGatekeeper` (S11). HTTPS fetches still go through
  `popen("curl …")`; the curl-missing check (S11) fires before any
  fetch is attempted.
- **`SvgFetcherWasm`** — uses `emscripten_fetch_t` with async
  completion. Relies on CORS, HTTPS-only context rules, and the
  browser's mixed-content blocking for security. No `ResourcePolicy`
  is consulted — the explicit promise is "we trust the browser."

## S11: `ResourcePolicy` + curl diagnostics

### Policy type

```cpp
namespace donner::editor {

struct ResourcePolicy {
  bool allowHttps = true;
  bool allowHttp = false;      // Opt-in per session; plaintext default-deny.
  bool allowFile = true;
  bool allowData = false;      // data: URIs; parser-opaque, default off.

  // Filesystem scoping. When non-empty, all file reads must canonicalize
  // into one of these roots. Empty = anywhere (today's behavior), kept
  // as the default because a user typing an address bar URL consents.
  std::vector<std::filesystem::path> fileRoots;

  // HTTPS host policy. If `httpsAllowHosts` is non-empty it is the only
  // way through; otherwise any host not in `httpsDenyHosts` is accepted.
  // Patterns support exact match and "*.example.com" wildcards.
  std::vector<std::string> httpsAllowHosts;
  std::vector<std::string> httpsDenyHosts;
  bool httpsPromptOnFirstUse = true;

  // Size + time caps.
  std::size_t maxFileBytes = 100u * 1024u * 1024u;
  std::size_t maxHttpBytes = 10u * 1024u * 1024u;
  int httpTimeoutSeconds = 10;
  int maxRedirects = 5;

  // Sub-resource policy (reserved; subsresource fetch protocol is Future
  // Work per original doc).
  enum class SubresourcePolicy { kBlockAll, kSameDocumentOnly, kPolicyFetch };
  SubresourcePolicy subresources = SubresourcePolicy::kBlockAll;
};

class ResourceGatekeeper {
 public:
  explicit ResourceGatekeeper(ResourcePolicy policy);

  struct Decision {
    enum class Outcome { kAllow, kDeny, kNeedsUserConsent };
    Outcome outcome;
    std::string reason;           // Human-readable for the chip.
    ResolvedUri resolved;         // Canonicalized, safe to pass to SvgSource.
  };

  [[nodiscard]] Decision resolve(std::string_view uri) const;

  // Called by the UI after the user confirms a first-use HTTPS host.
  void grantHost(std::string host);
 private:
  ResourcePolicy policy_;
  std::unordered_set<std::string> grantedHosts_;  // Session-scoped.
};

}  // namespace donner::editor
```

`ResourceGatekeeper::resolve` is the *only* legal caller of
`SvgSource::fetch`; a compile-time audit of `svg_source` dependents
enforces that.

### Default policy

```cpp
ResourcePolicy DefaultDesktopPolicy() {
  return {
      .allowHttps = true,
      .allowHttp = false,
      .allowFile = true,
      .allowData = false,
      .fileRoots = {},                       // Anywhere.
      .httpsAllowHosts = {},                 // No allow-list → all hosts that aren't denied.
      .httpsDenyHosts = {},                  // Empty today; governed by the prompt.
      .httpsPromptOnFirstUse = true,
      .maxFileBytes = 100u * 1024u * 1024u,
      .maxHttpBytes = 10u * 1024u * 1024u,
      .httpTimeoutSeconds = 10,
      .maxRedirects = 5,
      .subresources = ResourcePolicy::SubresourcePolicy::kBlockAll,
  };
}
```

Rationale for the defaults:

- HTTPS on, HTTP off: mixed-content parity with browsers.
- File roots empty: the user is typing into an address bar; that is
  explicit consent. A future "workspace mode" can tighten this to a
  project root.
- Prompt on first use: this is the only cross-origin consent gate in
  the MVP — an ImGui modal showing the host, a 1-line explanation,
  and `Allow for this session` / `Cancel`. No persistence.
- Sub-resources blocked: matches original Non-Goals. Turning this on
  requires the subresource-fetch protocol which is still Future Work.

### Curl-missing diagnostic

Today `SvgSource::fetchFromUrl` shells out to `popen("curl -sS …")`.
If `curl` isn't on `PATH`:

- `popen` still succeeds (the shell is what runs the command).
- The shell reports `curl: command not found` on stderr, merged into
  stdout via `2>&1`, and exits non-zero.
- `SvgSource` today surfaces that as a generic `kNetworkError` with
  `diagnostics = "curl exited with code 127: /bin/sh: curl: not found"`.

That's not *wrong*, but it's not actionable. S11 adds a one-shot
availability probe:

```cpp
class CurlAvailability {
 public:
  enum class State { kUnknown, kAvailable, kMissing };

  // First call runs `curl --version` in a blocking subprocess; memoizes.
  [[nodiscard]] static State check();

  // Human-readable install instructions per platform.
  [[nodiscard]] static std::string installHint();
};
```

The `ResourceGatekeeper` consults it before letting any `http(s)://`
URI through:

```
Decision resolve(...) {
  if (scheme == "https" || scheme == "http") {
    if (CurlAvailability::check() == CurlAvailability::State::kMissing) {
      return {
        .outcome = Outcome::kDeny,
        .reason = "curl is not installed. " + CurlAvailability::installHint(),
        .resolved = {},
      };
    }
    ...
  }
}
```

`installHint()` returns platform-specific guidance:

- **Linux**: `"Install curl: sudo apt install curl (Debian/Ubuntu) or sudo dnf install curl (Fedora)."`
- **macOS**: `"Install curl: brew install curl — the system curl at /usr/bin/curl should work out of the box; this check suggests it was removed or PATH is broken."`
- **Other**: `"Install the 'curl' CLI and ensure it's on your PATH."`

The status chip surfaces the full `reason` string so the user gets the
install hint inline rather than opening a log file.

### Tests

- `ResourcePolicy_tests.cc`: matrix of (scheme × policy × URI) against
  the decision table. Hostname wildcard matching. Deny-list takes
  precedence over allow-list. Path canonicalization catches
  `../etc/passwd` even with roots.
- `CurlAvailability_tests.cc`: stubbable probe — inject a fake `PATH`
  that doesn't contain curl; assert `installHint()` mentions the
  current OS correctly.
- `ResourceGatekeeper_integration_tests.cc`: end-to-end, fires an
  actual curl against a fixture HTTPS server; separately, wipes PATH
  and asserts the denied-chip message.

## Implementation order inside the single PR

To make the diff reviewable, commits within the PR land in this order:

1. Design doc extension (this section). No code.
2. `SandboxSession` + `SessionProtocol` transport layer (S7).
   `SandboxHost::renderToBackend` becomes a shim that opens a
   one-off session. Existing sandbox tests green.
3. Session codecs + editor-API wire messages (S8). Includes the
   `Frame` bundle, request/response encoders, round-trip tests,
   `editor_backend_request_fuzzer`, and `frame_response_fuzzer`.
4. Move editor document-owning code into `donner/editor/backend_lib/`.
   Pure file moves — no behavior change. `EditorApp_tests.cc`
   continues to pass, now under the new directory.
5. New `donner_editor_backend` binary. Wires the S8 request loop to
   `donner::editor::EditorApp`. Integration tests spawn it and
   exercise every API.
6. `EditorBackendClient` (S9a) with two implementations: `_Session`
   (desktop) and `_InProcess` (WASM). Both share a single header.
7. Refactor `donner/editor/main.cc` and the host library into the
   thin-client shape. Delete `PipelinedRenderer`, `app::EditorApp`,
   `donner_editor_gui_main.cc`. Banned-pattern lint enabled at the
   end of this commit — prior commits may still contain transient
   parse calls.
8. Unified `AddressBar` widget, `SvgFetcher` split, `nfd` dev-dep
   hookup (S10).
9. `ResourcePolicy` + `ResourceGatekeeper` + `CurlAvailability`
   (S11). Gating enabled on the address bar's fetch path.
10. Release-notes + README touch-up.

Every commit is independently buildable and testable, but the PR is
reviewed as a whole. CI runs full suites after each commit via
`git rebase --exec`.

## User Stories

- **As a security-conscious user**, I want to paste a URL from a random
  Discord channel into the editor without worrying that a malformed SVG
  will corrupt my in-progress document.
- **As a developer**, I want to debug "why does this SVG render wrong in
  Donner but right in Chrome" by pausing at the exact draw call where the
  visual diverges, and inspecting the paint parameters.
- **As a fuzzer operator**, I want `fuzz_replay_cli` to exercise the same
  code path real users hit (sandbox + IPC + replay), so crashes found in
  fuzzing are crashes users would see.
- **As a bug reporter**, I want to attach a `.rnr` file to an issue that
  reproduces the visual glitch without shipping the possibly-proprietary
  source SVG.
- **As a Donner maintainer**, I want the record/replay format to be
  version-controlled regression fixtures so I can assert that
  "issue #NNN renders this command sequence" forever.

## Background

Donner's parser is the highest-surface attack target in the codebase. It
accepts arbitrary XML + CSS + path data + filter graphs, and fuzzer corpus
already exists (`SVGParser_fuzzer`, `SVGParser_structured_fuzzer`). Every
editor launch that loads an SVG today is trusting the parser with the
host process lifecycle.

Chromium, Firefox, Safari, and Edge all moved HTML/CSS/image parsing out of
the browser UI process a decade ago for exactly this reason. Browsers treat
the renderer as a separate process that receives display-list-ish commands
and rasterizes them. Donner's `RendererInterface` is already shaped like a
display list; it's 28 pure virtual methods over value types. The only thing
stopping it from being a wire protocol is that we haven't written a
serializer.

Prior art in tree:

- `RendererRecorder` (planned, not shipped) — the tee pattern, in-memory
  only. See [renderer_interface_design.md:242-256](./0003-renderer_interface_design.md).
- `RendererInterface` — [donner/svg/renderer/RendererInterface.h:190-353](../../donner/svg/renderer/RendererInterface.h).
- Editor design doc — [docs/design_docs/0020-editor.md](./0020-editor.md). The editor
  currently has no sandbox story.
- Parser entry point — [donner/svg/parser/SVGParser.h:93](../../donner/svg/parser/SVGParser.h).
  Already `noexcept`; already produces a `ParseResult`. Already the right
  seam.

No existing IPC, process separation, shared memory, or sandboxing code in
the tree (grep confirmed). This is greenfield.

## Requirements and Constraints

**Functional:**
- Every SVG load through the address bar, file dialog, or programmatic
  `EditorApp::loadSvgString()` is routed through the sandbox.
- The host process never executes `SVGParser::ParseSVG` directly (except in
  WASM builds, where the browser is the sandbox).
- Wire format is stable within a milestone; breaking changes bump the
  4-byte version header.
- Record/replay files are portable across Skia/TinySkia/Geode backends with
  pixel identity on the same backend, best-effort parity across backends.

**Quality:**
- Sandbox roundtrip (parse + render + IPC + replay) adds no more than
  **2× the in-process render time** at p50 for the existing test corpus.
  The goal is "you don't notice"; a 2× budget leaves room to optimize.
- Child process spawn + first-render latency ≤ 80 ms at p95 on the M1 target
  machines (Linux dev laptop, macOS dev laptop). If it's higher, we switch
  to a warm-pool model.
- Frame inspector pause → unpause → next render ≤ 16 ms (one frame) so the
  inspector doesn't drop frames when engaged.

**Constraints:**
- No raising the minimum C++ standard for non-sandbox code until S5. S5 is
  opt-in under a separate build config.
- No new mandatory dependencies in the BCR consumer graph. All sandbox
  deps (`curl`, any IPC helpers) are `dev_dependency = True` in
  `MODULE.bazel`, same pattern as imgui/glfw/tracy.
- No `fork()` without `exec()` — we don't want to inherit the host's entt
  registry, GL context, or malloc arena into the child.

## Proposed Architecture

### Trust boundary

```
┌────────────────────────────────────────┐     ┌───────────────────────────────┐
│  Editor Host Process (TRUSTED)         │     │  Sandbox Child (UNTRUSTED)    │
│                                        │     │                               │
│  ┌──────────────────────────────────┐  │     │  ┌─────────────────────────┐  │
│  │  ImGui UI + Address Bar          │  │     │  │  stdin: ParseRequest    │  │
│  │   │                              │  │     │  │     │                   │  │
│  │   ▼                              │  │     │  │     ▼                   │  │
│  │  HTTP(S) fetch (curl) / fs read  │  │     │  │  SVGParser::ParseSVG    │  │
│  │   │                              │  │     │  │     │                   │  │
│  │   ▼ (raw bytes)                  │  │     │  │     ▼                   │  │
│  │  SandboxHost::render()           │──┼─────┼──│  SVGDocument            │  │
│  │   │                              │  │ IPC │  │     │                   │  │
│  │   │  ┌───────────────────────┐   │  │ pipe│  │     ▼                   │  │
│  │   │  │ ReplayingRenderer     │   │  │     │  │  RendererDriver         │  │
│  │   │  │   decodes wire msgs   │◄──┼──┼─────┼──│     │                   │  │
│  │   │  │   forwards to:        │   │  │     │  │     ▼                   │  │
│  │   │  └───────────────────────┘   │  │     │  │  SerializingRenderer    │  │
│  │   ▼                              │  │     │  │  (writes wire to stdout)│  │
│  │  FullSkiaRenderer / TinySkia / Geode │  │     │  └─────────────────────────┘  │
│  │   │                              │  │     │                               │
│  │   ▼                              │  │     │  seccomp-bpf / sandbox_init   │
│  │  GL framebuffer, on screen       │  │     │  no fs, no net, pipes only    │
│  └──────────────────────────────────┘  │     └───────────────────────────────┘
└────────────────────────────────────────┘
```

**Key invariant**: `RendererInterface` is the *only* data flow from sandbox
to host. The host never sees `SVGDocument`, never sees parsed XML, never
sees the sandbox's entt registry. It sees a stream of rendering commands,
decodes them with a fuzzed deserializer, and forwards them to its real
backend. The attack surface shrinks from "a full SVG parser" to "a
protobuf-shaped deserializer for 28 message types."

### Why RendererInterface is the right wire protocol

Three properties of the existing interface make this work:

1. **Pure virtual, no state bleed.** Every method is `virtual`, with no
   default implementation or inherited state. Calls are self-contained
   modulo the explicit LIFO stacks (`pushTransform`/`popTransform`,
   `pushClip`/`popClip`, `pushIsolatedLayer`/`popIsolatedLayer`). Those
   stacks are already serialized as "push these, pop them in this order" —
   there's no hidden state.
2. **Value-typed arguments.** Every argument is either a POD, a value type
   with well-defined copy semantics (`PathShape`, `ResolvedClip`,
   `PaintParams`), or an explicit `span<const T>`. No raw pointers, no
   back-references into the document. `ResolvedClip` even implements a
   deep-copy constructor explicitly
   ([RendererInterface.h:109-127](../../donner/svg/renderer/RendererInterface.h))
   because the driver already needs it. That deep copy is the serializer.
3. **Driver is already separated from backend.** `RendererDriver` consumes
   an `SVGDocument` and emits calls into a `RendererInterface&`. It doesn't
   know what backend is on the other side. Swapping a real backend for a
   `SerializingRenderer` is a one-line change at the driver call site.

This is the "we already have the interface, we just need the protocol"
observation. We're not designing IPC; we're designing *a serializer for an
interface that already exists*.

### Sandbox child lifecycle

- **Launch**: host `posix_spawn`s `donner_parser_child` with stdin/stdout
  piped, stderr inherited (for debug logging), working directory `/`, no
  environment except `LANG`, `LC_*`, `PATH` (for dyld/ld.so), and a
  `DONNER_SANDBOX=1` marker the child checks in `main()` to refuse to run
  without it. All other FDs closed via `posix_spawn_file_actions_addclosefrom_np`
  on Linux / `CLOEXEC` sweep on macOS.
- **Handshake**: child writes a 16-byte `{magic, version, pid, pad}` frame
  to stdout within 500 ms of launch. Host reads it, asserts version match,
  considers the child "ready." No complex handshake beyond this.
- **Request**: host writes `{opcode=kParseRequest, len, svg_bytes,
  viewport, backend_hint}` to child stdin.
- **Response**: child parses, drives renderer, writes
  `{opcode=kBeginFrame, ...}` … `{opcode=kEndFrame}` sequence. Each frame
  ends with `kEndFrame`. The child then waits for the next request.
- **Crash**: child exits non-zero (SIGSEGV, SIGABRT, parse OOM, etc.).
  Host's next `read()` on stdout returns 0 or errors. `SandboxHost`
  observes this, raises `SandboxCrashed{exit_code, signal}`, and respawns
  on the next request. Inflight frame is discarded.
- **Shutdown**: host writes `{opcode=kShutdown}`; child exits cleanly.
  Host joins the reader thread. On editor exit, host sends `kShutdown`
  with a 100 ms grace before `SIGKILL`.

### Wire format

Framed message stream, little-endian (matches every platform Donner
targets). No self-describing metadata beyond the opcode — the host and
child are built from the same source, so field layouts agree by
construction.

```
Message:
  u32 magic     = 'DRNR'                (only on first frame per connection)
  u32 opcode    // one per RendererInterface method + control opcodes
  u32 length    // payload bytes
  u8  payload[length]

Payload encoding:
  - Fixed-size fields: packed struct (alignment 4).
  - std::string / std::string_view: u32 length + bytes.
  - std::vector<T>: u32 count + T repeated.
  - std::optional<T>: u8 present + T if present.
  - std::variant<Ts...>: u8 tag + Ts[tag] body.
  - span<const T>: identical to vector<T>.
```

Control opcodes (not mapped to `RendererInterface` methods):

| Opcode | Direction | Purpose |
|---|---|---|
| `kHandshake` | child→host | First frame after spawn. |
| `kParseRequest` | host→child | SVG bytes + viewport + backend hint. |
| `kBeginFrame` … `kEndFrame` | child→host | Wraps a command sequence. |
| `kParseError` | child→host | Parser returned an error; no commands follow. |
| `kShutdown` | host→child | Exit cleanly. |
| `kDiagnostic` | child→host | Warning sink passthrough (non-fatal parse warnings surface in host UI). |

### Address bar flow

```
1. User types https://example.com/foo.svg into address bar.
2. EditorApp::navigate(uri):
   a. Classify scheme: https / http / file / relative.
   b. Host-side fetch:
      - https/http: curl → bytes (with a hard 10 MB cap + 10 s timeout).
      - file://  : fopen → bytes (cap 100 MB; this is the user's own disk).
   c. Show "Loading…" chip in UI.
3. SandboxHost::render(bytes, viewport, backend_hint).
4. Child parses, drives renderer, streams commands. ReplayingRenderer on the
   host feeds them into the real backend for the active frame.
5. Success: "Rendered" chip, viewport shows result, undo timeline resets
   (navigating is not an undoable operation; it's a document replacement).
6. Failure modes:
   - Fetch error  → "Fetch failed (HTTP 404)" chip, keep previous document.
   - Parse error  → "Parse error: <message>" chip, keep previous document.
   - Sandbox crash → "Sandbox crashed (SIGSEGV)" chip, keep previous doc,
                     respawn child asynchronously.
```

**The host never parses.** Even for local files that the user "trusts," we
route through the sandbox — defense in depth and consistency of code path
both matter. Files the user trusts can still be attacker-controlled (email
attachments, downloads folder, `~/Downloads/untrusted.svg`).

### Record/replay format (`.rnr`)

The `.rnr` file is *literally the sandbox's stdout stream for one render*,
written to disk verbatim. No reordering, no compression (initially). This
means:

- `DrnrRecorder` is a tee: `stdout → (real consumer, file)`. Zero
  serialization overhead — the bytes are already in wire format.
- A file-on-disk replay is indistinguishable from a live sandbox render.
  The `ReplayingRenderer` doesn't know or care whether its `BufferedReader`
  is a pipe or a file.
- Format version = wire version. A `.rnr` from version 1 fails to load
  against a version 2 editor with a clear error; we're not going to write
  version-migration code in the initial milestones.

File header (prepended at recorder construction, not passed through the
pipe):

```
u32 magic    = 'DRNF'  (note: F for file, distinguishes from pipe magic)
u32 version
u64 timestamp_unix_ns
u32 viewport_w
u32 viewport_h
u32 backend_hint
u32 uri_length
u8  uri[uri_length]   // originating URL, for provenance
```

### Frame inspector

The frame inspector is "pause the IPC pump and show the ring buffer."
Concretely:

- `SandboxHost` has two modes: `kStreaming` (commands flow straight to
  `ReplayingRenderer` as they arrive) and `kBuffered` (commands accumulate
  in a `std::vector<WireMessage>` until the user explicitly drains them).
- Pressing "Pause next frame" flips the mode to `kBuffered` on the next
  `kBeginFrame` boundary. The entire next frame's commands buffer without
  touching the real backend.
- ImGui pane renders the buffered commands as a scrollable table:

```
┌──────────────────────────────────────────────────────────────────────┐
│  Frame Inspector           [▶ Resume]  [⏏ Export .rnr]  [🔄 Clear]  │
├──────────────────────────────────────────────────────────────────────┤
│  #  │ Depth │ Opcode             │ Summary                           │
├─────┼───────┼────────────────────┼───────────────────────────────────┤
│  0  │   0   │ beginFrame         │ viewport=800×600                  │
│  1  │   0   │ pushTransform      │ scale(2,2) translate(50,50)       │
│  2  │   1   │ pushClip           │ rect(0,0,400,400) + 1 path        │
│  3  │   1   │ setPaint           │ fill=#FF0000 stroke=none          │
│  4  │   1   │ drawRect           │ (10,10,90,90) opacity=1.0         │
│  5  │   1   │ setPaint           │ fill=url(#grad) stroke=#000       │
│  6  │   1   │ drawPath           │ 47 ops, fill=NonZero              │
│  …  │       │                    │                                   │
│ 184 │   0   │ endFrame           │                                   │
├──────────────────────────────────────────────────────────────────────┤
│  Scrub: [0 ━━━━━━━━━━━●━━━━━━━━ 184]   Replay through command #93  │
├──────────────────────────────────────────────────────────────────────┤
│  Selected: #6 drawPath                                               │
│    path:   M10,10 L90,10 C... (47 ops)                               │
│    fill:   LinearGradient(#grad) [3 stops]                           │
│    stroke: #000 width=2.0 linecap=round                              │
│    bbox:   (10, 10) → (90, 90)                                       │
└──────────────────────────────────────────────────────────────────────┘
```

The scrub slider replays commands `[0..i)` into an off-screen renderer via
`RendererInterface::createOffscreenInstance()`. The off-screen result blits
into the main viewport, replacing the paused frame. Scrubbing is
interactive (60 fps target) because replay is a pointer walk through an
already-decoded `std::vector`.

Export writes the buffered messages + header to a `.rnr` file.

Depth tracking: the inspector counts `pushTransform`/`popTransform`,
`pushClip`/`popClip`, `pushIsolatedLayer`/`popIsolatedLayer`,
`pushFilterLayer`/`popFilterLayer`, `pushMask`/`popMask`. Mismatched push/pop
is flagged red.

### Insertion point into the editor

The editor's M2 milestone introduces `AsyncSVGDocument` — a single-threaded
command queue flushed at frame boundaries
([editor.md:226-238](./0020-editor.md)). That's the seam.

Today's M2 plan:
```
main thread:  DOM mutations → command queue → render thread → RendererInterface
```

Post-sandbox:
```
main thread:  fetch bytes → SandboxHost::render(bytes)
              │
              ▼
              child process ──── IPC wire ────┐
                                              ▼
              render thread: ReplayingRenderer → real RendererInterface
```

The `AsyncSVGDocument` mutation seam does not move — it still operates on
a host-side `SVGDocument` mirror for selection, hit-testing, and the text
editor pane. **But that mirror is built from the same `kParseRequest` bytes
the sandbox consumes, parsed a second time on the host** for non-rendering
purposes.

Wait — that reintroduces the parser in the host. Alternatives considered:

1. **Host re-parses for DOM mirror** (simplest): two parses per load, but
   the host parse is only for selection/editing structure, not rendering.
   The bug blast radius is "host crashes while building the mirror" — same
   as today. No regression, no improvement.
2. **Sandbox ships DOM structure back** (cleanest): extend the wire format
   with a `kDomStructure` message containing a flat tree of element IDs,
   tag names, bounding boxes, and source-range spans. The host never parses.
   Downside: a second protocol surface and another fuzzed deserializer.
3. **Read-only view mode initially** (punt): the address bar loads *only*
   render; the document is not mutable until the user issues an explicit
   "Edit this" command which re-parses on the host. First milestones pay
   this cost.

**Decision**: start with (3) for S3, migrate to (2) in a follow-up milestone
once the command-stream wire format is stable. (1) is the fallback if (2)
proves too expensive. Decision rationale: (3) lets the sandbox milestone
ship without touching the editor's edit path; (2) is the principled
end-state but requires designing a second wire protocol, which should not
block S3.

## API / Interfaces

```cpp
namespace donner::editor::sandbox {

class SandboxHost {
 public:
  explicit SandboxHost(SandboxOptions opts);
  ~SandboxHost();  // sends kShutdown, joins, SIGKILLs on timeout

  // Spawns the child if not already running, sends kParseRequest, returns a
  // handle that streams commands into `target` and completes at kEndFrame.
  // Throws SandboxCrashed on child exit before kEndFrame.
  void render(std::span<const uint8_t> svg_bytes,
              const RenderViewport& viewport,
              BackendHint backend,
              RendererInterface& target);

  // For the frame inspector: buffers the next frame's commands instead of
  // forwarding them, then returns the buffer. Caller owns the buffer.
  std::vector<WireMessage> captureNextFrame(
      std::span<const uint8_t> svg_bytes,
      const RenderViewport& viewport,
      BackendHint backend);

  [[nodiscard]] bool childAlive() const;
  [[nodiscard]] std::optional<ExitInfo> lastExit() const;
};

// Tee adapter for record/replay.
class DrnrRecorder {
 public:
  DrnrRecorder(std::filesystem::path out,
               const RecordingHeader& header);
  void feed(const WireMessage& msg);  // writes to disk and passes through
  // use via: SandboxHost::render(bytes, vp, backend, TeeRenderer{recorder, realRenderer})
};

// Replay a .rnr file into any RendererInterface.
ReplayResult ReplayDrnrFile(std::filesystem::path in, RendererInterface& target);

}  // namespace donner::editor::sandbox
```

Host-side `RendererInterface` wrappers:

```cpp
namespace donner::svg {

// Forwards calls into a wire stream. Owned by the sandbox child.
class SerializingRenderer : public RendererInterface {
 public:
  explicit SerializingRenderer(BufferedWriter& out);
  // ... 28 method overrides, each encoding opcode + payload.
};

// Reads a wire stream and forwards to a real backend.
class ReplayingRenderer {
 public:
  ReplayingRenderer(BufferedReader& in, RendererInterface& target);
  // Drives commands until kEndFrame or error. Returns ReplayStatus.
  ReplayStatus pumpFrame();
};

}  // namespace donner::svg
```

## Data and State

**Threading model**:
- Host main thread: ImGui, event loop, address bar, `SandboxHost::render()`
  (synchronous).
- Host IPC reader thread (one, owned by `SandboxHost`): blocks on child
  stdout `read()`, decodes frames, pushes `WireMessage` into a lock-free
  SPSC queue. Exists only to keep the main thread from blocking on
  per-command `read()` syscalls.
- Host render thread (inherited from editor M2): drains the SPSC queue via
  `ReplayingRenderer`, issues calls into the real backend. Same thread as
  the existing GL context.
- Child main thread: blocks on stdin `read()` for `kParseRequest`, parses,
  drives renderer, writes to stdout, loops.
- Child has no other threads. Parser and renderer driver are single-threaded.

**Memory ownership**:
- Wire messages are owned by the IPC reader thread's decode buffer until
  consumed by `ReplayingRenderer`. No reference-sharing with the real
  backend — `ReplayingRenderer` calls `drawPath(PathShape{...})` by value,
  same as the driver would.
- `PathShape::path` is the biggest allocation per draw call. It's
  reconstructed from wire bytes into a fresh `std::vector<PathOp>` in the
  host's arena. We accept the allocation cost; the Skia backend was already
  going to copy this into an `SkPath`.
- Frame inspector's buffered `std::vector<WireMessage>` is host-thread-owned
  and lives until the user drains it or clears the pane.

## Error Handling

| Class | Detection | Response |
|---|---|---|
| HTTPS fetch failure | curl return code | Address bar chip, no sandbox round-trip. |
| File read failure | `errno` | Address bar chip, no sandbox round-trip. |
| Parse error (valid wire, malformed SVG) | `kParseError` opcode | Address bar chip with parser message, previous document preserved. |
| Parse warnings | `kDiagnostic` opcodes | Logged to editor console pane; non-blocking. |
| Sandbox crash (SIGSEGV, SIGABRT, OOM) | Child exit non-zero, `read()` returns 0 | `SandboxCrashed` raised, address bar chip, child respawned on next request. Crashing corpus entry logged for fuzzer ingestion. |
| Sandbox hang | Host-side 30 s deadline on `kEndFrame` arrival | Host sends SIGKILL, treats as crash. Deadline is generous; real renders should complete in <1 s. |
| Wire format error (host sees an opcode it doesn't recognize, length overrun, invalid variant tag) | Deserializer check | Host raises `WireProtocolError`, kills child, respawns. **This path is fuzzed.** |
| Backend reports an error during replay | `ReplayingRenderer` catches | Logged, frame completes on best-effort basis. Host never crashes on a replay error. |

**Non-negotiable invariant**: *the host process never crashes due to any
input from the sandbox child, including maliciously-crafted wire messages
targeting the host's deserializer*. The `sandbox_wire_fuzzer` target in S2
exists specifically to hold this line.

## Performance

**Targets** (p95, on M1-class Linux + macOS dev machines):

- `posix_spawn` + child ready handshake: ≤ 40 ms.
- Small SVG (resvg test suite average): end-to-end parse + render + replay
  ≤ 15 ms additional over in-process baseline.
- Large SVG (Ghostscript tiger, 200+ paths): ≤ 50 ms additional.
- Address bar navigation p50 for a 50 KB `.svg` over HTTPS: ≤ 200 ms
  (dominated by network, not IPC).
- Frame inspector scrub: 60 fps on a 1000-command frame.

**Budget decomposition**:
1. Serialization (child side): one memcpy per primitive-typed field,
   allocations only for `std::vector` payloads. Target <10% CPU overhead
   vs. a real backend call.
2. Pipe I/O: the kernel pipe buffer is 64 KB on Linux default; typical frames
   are 10–200 KB. We accept that large frames will block the child momentarily
   when the host falls behind — this is desired backpressure.
3. Deserialization (host side): mirror of serialization, one memcpy per
   primitive, one allocation per variable-length field.
4. Replay (host side): identical to in-process rendering — the forwarded
   calls are exactly what the driver would have emitted.

**Watch list** (things that could blow the budget):
- `drawText` has the largest payload (`span<FontFace>` with TTF tables).
  Mitigation: if the font is resolvable via a system font family name, send
  only the name; fall back to shipping bytes only for embedded `@font-face`.
- `pushFilterLayer(FilterGraph)` has a deeply nested structure. Measure
  first, optimize if needed.
- `ImageResource` for raster images. Measure; may need shared memory fast
  path for very large images (Future Work).

**Measurement plan**:
- Benchmark target `//donner/editor/sandbox:ipc_benchmark` that runs the
  resvg test suite through sandbox-on and sandbox-off paths, reports
  per-test deltas.
- Tracy zones around every major boundary: parse start/end, wire
  write/read, replay start/end. Editor's Tracy toggle (M3) surfaces these.

## Security / Privacy

### Threat model

**Adversary**: attacker who controls the SVG bytes being parsed. Can craft
malformed XML, oversized paths, pathological filters, font data with
malicious OpenType tables, CSS expressions that trigger parser or style-
system bugs. Delivery vectors: user pastes URL, user opens downloaded file,
user receives `.svg` attachment and opens in Donner.

**Attacker capability**: arbitrary code execution *inside the sandbox
child* is considered "game won" from the parser's perspective. The child
must not be able to:

- Read or write the user's filesystem (other than the SVG bytes piped in).
- Open network connections.
- Send IPC to the editor host beyond the `RendererInterface` wire stream.
- Escalate to persist beyond its process lifetime.
- Trigger host-side crashes via malformed wire messages.

**Non-goals for the threat model**:

- Side-channel attacks (timing, cache) — out of scope; we're not rendering
  secrets.
- Protection against attackers who already have code execution on the host.
- Protection of the sandbox child from itself (e.g., DoS via slow renders
  is handled by the 30 s host deadline, not by fine-grained limits in the
  child).

### Trust boundaries

1. **User input (address bar) → host fetch**: host validates scheme allowlist
   (`https`, `http`, `file`, bare path = `file` resolved relative to CWD),
   enforces 10 MB size cap on HTTPS, 100 MB cap on local files, 10 s HTTPS
   timeout. No URL redirection following beyond 5 hops. No cookies, no
   user-agent customization.
2. **Host fetch → sandbox child**: raw bytes only. No filenames, no URLs,
   no host metadata crosses the pipe.
3. **Sandbox child → host (wire messages)**: deserializer is the trust
   boundary. Every length field is bounds-checked against the remaining
   payload before allocation; every variant tag is range-checked; every
   `std::vector` count is capped at a sensible maximum (e.g., 10M path ops
   per `PathShape`, 1024 clip paths per `ResolvedClip`). **The deserializer
   is fuzzed (`sandbox_wire_fuzzer`).**
4. **Sandbox child → OS**: seccomp-bpf (Linux) or `sandbox_init` (macOS)
   deny-by-default, allowing only the syscalls required to read stdin,
   write stdout, allocate memory, and exit. Enforced after child
   initialization completes (the parser allocator may warm up before the
   filter activates).

### Defensive measures

- **Resource caps**:
  - `RLIMIT_AS` (address space) = 1 GB. A parser OOM kills only the child.
  - `RLIMIT_CPU` = 30 seconds. A pathological SVG can't pin a core forever.
  - `RLIMIT_FSIZE` = 0. No file writes, belt-and-braces.
  - `RLIMIT_NOFILE` = 16. Enough for stdin/stdout/stderr and a handful of
    internal FDs; no extra file descriptors possible.
- **Wire message caps** (enforced in `ReplayingRenderer`):
  - Max frame size: 256 MB. Larger frames are treated as a protocol violation.
  - Max path ops per `PathShape`: 10M.
  - Max clip paths per `ResolvedClip`: 1024.
  - Max filter graph depth: 256.
  - Max command count per frame: 10M.
- **No ambient authority**: child inherits no environment variables beyond
  locale + `PATH`, no file descriptors beyond pipes, no working directory
  beyond `/`, no signal handlers, no `LD_PRELOAD` (wiped from environment),
  no privilege.
- **Opaque forwarding**: if the deserializer cannot fully understand a
  message (unknown opcode from a future child version), it treats the
  connection as corrupt and kills the child. No graceful degradation.

### Fuzzing plan

Three new fuzzers:

1. **`sandbox_wire_fuzzer`**: feeds random bytes into `ReplayingRenderer`
   operating against a `MockRendererInterface` (planned in the renderer
   interface design). Asserts the host never crashes. **The single most
   important fuzzer in this design.** Corpus seeded with real recorded
   frames from the resvg test suite.
2. **`drnr_replay_fuzzer`**: same deserializer, but file-shaped inputs
   with a header. Catches header-specific bugs.
3. **Reuse `SVGParser_fuzzer` end-to-end**: every corpus entry is fed
   through `SandboxHost::render` as part of CI. Any host crash is a
   test failure. This is the `fuzz_replay_cli` path from editor M5, with
   sandbox enabled.

### Sensitive data handling

- The editor does not log wire messages by default. A `--debug-wire`
  flag enables a hex dump to stderr for debugging, gated off in release
  builds.
- `.rnr` files can contain text content from the source SVG (via
  `drawText`'s `TextParams`). Users exporting a `.rnr` for bug reporting
  should be aware of this; the export dialog mentions it.
- No telemetry, no phone-home, no anonymous crash reporting in the initial
  milestones.

### Invariants enforced post-launch

1. **Host never crashes on sandbox input.** Enforced by `sandbox_wire_fuzzer`
   in CI.
2. **Parser never executes in the host process (non-WASM builds).**
   Enforced by `check_banned_patterns.py` extension: forbid
   `SVGParser::ParseSVG` imports outside `donner/editor/sandbox/child/**`
   and existing non-editor callers. The editor host code is not allowed
   to reach the parser directly.
3. **Wire format is versioned.** Breaking changes bump the version field;
   a version mismatch between host and child is a fatal error on handshake.

## Testing and Validation

- **Unit**: `SerializingRenderer` / `ReplayingRenderer` round-trips for
  every `RendererInterface` method. One test per opcode, table-driven.
  Asserts encoded bytes + decoded state match.
- **Integration**: the resvg test suite, run once in-process and once
  through the sandbox; pixel-identical assertion on same backend. This is
  the single most important test — it proves end-to-end correctness.
- **Golden**: record `.rnr` files for a curated subset of the resvg suite,
  check them in under `donner/editor/sandbox/tests/goldens/*.rnr`. On CI,
  replay them and assert pixel equality. Regenerate via the standard
  `UPDATE_GOLDEN_IMAGES_DIR` workflow.
- **Fuzzing**:
  - `sandbox_wire_fuzzer` runs on every PR via existing fuzzer CI plumbing.
  - `SVGParser_fuzzer` corpus replays through `fuzz_replay_cli` with
    `--sandbox` enabled as a release gate.
- **Crash recovery**: deliberately-crashing corpus entries (SIGSEGV, SIGABRT,
  infinite loops) fed through `SandboxHost`; assertions on host liveness
  and child respawn count.
- **Frame inspector**: headless test that enters buffered mode, captures a
  frame, scrubs from 0 to N, asserts the off-screen replay matches the
  in-stream replay at each index.
- **Address bar**: integration test with a local HTTP server serving curated
  payloads (valid SVG, malformed SVG, 404, slow response, oversized body).
  Asserts the UI chip transitions and the previous document survives every
  failure mode.

## Dependencies

- **New dev dependency**: `curl` (or `libcurl`) for HTTPS fetching in the
  address bar. Added as `dev_dependency = True` in `MODULE.bazel`, same
  pattern as imgui/glfw. Not added to the BCR consumer graph. Alternative:
  hand-rolled HTTP client — rejected as out of scope for an editor
  milestone.
- **No new runtime dependencies** for the parser child binary — it's just
  `//donner/svg:parser` + `//donner/svg/renderer:renderer_tiny_skia` +
  the new `:sandbox_wire_lib`.
- **Bazel `banned-patterns`**: new rule preventing `SVGParser::ParseSVG`
  calls from `//donner/editor:core` and its transitive deps. Editor core
  uses `SandboxHost::render` exclusively.

## Rollout Plan

- **Milestone S1** ships behind nothing — it's a new binary and a new test,
  no user-visible behavior change.
- **Milestone S3** (address bar + sandbox) ships with the address bar
  enabled by default. Users get the sandbox transparently.
- **`--in-process-parser`** debug flag remains for the entire S1–S5 period:
  routes parsing back through the host for performance comparison and
  debugging of sandbox issues. Default off. Removed after S6.
- **WASM build (editor M6)**: the sandbox code compiles out; `SandboxHost`
  becomes a direct call into `SVGParser::ParseSVG`. The browser is the
  sandbox.
- **`.rnr` format**: version 1 in S4. No migration path for version bumps
  in the S4–S6 window; a format change invalidates existing files. This
  is acceptable because `.rnr` files are not user-authored assets, only
  debug artifacts.

## Alternatives Considered

1. **seccomp-bpf only, no separate process**: run the parser in a thread
   with a restricted syscall filter. Rejected: seccomp-bpf can't protect
   the host process's memory from a parser bug; a corrupted heap in one
   thread corrupts the whole process. Process boundary is the only robust
   memory firewall.

2. **WASM parser embedded in the host**: compile `SVGParser` to WASM, run
   it in an in-process WASM runtime (e.g., Wasmtime). Memory isolation
   comes free. Rejected for now: adds ~5 MB of runtime dependency,
   complicates the build, parser-WASM boundary has its own IPC cost, and
   we'd still need to design a wire protocol. Revisit if sandbox process
   overhead proves unacceptable.

3. **Protobuf / FlatBuffers as the wire format**: rejected because the
   format is not versioned across runs (host and child always share source),
   and because a hand-rolled format is smaller, faster, and lets us
   integrate C++26 reflection directly with our own types. Protobuf is
   already in the tree as a dev dep (for other tooling) but adding it to
   the hot rendering path is a regression in both compile time and runtime.

4. **Shared memory for path / image payloads**: rejected in initial
   milestones because it complicates the trust boundary (the child gets a
   handle into host address space) and because pipe throughput is adequate
   for typical frames. Revisit if `drawImage` with large bitmaps blows the
   latency budget.

5. **Run the driver in the host, stream events from the sandbox**: the
   sandbox would ship a parsed `SVGDocument` subtree over the wire and the
   host would run `RendererDriver`. Rejected: `SVGDocument` is a much
   larger and more complex surface than `RendererInterface`. The wire
   format would need to encode entt registries, style trees, and resolved
   components. `RendererInterface` is the smallest possible seam.

6. **C++26 reflection on day one**: rejected because the tree is on C++20
   and the toolchain may not support P2996 yet. Shipping the hand-rolled
   version first lets us validate the wire format and ship the feature;
   reflection migration is a code-reduction follow-up.

## Open Questions

- **Long-lived child vs. per-render spawn**: **Resolved** (S7). One
  persistent `SandboxSession` per editor, transparent respawn on crash.
  The per-render `posix_spawn` path survives only as `renderOnce()` for
  fuzzer harnesses.
- **HTTPS client**: **Resolved for the MVP** — `popen("curl …")` is
  adequate for typical editor use and avoids pulling in libcurl as a
  runtime dep. S11 adds the missing-curl diagnostic. A libcurl upgrade
  remains Future Work.
- **Sub-resource fetching** (`<image href="https://...">` inside an SVG):
  still out of scope. `ResourcePolicy::subresources` is reserved so the
  shape is ready when the request-from-sandbox protocol lands. Not
  blocking S7–S11.
- **Shared memory for `ImageResource`**: measure first.
- **Windows sandboxing**: requires a separate story (Job Objects +
  AppContainer). Not blocking M1; Windows editor support is itself Future
  Work in [editor.md](./0020-editor.md).
- **DOM mirror strategy**: **Obsoleted by S8 rescope.** The original
  open question assumed the host would keep a mirror of sandbox state.
  The S8 rescope makes the IPC boundary the editor's API itself —
  there is no host-side DOM, and selection / hit-test / undo are
  backend-local. Mirror options (1)/(2)/(3) are all rejected in favor
  of "no mirror."
- **Hover latency for `PointerEvent`s**: every mouse-move sends a
  `kPointerEvent{phase=Move}` to the backend when a tool is active
  (drag in flight, marquee). Worst-case per-event budget: 16 ms
  minus render time. A pipe round-trip on the same machine is ~50 µs;
  the budget is dominated by the backend's per-event render cost
  (already 5–8 ms for a moderately complex document). If render
  becomes the bottleneck, the backend may coalesce pending moves —
  see `Frame` batching in §S8.
- **Hover feedback without document access**: when the user hovers
  over an element with no drag in flight, the host wants to highlight
  the element under the cursor. Since the host doesn't know element
  identity, it asks the backend with `kPointerEvent{phase=Move}` and
  draws the resulting `SelectionOverlay.hoverRect` on the next frame.
  The round-trip cost is acceptable because hover doesn't require
  pixel-precise latency — a 16-ms delay is imperceptible.

# Future Work

- [ ] Sub-resource fetching protocol (sandbox requests, host fetches,
      returns bytes). Allows `<image href>`, `<use href>` cross-document,
      `@font-face src=url()`.
- [ ] Shared-memory fast path for `drawImage` payloads larger than 1 MB.
- [ ] Windows sandbox via Job Object + AppContainer.
- [ ] `kDomStructure` wire message for structured editor DOM mirror
      (replaces the "re-parse on host" path).
- [ ] C++26 reflection migration (S5) assuming toolchain support lands.
- [ ] Compressed `.rnr` files (zstd framing) for fixtures that take up
      too much git space.
- [ ] Multi-frame `.rnr` files for animation capture.
- [ ] Cross-backend replay comparisons: replay the same `.rnr` against
      Skia, TinySkia, and Geode and pixel-diff the results. First-class
      integration of the existing renderer parity story.
- [ ] Profile-guided optimization of the wire format (variable-length
      integers, dictionary compression for repeated paints).
- [ ] Per-session sandbox telemetry (opt-in) for crash reporting.
