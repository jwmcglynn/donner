# Design: Editor Sandbox, Renderer-IPC, and Record/Replay

**Status:** Design
**Author:** Claude Opus 4.6
**Created:** 2026-04-11

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
  [renderer_interface_design.md:242](./renderer_interface_design.md) — the
  in-memory variant is the foundation for the wire format.
- [ ] Prototype **Milestone S1** (byte-level sandbox loopback, no IPC yet):
  parse in a child process, return rendered PNG bytes over stdout. This
  proves the process model before committing to the RendererInterface wire
  protocol.

## Implementation Plan

- [ ] **Milestone S1: Byte-in, PNG-out child process (proof of model)**
  - [ ] New binary `//donner/editor/sandbox:donner_parser_child`, no ImGui,
        no GL, no renderer backend. Reads SVG bytes from stdin, parses,
        renders to an in-memory bitmap via `RendererTinySkia::createOffscreenInstance()`,
        writes PNG bytes to stdout, exits.
  - [ ] Host-side `SandboxHost` class in `//donner/editor:sandbox_host` that
        `posix_spawn`s the child with `stdin`/`stdout` piped, writes SVG
        bytes, reads PNG bytes, returns them. Synchronous API.
  - [ ] Crash test: feed `SVGParser_fuzzer` corpus through `SandboxHost`;
        host must never crash, only observe non-zero exit codes.
  - [ ] **Decision gate**: if S1 frame time is dominated by process spawn,
        switch to a long-lived child with a framed-message protocol before
        S2. Measure `posix_spawn` + first-render on a p50 corpus entry.

- [ ] **Milestone S2: RendererInterface wire format**
  - [ ] Define `donner::editor::sandbox::Wire` — a framed message format.
        Header: 4-byte magic `DRNR`, 4-byte version, 4-byte length, 4-byte
        opcode, payload. Payload is POD-only; variable-length fields
        (`std::vector<PathShape>`, `SmallVector<RcString>`) are length-prefixed.
  - [ ] Enumerate opcodes, one per `RendererInterface` virtual method. 28
        total + `kEndOfFrame` sentinel + `kError`.
  - [ ] Implement `SerializingRenderer : RendererInterface` that writes the
        corresponding opcode + marshalled payload to a `BufferedWriter`
        (abstracted so it can target a pipe, a file, or an in-memory vector).
  - [ ] Implement `ReplayingRenderer` that reads from a `BufferedReader`,
        decodes, and forwards to a wrapped `RendererInterface&`.
  - [ ] **Round-trip test** in `//donner/svg/renderer/tests`: for every golden
        SVG in the existing resvg test suite, assert that
        `RendererTinySkia → serialize → deserialize → RendererTinySkia`
        produces a pixel-identical snapshot to `RendererTinySkia` alone.
        This is the single test that proves the wire format is lossless.
  - [ ] Hand-write marshallers for `PathShape`, `PaintParams`, `ResolvedClip`,
        `ImageParams`, `TextParams`, `FilterGraph`, `StrokeParams`,
        `ResolvedPaintServer` variant, `ResolvedMask`. Track total LOC; this
        is the number C++26 reflection will delete in Milestone S5.
  - [ ] Fuzz the deserializer. New target `sandbox_wire_fuzzer` that feeds
        random bytes into `ReplayingRenderer` and asserts it never crashes
        — only returns `kError`. This is non-negotiable: the deserializer is
        the trust boundary.

- [ ] **Milestone S3: Long-lived child + address bar**
  - [ ] Replace S1's one-shot child with a long-lived `donner_parser_child`
        that reads a `ParseRequest` (SVG bytes + viewport + backend choice)
        and streams a `SerializingRenderer` command sequence over stdout
        until `kEndOfFrame`, then loops. Exits on EOF.
  - [ ] Host-side `SandboxHost::render(bytes, viewport) -> CommandStream`
        where `CommandStream` is a movable handle that pumps commands into
        the host's real renderer via `ReplayingRenderer`.
  - [ ] **Address bar UI** in `EditorApp`:
        - ImGui `InputText` at the top of the viewport.
        - Accepts `https://`, `http://`, `file://`, and bare paths.
        - Dispatch: HTTPS fetch happens on the **host** (uses `curl` vendored
          as a dev-only `bazel_dep` — see Dependencies), then hands bytes to
          `SandboxHost`. `file://` / bare paths are read on the host
          (filesystem access is a host privilege, not a sandbox privilege),
          then handed off identically. The sandbox only ever sees raw bytes.
        - Status chip: `Loading…`, `Rendered`, `Crashed (sandbox)`,
          `Parse error`, `Fetch failed (HTTP 404)`, etc.
  - [ ] **Crash recovery**: if the sandbox child exits non-zero, show the
        error chip, keep the previously-rendered document on screen, and
        respawn the child on the next navigation. No editor state is lost.
  - [ ] Wire the address bar into `fuzz_replay_cli` from editor M5: every
        fuzzer corpus entry gets fed through the address bar code path,
        asserting host-process liveness.

- [ ] **Milestone S4: Record/replay + frame inspector**
  - [ ] Define `.rnr` file format = Wire framing + a single header block
        (viewport, backend hint, timestamp, originating URL). A file is
        literally the stdout of a sandboxed render session, captured verbatim.
  - [ ] `DrnrRecorder` is a `BufferedWriter` adapter that tees the IPC stream
        to disk while it's flowing into `ReplayingRenderer`. Zero-copy;
        recording is a ~0% overhead passthrough.
  - [ ] `donner_editor replay <file.rnr>` CLI mode: loads a file, replays
        into the editor's current backend, shows the result in the viewport.
        Also usable from `fuzz_replay_cli`.
  - [ ] **Frame inspector pane**: new ImGui dockable window.
        - Toggle "Pause next frame" button. When pressed, `SandboxHost`
          buffers the whole command stream for frame `N+1` into a
          `std::vector<WireMessage>` before handing it to `ReplayingRenderer`.
        - Command list: virtual-scrolled ImGui table, one row per command,
          showing opcode, nesting depth (tracked by push/pop balance),
          and a one-line summary (`setPaint(fill=#FF0000, stroke=none)`).
        - Scrub slider: `[0..N]` where `N = commands.size()`. Dragging
          replays commands `[0..i)` into an off-screen
          `RendererInterface::createOffscreenInstance()` at the viewport
          resolution, blits the result into the main viewport. This gives a
          "draw order visualization" for free.
        - Inspector sub-pane: click a command row, see its full decoded
          arguments (path ops, paint servers, transform matrix) in a
          ImGui property tree.
        - "Export .rnr" button dumps the paused frame to disk.
  - [ ] Structural-diff mode: load two `.rnr` files, show a diff of their
        command streams side-by-side. Useful for regression triage.

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

- [ ] **Milestone S6: Sandbox hardening (defense in depth)**
  - [ ] Linux: `seccomp-bpf` filter applied inside the child after
        initialization — allow only `read`, `write`, `brk`, `mmap`,
        `munmap`, `exit`, `exit_group`, `rt_sigreturn`, `futex`. Any other
        syscall kills the child.
  - [ ] macOS: `sandbox_init` with a deny-all profile allowing only
        stdin/stdout pipes. (macOS sandboxing is deprecated-but-extant; good
        enough for editor-local protection.)
  - [ ] Close all inherited file descriptors except the IPC pipes before
        `execve`. Set `RLIMIT_AS` (address space), `RLIMIT_CPU`, `RLIMIT_FSIZE`.
  - [ ] Run child under its own UID if the editor is launched with
        sufficient privilege (optional, off by default).
  - [ ] No `/proc` access, no filesystem access, no network.

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
  only. See [renderer_interface_design.md:242-256](./renderer_interface_design.md).
- `RendererInterface` — [donner/svg/renderer/RendererInterface.h:190-353](../../donner/svg/renderer/RendererInterface.h).
- Editor design doc — [docs/design_docs/editor.md](./editor.md). The editor
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
│  │  RendererSkia / TinySkia / Geode │  │     │  └─────────────────────────┘  │
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
([editor.md:226-238](./editor.md)). That's the seam.

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

- **Long-lived child vs. per-render spawn**: S1 decides. If `posix_spawn`
  dominates small-render latency, S3 must use a warm child pool. Suspected
  answer: one persistent child, spawned at editor startup, respawned on
  crash.
- **HTTPS client**: `libcurl` (battle-tested, big) vs. hand-rolled
  (`<TcpSocket>` + OpenSSL, small). Libcurl is the default recommendation;
  lean on it to avoid becoming a TLS implementation.
- **Sub-resource fetching** (`<image href="https://...">` inside an SVG):
  currently out of scope. Most interesting path forward: route through the
  host via a request-from-sandbox protocol. Delayed until after S6.
- **Shared memory for `ImageResource`**: measure first.
- **Windows sandboxing**: requires a separate story (Job Objects +
  AppContainer). Not blocking M1; Windows editor support is itself Future
  Work in [editor.md](./editor.md).
- **DOM mirror strategy**: S3 ships read-only (option 3 from Proposed
  Architecture). Decision on option 1 vs. option 2 happens when S3 closes.

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
