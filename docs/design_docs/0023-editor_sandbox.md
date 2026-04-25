# Design: Editor Sandbox, Renderer-IPC, and Record/Replay

**Status:** P0 incomplete — macOS desktop still falls back to the in-process backend, and the
Linux editor binary's default sandbox path is not covered by an end-to-end editor-binary test.
The sandbox is not done until both desktop platforms default to a process-backed backend and CI
proves that selection.
**Author:** Claude Opus 4.6
**Created:** 2026-04-11
**Last updated:** 2026-04-24

## Progress snapshot

| Milestone | State | Notes |
|-----------|-------|-------|
| S1 — byte-in / PNG-out child | ✅ Landed | `donner_parser_child`, `SandboxHost`, and Linux subprocess tests exist. This remains the one-shot CLI/debug path, not the live editor transport. |
| S2 — RendererInterface wire format | ✅ Landed for debug/replay | `Wire.h`, `SerializingRenderer`, `ReplayingRenderer`, `SandboxCodecs`, `sandbox_wire_fuzzer`, and record/replay tests exist. The live editor no longer uses DRNR as its primary frame payload; it receives `FramePayload.finalBitmapPixels` plus split-preview bitmaps. Pattern paint servers and the full filter primitive chain are still stubbed in the DRNR codec. |
| S3 — partial pipeline + partial address bar | ✅ Superseded | The early `PipelinedRenderer`/slim-shell path has been replaced by the S7–S11 thin-client architecture. `SvgSource` is still used by the desktop fetcher and sandbox CLI tools. |
| S4 — record/replay + frame inspector | ✅ Landed | `.rnr`, `FrameInspector`, `sandbox_inspect`, `sandbox_replay`, and `sandbox_diff` are present. This stack is still DRNR-based and separate from the live editor's bitmap frame payload. |
| S5 — C++26 reflection | ⏸ Parked | Gated on toolchain support; hand-rolled codecs still in place. |
| S6.1 — portable hardening | ✅ Landed | `SandboxHardening` applies the env gate, `chdir("/")`, FD sweep, `setrlimit` caps, and Linux seccomp-bpf in fail-open (`EACCES`) mode. Linux tests cover env/rlimit/seccomp subprocess behavior. |
| S6.2 — platform jails | 🚨 P0 incomplete | Linux still returns `EACCES` for denied syscalls instead of `KILL_PROCESS`; macOS `sandbox_init` and per-UID isolation are not implemented. Closing S6.2 requires deny-operation tests on both desktop platforms and fail-closed backend startup. |
| S7 — long-lived `SandboxSession` | 🟡 Linux transport implemented; macOS missing | Persistent child, reader/writer threads, FIFO futures, and auto-respawn code exist behind Linux-only Bazel targets. `sandbox_session_tests` covers the transport on Linux; macOS has no session target yet, and respawn-after-crash is implemented but not currently pinned by a test. |
| S8 — editor-API wire protocol | ✅ Core implemented | `SessionProtocol`, `SessionCodec`, and `EditorApiCodec` cover editor API messages. Current `kFrame` carries final bitmap / composited preview / tree / selection / writebacks / diagnostics, not `renderWire`. `applySourcePatch`, `keyEvent`, `wheelEvent`, and non-select `setTool` are still placeholders. |
| S9 — host editor becomes a thin client | 🚨 P0 gap | `donner/editor/main.cc` drives `EditorBackendClient`, but macOS currently links `EditorBackendClient_InProcess`, so desktop macOS still parses in the host process. Linux links `EditorBackendClient_Session`, but no CI target exercises the actual `//donner/editor:editor` binary and proves its default backend is session-backed. This milestone is not complete. |
| S10 — unified address bar | ✅ Mostly implemented | Shared `AddressBar`, dispatcher, desktop fetcher, WASM fetcher, history, drag/drop payload path, progress chip, and status chip exist. Native desktop file picker and a true async/cancelable desktop curl fetch are not implemented. |
| S11 — `ResourcePolicy` + curl diagnostics | ✅ Implemented with MVP consent semantics | `ResourcePolicy`, `ResourceGatekeeper`, `CurlAvailability`, and tests exist. Typed address-bar navigations set `autoGrantFirstUse=true`, so first-use host consent is implicit for a user-entered URL instead of a modal prompt. `SvgSource` also added SSRF and hardened-curl defenses beyond the original plan. |
| S12 — parity + compositor follow-ups | 🟡 Partially complete | ViewBox plumbing, `setViewport` pipelining, select-drag correctness, source writebacks, composited final bitmaps, split-preview uploads, SVG text export, marquee metadata, tree summary, and texture-bridge stubs are present. Rich element inspector, hover metadata, source-patch fast path, keyboard/wheel/tool behavior, PNG export, and real zero-copy GPU bridge remain open. |

## Completion audit (2026-04-24)

**Verdict:** the design is **not complete as originally written**, and the remaining platform gap
is P0. The Linux session/backend plumbing exists: URL/file bytes are intended to flow through the
host fetcher into `donner_editor_backend`, and the host drives the document through
`EditorBackendClient` futures. However, the current tests exercise lower-level session/client
targets, not the actual `//donner/editor:editor` binary's default backend selection. The macOS
desktop build still links the in-process backend, so it definitely does not satisfy the
process-boundary goal. S6.2 platform confinement is also incomplete.

The implementation deliberately changed the live rendering contract. The original design treated
`RendererInterface`/DRNR as the only sandbox-to-host data flow for every live frame. Current
`EditorBackendCore::buildFramePayload()` instead rasterizes in the backend with
`svg::Renderer` + `CompositorController` and ships:

- `FramePayload.finalBitmapPixels` for the normal frame bitmap.
- `FramePayload.compositedPreview*` bitmaps plus a document-space translation during active
  single-element drags.
- Metadata for selection, marquee, tree summary, source writebacks, diagnostics, status, and
  `documentViewBox`.

That change is justified by the compositor work: layer caching and translation-only drag previews
only save work if the backend owns rasterization and can preserve compositor state across frames.
DRNR is still valuable, but it is now the debug/record/replay protocol rather than the live
editor's primary presentation protocol.

### Satisfied design goals

- **Linux session/backend plumbing.** `EditorBackendClient_Session`,
  `SandboxSession`, and `donner_editor_backend` exist and the backend binary owns parsing,
  the `SVGDocument`, selection, undo, and tool dispatch. CI coverage:
  `//donner/editor/tests:editor_backend_client_tests` and
  `//donner/editor/sandbox/tests:editor_backend_integration_tests`. This is not enough to claim
  the Linux editor binary is sandboxed by default; add the P0 editor-binary smoke test below.
- **No production host parse calls in non-exempt editor files.** Enforced by the
  `SVGParser::ParseSVG` rule in `build_defs/check_banned_patterns.py`, which is emitted as the
  per-target `*_lint` tests.
- **Long-lived backend transport.** `SandboxSession` keeps a persistent child and exposes the
  future-shaped request API. CI coverage: `//donner/editor/sandbox/tests:sandbox_session_tests`.
- **Editor API protocol.** Request/response codecs exist for the API-shaped session protocol.
  CI coverage: `//donner/editor/sandbox/tests:session_codec_tests` and
  `//donner/editor/sandbox/tests:editor_api_codec_tests`.
- **Address-bar fetch policy.** Desktop fetches go through `ResourceGatekeeper` and `SvgSource`;
  WASM uses browser fetch rules. CI coverage:
  `//donner/editor/tests:resource_policy_tests`,
  `//donner/editor/tests:curl_availability_tests`,
  `//donner/editor/tests:desktop_fetcher_tests`, and
  `//donner/editor/tests:address_bar_dispatcher_tests`.
- **Record/replay and render-wire debug tools.** DRNR, `.rnr`, frame inspection, replay, diff,
  and the render-wire fuzzer exist. CI coverage:
  `//donner/editor/sandbox/tests:wire_format_tests`,
  `//donner/editor/sandbox/tests:record_replay_tests`,
  `//donner/editor/sandbox/tests:sandbox_diff_tests`, and
  `//donner/editor/sandbox/tests:sandbox_wire_fuzzer`.

### Open gaps before declaring the design complete

- **P0: macOS process sandbox.** The macOS desktop editor still uses the in-process backend, and
  `donner_editor_backend` / `SandboxSession` are Linux-only Bazel targets. Implement the macOS
  session path and apply a `sandbox_init` profile before claiming desktop-wide parser isolation.
- **P0: actual editor-binary verification.** Existing tests prove the session and backend client
  can work, but they do not prove that the shipped editor binary defaults to the session-backed
  backend on Linux, and there is no equivalent macOS target. Add an editor-binary smoke test that
  runs before GUI initialization and fails if the selected desktop backend is in-process.
- **P0: host/parser dependency fence.** Desktop host code must be structurally unable to parse.
  The shipped editor host must not link `EditorBackendCore`, `EditorApp`, `backend_lib`,
  `SVGParser`, or any target that can call `SVGParser::ParseSVG`. The in-process backend is a
  WASM/test-only implementation, not a desktop fallback.
- **S6.2 syscall policy.** Linux seccomp still uses `SECCOMP_RET_ERRNO | EACCES` for denied
  syscalls; flip to `SECCOMP_RET_KILL_PROCESS` only after adding explicit tests for the final
  allowlist. macOS has no platform jail yet.
- **Frame-protocol fuzzing.** `sandbox_wire_fuzzer` covers DRNR, but there is no
  `editor_backend_request_fuzzer` or `frame_response_fuzzer` for `SessionCodec` /
  `EditorApiCodec` payloads. The malformed-codec unit tests are useful but not a substitute for
  fuzzing the live trust boundary.
- **Backend API placeholders.** `EditorBackendCore::handleApplySourcePatch`,
  `handleKeyEvent`, `handleWheelEvent`, and non-select `handleSetTool` return a fresh frame
  without implementing the requested behavior. SVG text export is implemented; PNG export is not.
- **Editor parity.** The tree summary and selection flow are present, but the rich element
  inspector still needs a flattened attribute/computed-style snapshot. Hover metadata is present
  in the wire shape but not populated. Source-pane patch debouncing/fast-path classification still
  needs completion.
- **Zero-copy bridge.** `BridgeTexture` stubs and macOS IOSurface allocation/import plumbing exist,
  but backend readiness remains false and the editor still relies on `finalBitmapPixels`.
  Linux dmabuf and Windows shared-handle paths are not implemented.
- **Enforcement of “ResourceGatekeeper is the only fetch gate.”** Production address-bar fetches
  use it, but this is not a banned-dependency/lint invariant today. CLI tools and tests may still
  call `SvgSource` directly by design.

## P0 Closure Plan: Make Desktop Host Parsing Impossible

The macOS miss happened because the architecture allowed a desktop fallback from
`EditorBackendClient_Session` to `EditorBackendClient_InProcess`. That fallback must not exist for
desktop builds. The next sandbox PR should treat this as the release blocker and reshape the build
graph so a platform-selection mistake cannot silently reintroduce parser code into the trusted
editor process.

### P0-A: Split targets by process role

- Move the desktop UI binary and host-only libraries into a host package boundary, for example
  `//donner/editor/host/...`. Host targets may depend on:
  - the `EditorBackendClient` interface,
  - `EditorBackendClient_Session`,
  - `SandboxSession`,
  - fetch/policy/UI/viewport/text-editor libraries.
- Keep parser-owning code behind a backend-only boundary:
  - `//donner/editor/backend_lib/...`,
  - `//donner/editor/sandbox:editor_backend_core`,
  - `//donner/editor/sandbox:donner_editor_backend`,
  - `//donner/svg/parser`.
- Use Bazel `visibility` as the primary enforcement mechanism. Backend/parser targets should be
  visible only to the backend binary, backend tests, fuzzers, and the WASM backend package. They
  should not be visible to `//donner/editor/host/...`.
- Move desktop-only overlay/selection helpers that currently depend on `backend_lib:editor_app`
  behind backend-owned frame metadata or into parser-free host helpers. A host overlay renderer may
  consume `SelectionOverlay`, `FrameTreeSummary`, and `documentViewBox`, but not `SVGDocument`,
  `SVGElement`, `EditorApp`, or `SelectTool`.

### P0-B: Remove desktop in-process fallback

- Change `EditorBackendClient_InProcess` to be compatible only with WASM and selected tests. It
  must not be a dependency of the desktop editor binary on Linux or macOS.
- Replace the current `#if defined(__linux__) ... #else MakeInProcess()` selection in
  `main.cc` with a desktop-only session factory:

```cpp
#if defined(__EMSCRIPTEN__)
  auto backend = EditorBackendClient::MakeInProcess();
#else
  auto session = sandbox::SandboxSession({
      .childBinaryPath = runfiles::Resolve("donner_editor_backend"),
  });
  auto backend = EditorBackendClient::MakeSessionBacked(session);
#endif
```

- Failure to launch the backend child is a startup/load failure with an actionable diagnostic; it
  is not an automatic fallback to in-process parsing. Any `--unsafe-in-process-backend` debug flag,
  if added, must be test-only or explicitly excluded from release/editor CI.

### P0-C: Add macOS session backend

- Make `//donner/editor/sandbox:session` and
  `//donner/editor/sandbox:donner_editor_backend` compatible with macOS as well as Linux.
- Audit `SandboxSession` for POSIX portability: `posix_spawn` file actions, pipe setup,
  close-on-exec/FD sweep, runfiles resolution, child shutdown, and abnormal-exit reporting.
- Add a macOS `SandboxHardening` path that applies a deny-by-default `sandbox_init` profile before
  parsing. The profile should allow only the already-open stdio pipes, memory allocation,
  required dynamic-loader/runtime operations, and process exit. File reads, writes, and networking
  should be denied after startup.
- Add macOS hardening tests that run in a child process and assert denied filesystem/network
  operations fail after the sandbox profile is applied.

### P0-D: Test the shipped editor binary selection

Lower-level session tests are not enough. Add a parser-free startup smoke mode to the real editor
binary, before GLFW/ImGui initialization, for example:

```sh
donner/editor/editor --backend-smoke-test
```

The smoke mode should create the same backend factory the GUI would use, load a tiny SVG, wait for
one frame, and print machine-readable diagnostics:

```json
{
  "transport": "session",
  "host_pid": 123,
  "backend_pid": 456,
  "frame_status": "rendered"
}
```

Add CI tests that invoke the real binary as data:

- `//donner/editor/tests:editor_binary_backend_smoke_test_linux`
- `//donner/editor/tests:editor_binary_backend_smoke_test_macos`

Both tests fail unless `transport == "session"` and `backend_pid != host_pid`. These are the
targets that make "the desktop editor does not parse in the host process" an enforceable claim.

### P0-E: Acceptance criteria

The sandbox design can be called complete for desktop only when all of these are true:

- `//donner/editor:editor` builds on Linux and macOS without depending on
  `EditorBackendClient_InProcess`, `EditorBackendCore`, `backend_lib`, or `//donner/svg/parser`.
- The editor-binary smoke tests above pass on Linux and macOS.
- `//donner/editor/sandbox/tests:sandbox_session_tests`,
  `//donner/editor/sandbox/tests:editor_backend_integration_tests`, and
  `//donner/editor/tests:editor_backend_client_tests` run on both Linux and macOS, or have
  separate platform-specific equivalents with the same coverage.
- A macOS `SandboxHardening` test proves the backend child cannot read arbitrary files or open
  network sockets after applying its profile.
- The only remaining in-process parser path is WASM or explicitly test-only code. No desktop
  production target can select it by platform accident.

## S6.2 Closure Plan: Platform Jails Must Fail Closed

S6.2 is also a completion blocker. Process separation without a real OS policy still leaves a
compromised parser child with whatever ambient authority the child process retains. The closure
work is not just "turn on stricter flags"; it must add tests that prove denied operations cannot
silently succeed on Linux or macOS.

### S6.2-A: Make denied-syscall behavior explicit

- Replace the implicit Linux fail-open behavior with an explicit option:

```cpp
enum class DeniedSyscallAction {
  kReturnErrnoForTests,
  kKillProcess,
};

struct HardeningOptions {
  DeniedSyscallAction deniedSyscallAction = DeniedSyscallAction::kKillProcess;
  // ...
};
```

- Production backend children use `kKillProcess`. Tests may opt into
  `kReturnErrnoForTests` only when they are intentionally auditing a new
  allowlist.
- `ApplyHardening()` failure remains fatal. The host reports a sandbox startup
  error; it does not retry in-process.

### S6.2-B: Close Linux seccomp

- Flip the Linux seccomp default action from `SECCOMP_RET_ERRNO | EACCES` to
  `SECCOMP_RET_KILL_PROCESS` for production hardening.
- Add a dedicated probe helper that applies hardening and then attempts exactly
  one forbidden operation per subprocess. With kill-on-deny, one child cannot
  test multiple probes.
- Probe and assert termination for at least:
  - opening an arbitrary filesystem path for read,
  - opening/creating a filesystem path for write,
  - `socket(AF_INET, SOCK_STREAM, 0)` / network creation,
  - `execve` or `posix_spawn`,
  - `fork` / `clone`,
  - `ptrace`.
- Keep a normal-render test under the final kill policy so allowlist mistakes
  fail by breaking `donner_editor_backend`, not by silently weakening the
  policy.
- Required CI target:
  `//donner/editor/sandbox/tests:sandbox_hardening_linux_kill_tests`.

### S6.2-C: Add macOS `sandbox_init`

- Add a macOS hardening implementation that applies a checked-in
  deny-by-default `sandbox_init` profile after the backend child has completed
  startup setup and before it reads untrusted SVG bytes.
- The profile should deny filesystem writes, arbitrary filesystem reads,
  network sockets, subprocess creation, Mach service lookup beyond what the
  already-started process needs, and other IPC outside stdio pipes.
- Any required read-only runtime allowances must be named and justified in the
  profile comments. Broad home-directory, current-working-directory, or
  network allowances are not acceptable.
- Add macOS probe tests mirroring the Linux denied-operation tests. On macOS
  these probes should assert the operation fails under the profile without
  granting the child usable authority.
- Required CI target:
  `//donner/editor/sandbox/tests:sandbox_hardening_macos_profile_tests`.

### S6.2-D: Wire hardening into the live backend path

- `donner_editor_backend` must call `ApplyHardening()` on Linux and macOS before
  request decoding and before `EditorBackendCore` sees untrusted bytes.
- `SandboxSession` must set the environment marker and launch with a curated
  environment on both platforms.
- The editor-binary smoke test should fail if the backend reports that hardening
  was skipped or degraded. Include a `hardening` field in the smoke JSON:

```json
{
  "transport": "session",
  "hardening": "platform-jail",
  "host_pid": 123,
  "backend_pid": 456,
  "frame_status": "rendered"
}
```

### S6.2-E: Acceptance criteria

- Linux denied-operation probes terminate under seccomp kill mode.
- macOS denied-operation probes fail under `sandbox_init`.
- Normal backend render/integration tests pass under the final hardening mode on
  both desktop platforms.
- `donner_editor_backend` cannot start without `DONNER_SANDBOX=1` and cannot
  continue if platform hardening fails.
- The editor host has no code path that downgrades from failed platform
  hardening to in-process parsing.

## P0 Red-Team Hook: Sandbox Escape Canary

We also need a deterministic way to prove the full editor path is actually using
the sandbox and that a compromised child cannot escape its OS policy. Add a
test/dev-only red-team canary hook that is exercised through the same
`EditorBackendClient` / `SandboxSession` path as a normal load. The canary is
not a parser fuzz test. It intentionally assumes arbitrary code execution
inside the backend child, attempts escape-style operations, and lets the host
verify those attempts cannot succeed.

### Canary shape

- Add a backend-only `SandboxProbeAction` enum, encoded either as a dedicated
  `kSandboxProbe` request or as smoke-test-only metadata attached to a
  `LoadBytes` request:

```cpp
enum class SandboxProbeAction {
  kCrashSigsegv,
  kAbort,
  kReadForbiddenPath,
  kWriteForbiddenPath,
  kOpenNetworkSocket,
  kSpawnProcess,
  kReadEnvironment,
  kConnectToHostIpc,
};
```

- The probe executes **inside `donner_editor_backend` after `ApplyHardening()`
  succeeds** and before any fallback or host-side parser path can run.
- The host may provide a checked-in probe fixture such as
  `donner/editor/sandbox/tests/probes/crash.svg`, but the marker must be
  consumed by the backend child, not by the host. The host's role is only to
  pass bytes and expected probe metadata through the normal backend client.
- The hook is gated by an explicit test/dev flag such as
  `--enable-sandbox-probes` or `DONNER_ENABLE_SANDBOX_PROBES=1`. Normal editor
  loads must ignore probe-looking SVG comments or metadata.
- The probe code lives in a small backend-only target such as
  `//donner/editor/sandbox:red_team_probes`. It must not be linked into host
  targets or normal parser/renderer libraries.

### Expected outcomes

- `kCrashSigsegv` and `kAbort`: backend child dies, the host process survives,
  reports a sandbox crash, keeps or clears the current document according to the
  normal crash policy, respawns a fresh child, and can render a normal SVG after
  the probe. Any host crash is a test failure. Any in-process retry is a test
  failure.
- `kReadForbiddenPath`: host creates a temporary sentinel file outside the
  sandbox contract and asks the child to read it. Success is a test failure.
  Acceptable outcomes are child termination under Linux kill-on-deny or an
  explicit denied result under macOS `sandbox_init`.
- `kWriteForbiddenPath`: child attempts to create or overwrite a host-provided
  sentinel path. The host asserts the path was not created or modified.
- `kOpenNetworkSocket`: child attempts to create/connect a TCP socket. The host
  may run a local listener and asserts no connection is accepted.
- `kSpawnProcess`: child attempts `posix_spawn` / `execve`. Success is a test
  failure.
- `kReadEnvironment`: child tries to read sensitive environment variables such
  as `HOME`, `SSH_AUTH_SOCK`, `AWS_*`, and `GITHUB_TOKEN`. The curated child
  environment should not contain them.
- `kConnectToHostIpc`: child attempts to connect to a host-controlled Unix
  domain socket or named pipe that is not one of the already-open sandbox
  stdio/session pipes. Success is a test failure.

### Red-team scenario bundle

In addition to one-probe-per-test coverage, add a bundled scenario that behaves
like a mini exploit chain:

1. Load a canary SVG through the real editor smoke path.
2. Backend probe code assumes arbitrary code execution and tries, in order:
   read a host sentinel file, write a marker file, open a TCP socket, spawn
   `/bin/sh`, read sensitive env vars, connect to a host IPC socket, then crash.
3. Host asserts:
   - the editor process survives,
   - no sentinel file contents were returned,
   - no marker file was created or modified,
   - no network or IPC connection was accepted,
   - no subprocess side effect occurred,
   - sensitive env vars were not present,
   - the backend child either died under policy or reported denied operations,
   - a fresh backend child can still render a normal SVG,
   - `fallback_used == false`.

This scenario is the red-team proof that "parser compromise inside the child"
does not become "host compromise."

### Editor smoke interface

Extend the real editor binary's parser-free smoke path:

```sh
donner/editor/editor \
  --backend-smoke-test \
  --enable-sandbox-probes \
  --sandbox-probe=crash-sigsegv
```

For crash probes, the editor smoke command should exit 0 only when it observes
the expected backend death and then successfully renders a follow-up normal SVG
through a new backend child. For denied-operation probes, it should exit 0 only
when the forbidden operation fails and the sentinel checks prove no host
resource was accessed.

Machine-readable output should include:

```json
{
  "transport": "session",
  "hardening": "platform-jail",
  "probe": "read-forbidden-path",
  "probe_result": "denied",
  "host_pid": 123,
  "backend_pid_before": 456,
  "backend_pid_after": 789,
  "fallback_used": false
}
```

### Required canary CI targets

- `//donner/editor/tests:editor_binary_sandbox_canary_crash_linux`
- `//donner/editor/tests:editor_binary_sandbox_canary_crash_macos`
- `//donner/editor/tests:editor_binary_sandbox_canary_escape_linux`
- `//donner/editor/tests:editor_binary_sandbox_canary_escape_macos`
- `//donner/editor/tests:editor_binary_sandbox_red_team_linux`
- `//donner/editor/tests:editor_binary_sandbox_red_team_macos`

These targets are the acceptance tests for the claim: "loading a hostile or
probe SVG can crash or compromise only the sandbox child, not the trusted editor
host, and the child cannot read/write files, open network sockets, or spawn
processes outside its policy."

### Fuzzable red-team surfaces

Yes, this should also be fuzzed, but split the fuzzing into fast in-process
targets and slower subprocess targets:

- **Fast codec fuzzers**:
  - `//donner/editor/sandbox/tests:session_codec_fuzzer` mutates DRNS frame
    headers, request ids, opcodes, payload lengths, and truncation patterns.
  - `//donner/editor/sandbox/tests:editor_backend_request_fuzzer` mutates live
    backend request payloads and asserts decode/dispatch returns `kError` or a
    valid response without crashing.
  - `//donner/editor/sandbox/tests:frame_response_fuzzer` mutates
    `FramePayload` bytes and asserts host-side decode plus `FrameResult`
    conversion never crashes or allocates beyond caps.
  - `//donner/editor/sandbox/tests:sandbox_probe_plan_fuzzer` mutates the
    canary/probe metadata and validates plan parsing, gating, enum handling,
    max step count, path canonicalization, and sentinel binding. This target
    must not perform real syscalls beyond normal fuzzer process activity.
- **Subprocess red-team fuzzer / corpus replay**:
  - `//donner/editor/tests:editor_binary_sandbox_red_team_fuzzer` maps fuzzer
    bytes to a bounded `SandboxProbePlan` (for example max four steps, no
    arbitrary host paths, timeouts per child) and runs the real editor smoke
    path in a subprocess. Each generated plan can crash or kill only the backend
    child; the fuzzer harness asserts the editor host survives and no sentinel
    escape succeeds.
  - Because this spawns hardened children and some inputs intentionally kill
    them, it should run as a nightly/continuous fuzz target or corpus replay in
    PR CI rather than an unbounded per-PR libFuzzer job.

Seed corpora should include:

- one input per `SandboxProbeAction`,
- malformed/truncated DRNS frames,
- oversized `FramePayload` lengths,
- probe plans with repeated crash steps,
- probe plans that interleave deny operations with normal SVG loads,
- platform-specific path/socket cases for Linux and macOS.

## Summary

Donner's editor loads arbitrary `https://`, `http://`, and `file://` SVGs from a
user-facing address bar. The parser is the largest fuzzer-exposed surface in the
editor, so the security goal is to keep parser, DOM, selection, undo, tool
dispatch, and rendering state out of the trusted UI host process.

The implemented architecture now has two related protocols:

1. **Live editor protocol (`DRNS`)**: the host calls the editor API remotely
   through `EditorBackendClient` and `SandboxSession`. The backend owns
   `EditorBackendCore`, `EditorApp`, `SVGDocument`, `SelectTool`, undo, and
   rasterization. Every mutating request returns a `FramePayload` with final
   bitmap data, composited-preview data, tree/selection metadata, source
   writebacks, diagnostics, and status.
2. **Render-wire protocol (`DRNR`)**: the debug/record/replay path serializes
   `RendererInterface` calls via `SerializingRenderer` and replays them through
   `ReplayingRenderer`. `.rnr`, `FrameInspector`, `sandbox_replay`, and
   `sandbox_diff` use this protocol.

Linux has the live subprocess backend plumbing, but the shipped editor binary
still needs a smoke test proving it selects that path by default. macOS desktop
currently uses the in-process backend implementation; that is the P0 gap. WASM
also uses the in-process backend, but there the browser is the sandbox. C++26
reflection remains an aspirational code-reduction milestone for the hand-written
codecs and is not required for the shipped behavior.

## Goals

- **Make desktop host parsing impossible**: `SVGParser::ParseSVG` runs only in a backend child
  process for desktop builds. Linux and macOS desktop host binaries must not link parser-owning
  code, backend implementation code, or the in-process backend. Bazel visibility, target
  compatibility, and editor-binary smoke tests enforce this.
- **Address bar**: editor gains a URL bar that accepts `https://`, `http://`,
  and `file://` URIs. Desktop loads route through `ResourceGatekeeper` and
  `SvgSource`; Linux then hands bytes to the sandbox backend.
- **Editor API as IPC**: the desktop process boundary is the editor API
  (`LoadBytes`, `ReplaceSource`, input events, undo/redo, export), not a DOM
  mirror. The host never materializes `SVGDocument` state in Linux or macOS
  desktop builds.
- **RendererInterface as debug/replay wire**: the 28-method `RendererInterface`
  remains the `.rnr` and frame-inspector protocol. It is no longer the live
  editor frame transport.
- **RendererRecorder → file format**: promote the planned in-memory
  `RendererRecorder` into a serializable format (`.rnr`, "Renderer Recording")
  that round-trips across the IPC boundary and to disk.
- **Frame inspector**: ImGui panel inside the editor that shows the command
  stream for the current frame, allows pausing before `endFrame()`, scrubbing
  to command index `N`, and inspecting individual command arguments.
- **Determinism**: replaying a `.rnr` file against the same backend at the same
  viewport must be pixel-identical for the feature subset the DRNR codec
  actually serializes. Live editor bitmap output is separately checked against
  direct renders by the sandbox golden-image tests.
- **No C++26 dependency on day one**: hand-rolled marshallers ship first;
  reflection is a follow-up that preserves the wire format.

## Non-Goals

- **Not a full browser sandbox yet.** Linux has process separation, FD cleanup,
  resource limits, and fail-open seccomp. macOS `sandbox_init`, Linux
  kill-on-deny seccomp, AppArmor/SELinux-style profiles, and per-UID isolation
  are still staged work.
- **Not cross-origin CSS/font/image fetches.** The sandbox child parses SVG
  *bytes*; it never initiates network requests. The host fetches the bytes
  over HTTPS and hands them to the sandbox. Sub-resource URL fetching (e.g.
  `<image href="https://...">`) is Future Work.
- **Not an OS subprocess in WASM.** The WASM build runs the backend in-process
  and relies on the browser sandbox as the trust boundary. The same
  `EditorBackendClient` surface is used so host UI code stays shared.
- **Not Windows in milestone 1.** Editor M3 targets macOS + Linux; the
  sandbox follows the same platform cut. Windows is Future Work.
- **Not a replacement for the fuzzer.** `SVGParser_fuzzer` continues to find
  bugs at the parser level. The sandbox is defense-in-depth, not defense-in-first.
- **Not a general-purpose IPC framework.** This is one editor-session protocol
  plus the existing DRNR replay protocol. No service discovery and no host-side
  DOM reflection.

## Next Steps

- [ ] **P0:** Split host/backend Bazel packages and visibility so desktop host
  targets cannot depend on parser-owning backend code.
- [ ] **P0:** Make `EditorBackendClient_InProcess` WASM/test-only and remove
  the desktop fallback from `main.cc`.
- [ ] **P0:** Implement the macOS `SandboxSession` / `donner_editor_backend`
  path and apply a `sandbox_init` profile so macOS desktop stops using the
  in-process parser.
- [ ] **P0:** Add editor-binary backend smoke tests for Linux and macOS that
  run `//donner/editor:editor --backend-smoke-test` and assert
  `transport=session` plus `backend_pid != host_pid`.
- [ ] **P0:** Close S6.2: Linux seccomp kill-on-deny, macOS
  `sandbox_init`, denied-operation probe tests, and no hardening downgrade path.
- [ ] Add live-protocol fuzzers: `editor_backend_request_fuzzer` for backend
  request decoding and `frame_response_fuzzer` for host frame decoding.
- [ ] Finish the remaining backend API handlers: `ApplySourcePatch`,
  keyboard/wheel behavior, non-select tools, and PNG export.
- [ ] Fill the remaining parity gaps: rich inspector snapshots, hover metadata,
  and source-pane debounce / fast-path classification.
- [ ] Complete or explicitly defer the zero-copy GPU bridge. Until then,
  `finalBitmapPixels` remains the authoritative live frame payload.

## Historical Implementation Plan

This checklist preserves the original S1-S6 plan. The progress snapshot and
completion audit above are authoritative for the current implementation state.

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
  - [x] Fuzz the deserializer. Target `sandbox_wire_fuzzer` feeds
        random bytes into `ReplayingRenderer` and asserts it never crashes
        — only returns `kError`. This is non-negotiable: the deserializer is
        the DRNR debug/replay trust boundary. `SandboxWire_fuzzer.cc` drives
        `ReplayingRenderer::pumpFrame` against a `RendererTinySkia` sink.

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
        - Dispatch: file reads on the host; HTTPS fetched by launching
          the system `curl` CLI through `posix_spawnp` with a fixed argv and
          curated environment (no `libcurl` link dep). The sandbox only ever
          sees raw bytes.
        - Status chip: `Loading…`, `Rendered`, `Crashed (sandbox)`,
          `Parse error`, etc.
  - [x] **Crash recovery**: if the sandbox child exits non-zero, show the
        error chip, keep the previously-rendered document on screen, and
        respawn the child on the next navigation.
  - [ ] Add parser-corpus replay through the address-bar/session path so every
        parser fuzzer corpus entry is classified as either a successful backend
        frame, a parse diagnostic, or a child crash. No `fuzz_replay_cli`
        integration exists yet.

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
  - [x] Structural-diff mode: load two `.rnr` files, show a diff of their
        command streams side-by-side. The `sandbox_diff` CLI plus reusable
        `SandboxDiff` library
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
│  Chrome ◀── metadata/bmp ────│─ Frame ◀┤          │   ├── AttributeWriteback             │
│                              │         │  Frame   │   ├── svg::Renderer                 │
│                              │         │          │   └── CompositorController          │
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
| **Live render output** | Backend | The backend owns `svg::Renderer` + `CompositorController` and sends `FramePayload.finalBitmapPixels` plus split-preview bitmap fields. The host uploads those pixels or cached preview textures; it does not replay DRNR for live frames. |
| **DRNR render wire** | Backend (producer) + Host/tools (consumer) | `SerializingRenderer` / `ReplayingRenderer` remain the record/replay and frame-inspector protocol. `.rnr` is still the rendering wire stream, but it is not the live editor frame payload. |
| **Text editor (`TextEditor`, `TextBuffer`, `TextPatch`)** | Host | Cursor, scroll, text-pane undo — all host-local. On text edits the host sends `ReplaceSource(bytes)` or `ApplySourcePatch(range, new_bytes)` to the backend. |
| **Address bar, resource policy, fetchers** | Host | Orthogonal to backend; host fetches bytes and hands them off as `LoadBytes(bytes)`. |
| **Frame inspector / `.rnr` record + replay** | Host/tools + DRNR producer | Inspector and replay decode DRNR streams locally. Live editor export currently returns SVG text; `.rnr` export remains a debug-tool concern. |

### Why one PR

The original implementation plan expected one PR because splitting this across four PRs would leave
the editor in hybrid states
where *some* operations go over IPC and others don't, with no
enforcement mechanism preventing new host-side parse calls. A single PR
lets the banned-pattern lint land on the first commit and makes every
call site visibly trace through the new API.

Current status: the split exists in Linux-only transport/backend targets, but it is not yet
enforced as the desktop architecture. macOS still uses the in-process backend, and Linux lacks an
editor-binary smoke test proving the shipped binary selects the session path. Closing those P0 gaps
takes precedence over the remaining S12 parity work.

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

Current implementation note: `SandboxHost::renderToBackend` remains the
one-shot DRNR path used by the sandbox CLI/debug tools and tests. It was not
rewired as a shim over `SandboxSession`; the live desktop editor should create a
separate `SandboxSession` and talk to `donner_editor_backend` through the S8
editor API. Today that is implemented only in the Linux selection path and is
not proven by an editor-binary smoke test. This keeps the historical `.rnr`
tooling independent from the live editor frame protocol.

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
Every response is *the same frame bundle*: the backend-rasterized final bitmap,
optional composited-preview payloads, selection overlays, tree/source metadata,
diagnostics, and status data the host needs to update its chrome / source pane /
status strip. DRNR render-wire bytes are now limited to `.rnr`, replay, diff,
and inspector tooling.

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
| `kSelectElement` | `{entityId, entityGeneration, mode}` | Tree-pane selection using opaque backend-assigned entity handles. |
| `kExport` | `{format}` | Request the current source bytes (SVG) or a rendered raster. Reply is `kExportResponse{bytes}`. |
| `kAttachSharedTexture` | `{kind, handle, width, height, rowBytes}` | Optional texture-bridge setup. Currently falls back to CPU bitmap payloads because bridge backends report not-ready. |
| `kShutdown` | `{}` | Graceful exit. Backend replies with `kShutdownAck` then exits. |

### Response opcodes (backend → host)

| Opcode | Payload | When sent |
|---|---|---|
| `kHandshakeAck` | `{protocolVersion, buildId, backendCapabilities}` | Once, after `kHandshake`. Buildid mismatch → host closes session. |
| `kFrame` | `{frameId, finalBitmap, compositedPreview, tree, selectionOverlay, sourceWritebacks, statusChip, parseDiagnostics, documentViewBox}` | **The default response** to every request that mutates the document or viewport. One frame per logical user action. |
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

```text
u64 frameId

// Final pre-composed RGBA frame. Present when a document is loaded and
// the backend is not relying solely on split-preview texture reuse.
u8  hasFinalBitmap
FrameBitmap finalBitmap {
  i32 width
  i32 height
  u32 rowBytes
  u8  alphaType
  u32 pixelLength
  u8  pixels[pixelLength]
}

// Split compositor preview for active single-element drags. The backend
// may send bg/promoted/fg/overlay bitmaps once, then only update the
// document-space translation on subsequent drag frames.
u8  hasCompositedPreview
u8  compositedPreviewActive
u8  hasCompositedPreviewBitmaps
f64 compositedPreviewTranslationDoc[2]
FrameBitmap background/promoted/foreground/overlay  // when bitmaps present

// Selection overlay metadata. Bboxes are in document space; the host
// applies its viewport transform when it needs UI-only chrome.
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

// Flattened tree summary for the sidebar. Entity ids are opaque and
// generation-checked by the backend; they are not DOM handles.
u64 treeGeneration
u32 rootIndex
u32 treeNodeCount
TreeNode treeNodes[treeNodeCount]

// SVG user-space coordinate system used by selection bboxes, marquee,
// hover rect, and pointer-event round trips.
u8  hasDocumentViewBox
f64 documentViewBox[4]
```

All variable-length fields are length-prefixed. The decoder on the host caps
lengths through `EditorApiCodec` / `SessionCodec`. The DRNR `kMax*` constants
still apply to `.rnr` / render-wire decoding, not to the live frame bitmap
payload directly.

### The backend's request loop

The backend binary (`donner_editor_backend`) runs a single thread that:

1. Reads a framed request from stdin.
2. Dispatches via a switch on `SessionOpcode`:
   - Input events → `EditorApp::applyMutation` / tool dispatch.
   - `kLoadBytes` / `kReplaceSource` → `AsyncSVGDocument::loadFromString`.
   - `kUndo` → `EditorApp::undo()`.
   - `kExport` → serialize + reply.
3. After the mutation settles, drains writeback queues into `Frame.writebacks`.
4. Runs the backend `svg::Renderer` / `CompositorController`, including the
   split-preview fast path for active drags.
5. Encodes final bitmap / preview bitmaps plus metadata into the `Frame` and
   writes it to stdout.
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

`sandbox_wire_fuzzer` still covers the DRNR render wire used by `.rnr` tools.
It does not cover the live `FramePayload` trust boundary; add the two fuzzers
above before treating S8's host/child boundary as fuzz-complete.

## S9: Host editor becomes a thin client

### The new `donner_editor_backend` binary

A new Bazel target `//donner/editor/sandbox:donner_editor_backend`
links:

- `//donner/editor/backend_lib:editor_app` and `:select_tool` — parser,
  document, undo, writeback, and tool behavior live in the backend-side library.
- `//donner/editor/sandbox:editor_backend_core` — transport-independent
  dispatch core that builds `FramePayload`s.
- `//donner/editor/sandbox:editor_api_codec` and `:session_codec` —
  encoders/decoders for the S8 messages.
- `//donner/editor/sandbox:sandbox_hardening` — existing `ApplyHardening()`
  so the backend inherits the same S6 jail.
- `//donner/svg/renderer` and `//donner/svg/compositor` through
  `editor_backend_core` — the backend now owns live rasterization.

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
  std::future<FrameResult> attachSharedTexture(const bridge::BridgeTextureHandle& handle);
  std::future<FrameResult> undo();
  std::future<FrameResult> redo();
  std::future<FrameResult> selectElement(uint64_t entityId, uint64_t entityGeneration,
                                         uint8_t mode);
  std::future<ExportResult> exportDocument(const ExportPayload&);

  // Callbacks for unsolicited backend pushes.
  using ToastCallback = std::function<void(ToastPayload)>;
  using DialogRequestCallback = std::function<void(DialogRequestPayload)>;
  void setToastCallback(ToastCallback);
  void setDialogRequestCallback(DialogRequestCallback);

  // Runtime state surfaced from the last Frame.
  [[nodiscard]] const SelectionOverlay& selection() const;
  [[nodiscard]] const svg::RendererBitmap& latestBitmap() const;
  [[nodiscard]] std::optional<Box2d> latestDocumentViewBox() const;
  [[nodiscard]] const sandbox::FrameTreeSummary& tree() const;
  [[nodiscard]] std::optional<ParseDiagnostic> lastParseError() const;
};

}  // namespace donner::editor
```

The session-backed client consumes one `SandboxSession`. The in-process client
owns an `EditorBackendCore` directly. Both decode the same `FramePayload` and
wrap `finalBitmapPixels` as the host's `RendererBitmap`; no host-side
`ReplayingRenderer` participates in the live editor path.

### `FrameResult`

```cpp
struct FrameResult {
  bool ok = false;
  uint64_t frameId = 0;
  // Bitmap produced by backend rasterization and carried in Frame.finalBitmap.
  svg::RendererBitmap bitmap;
  // Optional split-layer preview for active drags.
  std::optional<CompositedPreview> compositedPreview;
  // Updated selection chrome metadata.
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
  sandbox::FrameTreeSummary tree;
  std::optional<Box2d> documentViewBox;
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

`donner/editor/main.cc` hosts the address bar, text editor, viewport, texture
uploads, and frame polling. The current implementation constructs a
session-backed backend on Linux and an in-process backend elsewhere; that is the
P0 bug this document now calls out. The intended desktop structure is:

```cpp
int main(int argc, char** argv) {
  auto gatekeeper = ResourceGatekeeper(DefaultDesktopPolicy());
  #if defined(__EMSCRIPTEN__)
    auto backend = EditorBackendClient::MakeInProcess();
  #else
    auto session = sandbox::SandboxSession({
        .childBinaryPath = runfiles::Resolve("donner_editor_backend"),
    });
    auto backend = EditorBackendClient::MakeSessionBacked(session);
  #endif
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

The real loop is larger because it includes texture upload state, drag preview
textures, source-pane debounce, repro capture, and platform glue, but document
state still flows through `EditorBackendClient`. Desktop builds must fail
closed if the child cannot launch; they must not silently instantiate
`EditorBackendClient_InProcess`.

### The WASM build

WASM can't `posix_spawn`. Instead:

- `EditorBackendClient` has two implementations behind the same header:
  `EditorBackendClient_Session.cc` (desktop, talks to `SandboxSession`)
  and `EditorBackendClient_InProcess.cc` (WASM and selected tests only,
  calls an in-process `EditorBackendCore` directly).
- The **in-process variant statically links the backend library** — same
  `EditorBackendCore`, `EditorApp`, tools, and codecs — but skips the OS
  process boundary. It's the "backend running in the same address space as the
  client" case.
- The browser is the sandbox. `SVGParser::ParseSVG` runs in the WASM
  module, which is exactly what the browser's sandboxing was
  designed for.

This means:

- The WASM build compiles the banned-pattern lint-exempt
  `EditorBackendClient_InProcess.cc`, which is allowed to call into the backend
  library because the browser is the process sandbox.
- Linux and macOS desktop builds compile `EditorBackendClient_Session.cc` and
  must not depend on `EditorBackendClient_InProcess.cc`, `EditorBackendCore`,
  `backend_lib`, or `//donner/svg/parser`.
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
        "Desktop editor host code must route every parse through EditorBackendClient. "
        "Only the backend library, parser/engine code, the EditorBackendClient_InProcess "
        "path (WASM/test-only), and tests may call SVGParser::ParseSVG directly."
    ),
    exempt_path_prefixes=(
        "donner/svg/",
        "donner/benchmarks/",
        "donner/editor/sandbox/parser_child_main.cc",
        "donner/editor/sandbox/editor_backend_main.cc",
        "donner/editor/sandbox/EditorBackendCore.",
        "donner/editor/backend_lib/",
        "donner/editor/EditorBackendClient_InProcess.cc",  # WASM/test-only target.
        "donner/editor/sandbox/tests/",
        "donner/editor/tests/",
        "donner/editor/backend_lib/tests/",
        "donner/editor/repro/tests/",
        "examples/",
        "tools/",
    ),
)
```

This source-pattern lint is necessary but not sufficient: it prevents obvious
host-side parse calls, but it does not prevent the host binary from
transitively linking parser-owning backend targets. The P0 dependency fence
must add Bazel visibility/target-compatibility constraints so desktop host
targets cannot depend on `EditorBackendClient_InProcess`, `EditorBackendCore`,
`backend_lib`, or `//donner/svg/parser` in the first place.

### Tests

- `EditorBackendClient_tests.cc`: round-trip every API message through
  `_Session` and test-only/WASM `_InProcess`; assert identical `FrameResult`s.
- `editor_backend_integration_tests.cc`: spawn a real backend,
  reproduce the scenarios covered by today's `EditorApp_tests.cc`
  (hit-test, multi-select, drag writeback, undo) through the wire.
- `editor_backend_request_fuzzer.cc`: see §S8 fuzzing.
- Existing `EditorApp_tests.cc` continues to live in
  `donner/editor/backend_lib/tests/` (moved alongside the class).
- `editor_binary_backend_smoke_test_{linux,macos}`: P0 additions that run the
  real editor binary with `--backend-smoke-test` and prove the selected desktop
  backend is session-backed.

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
- Enter or the Load button navigates.
- The implemented widget has an LRU history menu, load-progress indicator,
  status chip, and drag/drop byte-payload entry point.
- Native desktop file picker and a real cancelable desktop fetch are still
  open. Desktop `SvgFetcher` currently completes synchronously; its `cancel`
  method is a no-op.
- Drag/drop is wired through `AddressBar::notifyDropPayload`; platform shells
  decide whether they can provide bytes or only a URI.
- Status chip: `Loading…`, `Rendered`, `Crashed (sandbox)`,
  `Parse error (line N)`, `Fetch error`, `Policy denied`. Colors
  match the existing slim-shell chip.

### Layout

`donner/editor/main.cc` renders the address bar above the editor panes. The
status chip wraps below the URL input so long fetch / parse diagnostics stay
readable on narrow windows.

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
  `ResourceGatekeeper` (S11). HTTPS fetches use `posix_spawnp("curl", argv)`
  with a curated environment, protocol restrictions, DNS pre-resolution, and
  private-address rejection.
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

Production address-bar fetches call `ResourceGatekeeper::resolve` before
`SvgSource::fetch`. This is a code-structure convention today, not a
banned-dependency invariant: CLI tools and tests still call `SvgSource`
directly, and no lint currently proves that every future production fetch path
uses the gatekeeper.

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
- Prompt on first use: the policy type can return `kNeedsUserConsent`, but the
  implemented address-bar fetcher passes `autoGrantFirstUse=true`. A URL typed
  into the bar is treated as the user's consent for that host in this MVP; no
  ImGui modal is implemented yet. Sub-resource fetchers should leave
  auto-grant off when that feature exists.
- Sub-resources blocked: matches original Non-Goals. Turning this on
  requires the subresource-fetch protocol which is still Future Work.

### Curl-missing diagnostic

`SvgSource::fetchFromUrl` now uses `posix_spawnp` with an argv vector and a
curated environment rather than `popen` through a shell. `CurlAvailability`
adds a one-shot availability probe so a missing `curl` becomes an actionable
policy denial before a fetch starts:

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
- `DesktopFetcher_tests.cc`: verifies consent handling through the desktop
  fetcher with and without auto-grant.
- There is no end-to-end fixture HTTPS server test yet; network fetch behavior
  is covered by `SvgSource_tests.cc` and the URL-security unit tests.

## Historical implementation order inside the single PR

This list records the intended review order from the S7–S11 planning phase.
It is no longer a current TODO list; use the completion audit and S12 remaining
work above for current status.

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

## S12: parity with the main-branch editor (post-S7–S11 follow-up)

The S7–S11 PR intentionally scopes to the **architecture split** — the
IPC boundary, the `EditorBackendClient` API, the thin-client shell,
the address bar, and the resource policy. It does NOT re-wire all of
main's editor UX behaviors through that boundary; that's S12.

The consequence: after S7–S11 lands, `//donner/editor:editor` runs
the thin-client shell against an in-process or subprocess backend,
but its on-pane behavior has regressed from main in specific,
enumerated ways. This section lists each gap, the test that pins it
(writing the test first per the project's debugging discipline), and
the approximate shape of the fix.

Intent: S12 doesn't re-architect anything. It ports
`EditorShell`-shaped UX wiring from main into the thin client,
replacing direct-`EditorApp` calls with `EditorBackendClient` calls
where necessary. New protocol fields are allowed but kept minimal.

### Parity gaps

| # | Symptom on sandbox | Root cause | Fix surface |
|---|---|---|---|
| G1 | "All black" pane on startup; selection chrome draws in the right place but the document doesn't | `viewport.documentViewBox` left as `Box2d::Zero` in the thin-client `main.cc`; `imageScreenRect()` returns a 0×0 rect so `ImGui::AddImage` draws into empty space | **Fixed in this PR.** Added `FramePayload.hasDocumentViewBox / documentViewBox[4]` to the wire protocol (back-compat-safe trailing optional). Backend fills from `svgElement().viewBox()` with `(0, 0, width, height)` fallback; client exposes it on `FrameResult.documentViewBox` and `latestDocumentViewBox()`; `main.cc` seeds `ViewportState::documentViewBox` from it on load + refresh in `ProcessFrameResult`. |
| G2 | Top-left corner of the document is centered in the pane instead of the document's center; clicks land in the wrong document-space region; AABB overlay appears in the wrong position | Same as G1. With `documentViewBox = Box2d::Zero`, `documentViewBoxCenter()` returns `(0, 0)` so `resetTo100Percent` anchors the doc's origin to the pane center; `screenToDocument(click)` yields coordinates in the rasterized-bitmap pixel space (when G1 was tactically seeded from bitmap dims) or garbage (when left zero), which the backend hit-tests against its own SVG-viewBox-space geometry and either misses or picks the wrong element. | **Fixed with G1.** Pinned by `ThinClientUiFlowTest.FramePayloadReportsSvgOwnViewBox`, `ClickOnScreenHitsCorrectElementInDocumentSpace`, and `DocumentCenterMapsToPaneCenter`. |
| G3 | `setViewport` is fired on every UI frame and its `FrameResult` is discarded via `(void)-`cast — backend re-rasterizes every frame for nothing, and the new bitmap never reaches the GL texture | Thin-client `main.cc` never pipelined `setViewport`'s future through `ProcessFrameResult` | `main.cc` tracks the last posted canvas size, only re-posts on change, and threads the returned future through `pendingFrame` so the bitmap gets uploaded. Pinned by `EditorBackendClientHostFlowTest.DiscardedSetViewportLeavesHostUploadFrozenAtInitialBitmap`. |
| G4 | **Element inspector pane shows only a tree list; no per-node attribute table, computed-style dump, or edit affordances** | `SidebarPresenter::renderInspector` in the main-branch library dereferences host-side `SVGElement` handles to format the attribute table — the thin client has no such handle, only the backend's opaque `entityId` | Either (a) add `InspectedElementSnapshot` to `FrameResult` (attribute name/value pairs + computed-style summary for the current selection), or (b) expose a new `inspectElement(entityId)` API that returns the snapshot on demand. (b) is cleaner since the inspector only opens when the user looks at it. |
| G5 | ~~No `--experimental` (composited drag preview) or `--save-repro` wiring on the sandbox thin-client~~ | Flags existed in the old shell but were not wired through the thin client. | **Fixed.** `--experimental` enables diagnostics around the compositor path; `--save-repro` records host-side input/frame state through the existing repro recorder. |
| G6 | ~~No drag feedback — click, see AABB, but dragging doesn't move the selection~~ **Drag now correct; see G6b for the perf story.** | Backend's `handlePointerEvent` did only hit-test + setSelection; `SelectTool` lived in `backend_lib` but wasn't instantiated by `EditorBackendCore` | **Fixed in this PR.** `EditorBackendCore` owns a `SelectTool` member; `handlePointerEvent` dispatches `kDown`/`kMove`/`kUp` (and folds `kCancel` → up) through it. Writebacks from drag-end flow through `FrameResult.writebacks`. Pinned by `ThinClientUiFlowTest.DragMovesSelectedElementAndCommitsWriteback`. |
| G6b | ~~Dragging is slow~~ **Partially fixed in this PR** (35ms → 16ms avg). Composited rendering ported to backend; tile-level host caching is the remaining phase-2 win. | Every `kMove` previously triggered a full `buildFramePayload` → full `SerializingRenderer` tree walk → full `ReplayingRenderer` re-raster on the host. | **Phase 1 done in this PR.** Backend now owns a real `svg::Renderer` + `CompositorController`; `buildFramePayload` runs `compositor_->renderFrame(viewport)` and ships the final pre-composed bitmap via new `FramePayload.finalBitmap` fields. Drag events route through `SelectTool` which mutates the drag target's transform; the compositor's translation-only fast path detects the pure-translation delta and reuses the cached layer bitmap instead of re-rasterizing. Additional fix along the way: `SVGDocument::setCanvasSize` unconditionally `invalidateRenderTree()`s, so `EditorBackendCore` now skips the call when the requested dimensions haven't changed (comparing against a local shadow, not `doc.canvasSize()` which reports the aspect-fit-scaled result). Pinned by `ThinClientUiFlowTest.CompositorModeSteadyStateDragIsFast` with a 30 ms budget against a 100-shape document. **Phase 2 (follow-up):** add per-tile fields to `FramePayload` so the host can cache individual layer bitmaps across frames and skip re-uploading the static bg/fg tiles on every drag frame — the compositor's `snapshotTilesForUpload()` already exposes the tile list with stable `tileId` + `generation`. |
| G7 | ~~No marquee metadata or multi-select shortcuts~~ Hover rect still missing | `SelectTool` was not originally instantiated by the backend. | **Mostly fixed.** Backend `SelectTool` handles shift-click, marquee, and multi-select drag. `FramePayload.hasMarquee / marquee[4]` is populated. `hasHoverRect / hoverRect` exists in the wire shape but is not populated yet. |
| G8 | ~~Zoom / pan gestures adjust `ViewportState` locally but the backend keeps rendering at its own `viewportWidth/Height`~~ | Thin-client zoom originally bypassed `desiredCanvasSize()` because `documentViewBox` was missing. | **Fixed with G1.** `main.cc` posts `setViewport(desiredCanvasSize())` only when the target canvas size changes and processes the returned frame. |
| G9 | Source-pane typos produce backend reparses per keystroke — no debounce | `main.cc` has `textChangePending` debounce logic inherited from main but `ApplySourcePatch` payload is sent too eagerly | Tune the debounce threshold / hold condition. Not load-bearing, but noticeable. |
| G10 | ~~SVG Save-As returns empty bytes~~ PNG export still missing | `EditorBackendCore::handleExport` was initially a placeholder. | **Partially fixed.** SVG text export returns `EditorApp::cleanSourceText()` through `kExportResponse`. PNG export is still not wired. |

### S12 PR structure — landed, in progress, remaining

Unlike S7–S11 which had to land as a single PR for the banned-pattern
lint to stick, S12 is a collection of ~10-14 narrower PRs. Status as
of this commit:

**Landed this PR:**

- [x] **G1/G2 `documentViewBox` plumbing** — `FramePayload.documentViewBox`
      added, thin-client consumes via `backend->latestDocumentViewBox()`.
- [x] **G3 `setViewport` pipelining audit** — `(void)setViewport` bug
      fixed, `pendingFrame` now threads through `ProcessFrameResult`.
- [x] **G5 `--experimental` + `--save-repro` wiring** — both CLI flags
      parsed; `--experimental` enables drag stderr instrumentation,
      `--save-repro` drives the existing `ReproRecorder` on the host
      side (once-per-frame `snapshotFrame()` + on-exit `flush()`).
- [x] **G6a drag correctness** — `SelectTool` wired into
      `EditorBackendCore::handlePointerEvent`; drag moves elements,
      commits writeback.
- [x] **G6b phase 1: composited rendering port** — backend owns
      `svg::Renderer` + `CompositorController`; ships
      `FramePayload.finalBitmap`; selection chrome baked into the
      bitmap via `OverlayRenderer::drawChromeWithTransform`; drag
      fast path verified under
      `CompositorModeSteadyStateDragIsFast` (35 ms → 18 ms/frame).
- [x] **G8 zoom/pan re-wiring** — `desiredCanvasSize()` replaces the
      direct pane-size fallback once `documentViewBox` is populated.
- [x] **Bridging phase A** — `BridgeTexture{.h,.cc}` interface +
      CPU stub; `BridgeHandleKind` enum; host/backend stub factories.
- [x] **Bridging phase B part 1** — macOS `IOSurfaceCreate` host +
      `IOSurfaceLookup` backend; `kAttachSharedTexture` session
      opcode; `EditorBackendClient::attachSharedTexture()`;
      `handleAttachSharedTexture` in the backend core; `main.cc`
      fires attach on startup.
- [x] **Rust patch, unconditional source build** —
      `third_party/patches/wgpu-native-iosurface-export.patch`
      (the concrete diff), `bazel_dep(name = "rules_rust")` +
      toolchain, `crate.from_cargo` extension wired into
      `MODULE.bazel`, `@wgpu_native_source//:wgpu_native` as the
      single wgpu-native target (no flag, no prebuilt fallback).
      Patched exports return null today — the MTLDevice extraction
      itself is the follow-up.

**Remaining work** (in priority order for the next PR spate):

1. **Bridging phase B part 2 — fill in the Rust extraction.**
    Replace the `std::ptr::null_mut()` stubs in
    `@wgpu_native_source//src/iosurface.rs` with a real
    `Global::device_as_hal::<wgpu_hal::api::Metal, _, _>` callback
    that yields the `MTLDevice` pointer. Mirror for Vulkan on
    Linux. **Estimated: 4-6 hours.** Fallback: the Metal-bypass
    backup plan below.
2. **Bridging phase B part 3 — host GL `CGLTexImageIOSurface2D`.**
    Populate `MacOSHost::glTextureId()` so ImGui's `AddImage`
    samples the shared IOSurface. Needs the GL context from
    `main.cc`. **Estimated: 1-2 hours.** Independent of part 2;
    can land first.
3. **Bridging phase B engagement.** Flip `MacOSBackend::ready()`
    to return `true` once parts 2+3 are in; flip
    `buildFramePayload` to skip `finalBitmapPixels` when
    `bridge_->ready()`. One-line change; test covers the flip.
4. **G6b phase 2 — per-tile host texture cache.** Expose
    `CompositorController::snapshotTilesForUpload()` across the
    IPC boundary (add a tiles array to `FramePayload` with
    `{tileId, generation, paintOrderIndex, bitmap,
    compositionTransform}` per entry). Host caches GL textures
    keyed on `tileId`; only re-uploads when `generation` bumps.
    Orthogonal to bridging — bridging saves bytes; tile caching
    saves work. **Estimated: 2-3 days**, mostly protocol +
    host-side cache.
5. **G4 element inspector.** `inspectElement(entityId)` API +
    host sidebar UI. Needs a flattened name/value snapshot per
    element (no DOM mirror — see Invariants).
6. **G7 hover serialization.** `FramePayload.hasMarquee / marquee` is now
    populated from `SelectTool::marqueeRect()`. Hover tracking still needs a
    backend source of truth and `FramePayload.hasHoverRect / hoverRect`
    population.
7. **G9 source-pane debounce tuning.** Host-only. Tune
    `kTextChangeDebounceSeconds` so keystroke storms don't spam
    `kReplaceSource`.
8. **G10 PNG export handler.** SVG text export now returns
    `EditorApp::cleanSourceText()`. PNG export should encode the latest
    `finalBitmapPixels` or render on demand through the backend renderer.
9. **Bridging phase C — Linux dmabuf.** Blocked on seccomp
    audit for `SCM_RIGHTS` receive during handshake. The Rust
    patch from phase B part 2 also needs the Vulkan-hal variant
    (`wgpuTextureCreateFromVulkanDmabufFd` or similar). **Scope:
    ~1 week once the Rust pipeline from phase B is working.**
10. **Bridging phase D — Windows shared handle.** Lowest
     priority; ship when the WASM-browser-WebGPU path settles.

**Testing surface.** Each of those is independently testable under
the `EditorBackendClient` harness the current PR introduces — no
new test infrastructure required.

### Invariants S12 must preserve

- **IPC shape stays `FrameResult`-centric.** S12 adds fields to
  `FrameResult` as needed (inspector snapshot, drag hints, document
  viewBox) but does not add new round-trip response types. The host
  shouldn't grow a family of small request/response pairs —
  everything flows through the frame bundle.
- **No host-side `SVGDocument` reintroduction.** G4 in particular is
  tempting to solve by shipping a DOM mirror; that reopens the
  attack surface the whole sandbox exists to close. The inspector
  snapshot must be a flattened name/value structure, not an
  entt-reflection.
- **Banned-pattern lint stays green.** Any new host-side file that
  reaches for `SVGParser::` or `svg::components::*` fails the lint;
  S12 PRs must re-check that gate.

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

At the time this design started there was no existing IPC, process separation,
shared-memory, or sandboxing code in the tree. The current implementation now
has `SandboxHost`, `SandboxSession`, `donner_editor_backend`, `SandboxHardening`,
and `BridgeTexture` scaffolding.

## Requirements and Constraints

**Functional:**
- Linux and macOS desktop SVG loads route through a session-backed backend
  process. No desktop fallback to `EditorBackendClient_InProcess` is allowed.
- Desktop host targets cannot link parser-owning code (`EditorBackendCore`,
  `backend_lib`, `SVGParser`, or any target that can call
  `SVGParser::ParseSVG`). Enforce with Bazel visibility and target
  compatibility, not only source-pattern lint.
- The shipped editor binary has a parser-free `--backend-smoke-test` path that
  proves the selected desktop backend is `session` and the backend PID differs
  from the host PID on Linux and macOS.
- Wire format is stable within a milestone; breaking changes bump the
  4-byte version header.
- Record/replay files are DRNR-based and validated on the same backend for
  the codec-supported feature subset.

**Quality:**
- Desktop sandbox roundtrip (parse + render + IPC + bitmap upload) should add
  no more than **2× the in-process render time** at p50 for the existing test corpus.
  The goal is "you don't notice"; a 2× budget leaves room to optimize.
- Child process spawn + first-render latency ≤ 80 ms at p95 on Linux and macOS
  desktop dev machines.
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

## Historical DRNR-first proposed architecture

The sections below preserve the original DRNR-first architecture proposal for
context. They are not authoritative for the current live editor path where they
conflict with the completion audit, S7-S12 status, or `FramePayload.finalBitmap`
design above. In particular, claims that `RendererInterface` is the only
sandbox-to-host live data flow now apply only to the `.rnr`/debug path.

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

### Rendering data flow (no shared textures)

A question that comes up on every code review of this area: *how do the
pixels get from the sandbox back to the UI?* Short answer: **there is no
shared GPU memory and no shared texture handle.** The sandbox boundary
sits strictly below the renderer command interface, so pixels live only
on the host side — the backend emits **draw-command bytes**, the host
re-rasterizes them. That decision is load-bearing; the rest of this
section unpacks why and what it means in practice.

```
  Backend (sandbox)                              Host (thin client)
  ───────────────────                            ──────────────────────
  EditorBackendCore                              EditorBackendClient
       │                                              │
       │  (operates on EditorApp)                     │
       ▼                                              │
  SerializingRenderer                                 │
       │  encodes draw calls                          │
       │  as opcode stream                            │
       ▼                                              │
  FramePayload.renderWire  ═══ IPC / move ══▶   ReplayingRenderer
       │                    (Linux subprocess           │
       │                     pipes or in-process        ▼
       │                     vector move)          svg::Renderer
       │                                           (tiny-skia / Geode /
       │                                            whatever the host has)
       │                                                │
       │                                                ▼  cpu/gpu raster
       │                                           RendererBitmap (RGBA)
       │                                                │
       │                                                ▼  one copy
       │                                           glTexImage2D → GL texture
       │                                                │
       │                                                ▼
       │                                           ImGui::Image in render pane
```

The backend builds one `SerializingRenderer` per call to
`buildFramePayload()`; it holds no pixels and never calls into any
graphics driver. The host builds one long-lived `svg::Renderer` per
client; each frame it runs a fresh `ReplayingRenderer` over the wire
bytes, dispatching every opcode onto that renderer. `takeSnapshot()`
reads the resulting RGBA buffer; `glTexImage2D` copies it into a
texture; ImGui samples the texture for the render pane.

Consequences worth internalizing:

1. **The backend touches no GL/Metal state.** This is the whole point —
   the subprocess sandbox on Linux uses seccomp to refuse the syscalls
   GL would need, and can't open `/dev/nvidia*` or a display server.
   Command-level replay is what lets the same backend binary run
   identically under subprocess sandboxing, in-process on macOS, and
   in-process in a WASM worker.

2. **Rasterization cost is on the host.** The backend pays for
   `prepareDocumentForRendering` + `RendererDriver` traversal +
   opcode encoding; the host pays for decode + the actual pixel
   work (tiny-skia scan-conversion, or Geode's WebGPU shaders).
   Perf work on the render pane budget has to measure both halves.
   See `editor_backend_golden_image_tests` for a pixel-identity
   gate that proves the two renderers agree on simple documents —
   a divergence there points at `SerializingRenderer` dropping
   calls or `ReplayingRenderer` mis-decoding them.

3. **Every frame is a full re-raster.** We don't (yet) diff the wire
   stream or cache bitmaps across `FramePayload`s on the host side;
   a setViewport or replaceSource call always produces a full
   `renderWire` from the backend, and the host always replays it
   end-to-end. That's acceptable for the sandboxed S7–S11 milestone
   (the previous in-process editor also re-rastered per frame); S12
   is where we integrate the compositor caching from #531.

4. **The bitmap is a value type.** `FrameResult.bitmap` owns its
   pixel `std::vector<uint8_t>`. There's no lifetime entanglement
   between the backend's buffers and the host's GL texture; we can
   drop the backend half and keep rendering from the cached
   `latestBitmap_`.

**Pitfall: discarding a `FrameResult` discards the bitmap.** Every
mutating API on `EditorBackendClient` (including `setViewport`,
`pointerEvent`, `replaceSource`, `undo`/`redo`) returns a
`std::future<FrameResult>` whose `.bitmap` is the authoritative
post-mutation rasterization. If the host drops the future without
running it through `ProcessFrameResult`, the backend's internal state
advanced but the GL texture is frozen at whatever the last explicit
upload produced. The editor's thin-client `main.cc` hit this exact
bug by firing `(void)backend->setViewport(...)` once per UI frame and
never re-uploading — on a pane larger than the backend's 512×384
default, the texture stayed at the tiny startup bitmap and the user
reported "nothing renders." Fix: pipeline the returned future through
`pendingFrame`, and only post `setViewport` when the canvas size
actually changed. The test
`EditorBackendClientHostFlowTest.DiscardedSetViewportLeavesHostUpload
FrozenAtInitialBitmap` pins the failure mode.

**Pitfall: `ViewportState::documentViewBox` is the SVG's own viewBox,
not the rasterized bitmap's dimensions.** All three of these host-
side facilities live in SVG user-space (document) coordinates:
selection bboxes (`FrameResult.selection.worldBBox`), the pointer-
event inputs (`PointerEventPayload.documentPoint`), and the
viewport transforms (`documentToScreen` / `screenToDocument`).
The rasterized bitmap is SVG user-space *mapped through
`canvasFromDocumentTransform`* into canvas pixels — the same shape
as the viewBox only when the SVG's intrinsic size and aspect
happen to match. Seeding `documentViewBox` from bitmap dims
produces three correlated failures that look like one
"miscalibrated editor" bug:

1. `imageScreenRect() = documentToScreen(documentViewBox)` returns a
   rect in the wrong coordinate space — the image draws at the
   wrong place or at 0×0 (when documentViewBox is left zero).
2. `resetTo100Percent()` anchors `panDocPoint = documentViewBoxCenter()`.
   A bad viewBox means the doc's top-left lands at the pane's
   center instead of the doc's center.
3. `screenToDocument(click)` yields coords in the wrong space. The
   backend hit-tests those against its own viewBox-space geometry
   and either misses or picks the wrong element. The resulting
   AABB chrome then draws at the wrong point too — two wrongs
   that look deceptively like a single consistent offset.

**The fix:** the backend ships the SVG's viewBox in
`FramePayload.documentViewBox` (4 doubles, fall back to `(0, 0,
widthAttr, heightAttr)` when there's no explicit viewBox attribute).
The host reads it out of `FrameResult.documentViewBox` (or the
cached `client->latestDocumentViewBox()`) and seeds `Viewport
State::documentViewBox` from it on load and in `ProcessFrameResult`.
`desiredCanvasSize() = documentViewBox * zoom * dpr` then produces
a sensible canvas size, and zoom/pan gestures update
`panScreenPoint` / `zoom` without re-rasterize calls.

### Compatibility with Geode (WebGPU / wgpu-native)

**Yes, compatible — but suboptimal.** Geode renders on the GPU via
**wgpu-native** — the C ABI over Mozilla's Rust `wgpu` crate
(Firefox's WebGPU impl). See `donner/svg/renderer/geode/BUILD.bazel`
which fetches prebuilt `libwgpu_native` tarballs; we migrated off
Dawn (Google/Chrome's impl) a few releases ago. `svg::Renderer::
takeSnapshot()` does a `wgpuTextureCopyToBuffer` + buffer map
readback that produces a CPU RGBA buffer. We then ship those bytes
over IPC and the host uploads them to GL via `glTexImage2D` — a
round-trip GPU → CPU → CPU → GPU on every frame.

The GPU → CPU readback is **~2-4 ms** per 1280×720 frame on
integrated GPUs, wasted work if the host is going to turn around
and upload it right back. Geode's layer-cached drag path still
saves work (the cached layer bitmap is reused with a fast-path
compose-transform update, no per-frame re-rasterize), but every
frame still pays the readback tax.

### Cross-process texture bridging (design)

**Goal.** Replace the `wgpuTextureCopyToBuffer` → memcpy →
`glTexImage2D` round-trip with a zero-copy shared GPU texture that
the backend writes and the host samples. Because we're on
wgpu-native, the backend side is tractable — wgpu's `hal` layer
exposes raw platform handles (`MTLTexture`, `VkImage`,
`ID3D12Resource`) via `as_hal::<T, _>` in Rust, and wgpu-native's
C ABI surfaces the same capability via `wgpuTextureGetHal*`
helpers.

```
  Backend                                     Host
  ──────────                                  ───────────────
  wgpu::Device                                GL context
     │                                             │
     │   wgpu::Texture                             │   GLuint
     │       │                                     │       │
     ▼       ▼                                     ▼       ▼
  BridgeTextureBackend ── Transport ──▶ BridgeTextureHost
   (imports host-allocated                   (wraps handle as a
    native texture via wgpu hal)              GL texture the
                                              ImGui shader samples)
```

#### Transport primitive (platform-specific)

| Platform | Shared-surface type | Creation (host) | Wire format |
|---|---|---|---|
| macOS | `IOSurface` | `IOSurfaceCreate` | `mach_port` via `mach_msg` |
| Linux | `dmabuf` | `VkDeviceMemory` + `VK_EXT_external_memory_fd` | `int fd` via `SCM_RIGHTS` |
| Windows | D3D11/12 shared handle | `D3D11_RESOURCE_MISC_SHARED_NTHANDLE` | `HANDLE` via `DuplicateHandle` |

New files at `donner/editor/sandbox/bridge/` (one per platform +
shared interface). The session codec grows a new opcode
`kAttachSharedTexture` carrying the platform FD/handle once per
session, before any `kLoadBytes`; subsequent `FramePayload`s refer
to the bound texture by integer id + optional dirty-region rect.

#### Backend side (wgpu-native texture import)

`svg::Renderer` (Geode backend) grows an alternate rasterization
target:

```cpp
// New sibling to takeSnapshot — binds the renderer's output to a
// pre-allocated shared GPU texture instead of the internal scratch
// pixmap. Caller owns the texture; Renderer holds a non-owning view.
void attachSharedTexture(BridgeTextureHandle handle);
```

Under wgpu-native, the implementation on macOS is
`wgpuHalCreateTextureFromIOSurfaceMetal(device, ioSurface, &desc)`
(the exact symbol depends on our pinned wgpu-native version — check
`ffi/src/wgpu_hal.h`). Linux uses
`wgpuHalCreateTextureFromVulkanDmabufFd(...)`; Windows uses
the D3D12 equivalent. Because wgpu is one library across all three,
the C++ caller is platform-branching by one layer, not
per-renderer-backend branching.

The tiny-skia backend doesn't participate in the bridge — it's
CPU-rasterizing — so on tiny-skia we fall through to the existing
`finalBitmapPixels` wire field and pay the copy. Feature flag on
`CompositorConfig::bridgeToHostTexture` (default off, enable per-
platform per-renderer-backend as each lands).

#### Host side (shared handle → GL texture)

| Platform | GL interop extension |
|---|---|
| macOS | `CGLTexImageIOSurface2D` (Apple, pre-existing) |
| Linux | `EGL_EXT_image_dma_buf_import` + `glEGLImageTargetTexture2DOES` |
| Windows | `WGL_NV_DX_interop2` (or switch the host to D3D) |

Each produces a `GLuint` ImGui's existing `AddImage` path samples
with no shader changes.

#### Security: why bridging is safe across the sandbox

The sandbox guarantee is "backend can't read or write arbitrary
host memory / fds." Sharing a texture naively breaks this; we
preserve the guarantee by construction:

- **Unidirectional data flow.** Backend writes (render target),
  host reads (sampler). The host allocates the texture with the
  appropriate usage flags and hands the backend a handle with
  render-target-only usage. Backend cannot allocate new shares
  or upgrade usage post-facto.
- **Pre-allocated, fixed-size.** The handle is passed once at
  session start via `kAttachSharedTexture`. Backend can't grow
  it or request additional shares.
- **No new sandbox syscalls.** Fd-receive happens on the host
  pre-fork (`SCM_RIGHTS`) or on the backend before `seccomp(2)`
  locks down. We already do this dance for stdin/stdout on
  Linux and `mach_msg` on macOS — one more handle in the
  handshake is a bounded addition.

The one sensitive bit is "backend scribbling garbage into a
texture the host will trust." We mitigate by: (a) host samples
with a trust-but-verify size-clamp shader uniform, and (b) the
texture is single-purpose (this compositor session only) — no
cross-session or cross-user reuse.

#### Implementation plan and status

Phase order — each phase is independently testable + shippable:

- [x] **Design locked in this doc.** ☝️
- [x] **Phase A: `BridgeTexture` interface + CPU stub.** Landed at
      `donner/editor/sandbox/bridge/BridgeTexture.{h,cc}`. Abstract
      `BridgeTextureHost` + `BridgeTextureBackend` surface;
      `MakeHostStub` / `MakeBackendStub` factories; `BridgeHandle
      Kind` enum enumerates each platform. Tests at
      `bridge_texture_tests`.
- [x] **Phase B, part 1: macOS IOSurface + transport opcode.**
      `BridgeTexture_macOS.cc` ships `IOSurfaceCreate` host
      factory + same-process `IOSurfaceLookup` backend factory.
      Session protocol gains `kAttachSharedTexture`,
      `EditorApiCodec` gains `AttachSharedTexturePayload` +
      encode/decode, `EditorBackendClient::attachSharedTexture()`
      is live on both in-process and session-backed clients, and
      `EditorBackendCore::handleAttachSharedTexture` imports the
      handle + stashes the bridge. `main.cc` fires the attach on
      macOS at startup before `loadBytes`. `BridgeTextureBackend::
      ready()` still returns `false` — CPU `finalBitmapPixels`
      wire path stays authoritative until phase B part 2.
- [ ] **Phase B, part 2: wgpu-native `IOSurface` → `MTLTexture`
      import.** BLOCKS zero-copy engagement on macOS. Grepping our
      pinned wgpu-native C ABI (`webgpu.h` + `wgpu.h`) for
      `IOSurface` / `MTLTexture` / `ExternalTexture` returns zero
      symbols — this path needs a Rust-side patch on wgpu-native.
      See "Rust-side wgpu-native patch" below.
- [ ] **Phase B, part 3: host GL `IOSurface` → `GLuint` via
      `CGLTexImageIOSurface2D`.** Populates `MacOSHost::
      glTextureId()` so ImGui's `AddImage` samples the shared
      surface instead of the uploaded-from-CPU texture. Lands
      after part 2 so the first version tested has a GPU
      producer to point at.
- [ ] **Phase C: Linux dmabuf.** Blocked on auditing seccomp for
      `SCM_RIGHTS` receive during handshake. Send is host-side +
      pre-sandbox; receive is backend-side and must happen before
      `prctl(PR_SET_NO_NEW_PRIVS)`. Also needs the same Rust
      patch applied to wgpu-native's Vulkan hal (export an
      external-memory-fd → `VkImage` → `wgpu::Texture` path).
- [ ] **Phase D: Windows.** Lowest priority until the WASM-
      browser-WebGPU path settles.

### Rust-side wgpu-native patch (phase B, part 2)

wgpu-native's Rust source exports the `hal` module internally
(not through the C ABI) and `wgpu-hal::metal` already has
`Device::texture_from_raw(MTLTexture)`. We need a thin C wrapper —
tentatively `wgpuTextureCreateFromIOSurfaceMTL` — that takes an
`IOSurfaceRef` + `WGPUDevice` and returns a `WGPUTexture` C++ can
bind as a render-pass color attachment.

**Target files in the gfx-rs/wgpu repo** (we pin a tagged release
via prebuilt tarball at `third_party/bazel/non_bcr_deps.bzl`):

1. **`wgpu-native/src/lib.rs`** — add the exported function:
   ```rust
   #[cfg(target_os = "macos")]
   #[no_mangle]
   pub unsafe extern "C" fn wgpuTextureCreateFromIOSurfaceMTL(
       device: native::WGPUDevice,
       surface: *mut std::ffi::c_void,   // IOSurfaceRef
       desc: *const native::WGPUTextureDescriptor,
   ) -> native::WGPUTexture {
       // Extract MTLDevice from wgpu Device via the Metal hal.
       let mtl_device = /* device.as_hal::<wgpu_hal::metal::Api, _, _>(…) */;
       let mtl_tex = mtl_device.new_texture_from_iosurface(
           surface.cast(), 0, convert_desc(desc));
       let hal_tex = wgpu_hal::metal::Texture::from_raw(mtl_tex);
       wgpu_core::device::texture_from_hal::<wgpu_hal::metal::Api>(
           device, hal_tex, &convert_desc(desc),
       ).into_ffi()
   }
   ```

2. **`wgpu-native/ffi/src/wgpu.h`** — add the matching C prototype
   inside `#if defined(__APPLE__)`.

3. **`wgpu-hal/src/metal/mod.rs`** — likely no change; verify
   `Texture::from_raw` is exposed (may need to remove a `pub(crate)`
   gate).

**Integration on Donner's side**, once the patched wgpu-native is
pinned in:

```cpp
// donner/editor/sandbox/bridge/BridgeTexture_macOS.cc — replaces
// the `bindAsRenderTarget` no-op with the real import.
void MacOSBackend::bindAsRenderTarget() {
  if (!surface_.valid() || device_ == nullptr) return;
  WGPUTextureDescriptor desc = { /* w,h, BGRA8Unorm, RENDER_ATTACHMENT */ };
  wgpuTexture_ = ::wgpuTextureCreateFromIOSurfaceMTL(
      device_, const_cast<void*>(static_cast<const void*>(surface_.get())), &desc);
  // Subsequent Geode render passes bind `wgpuTexture_` as the color
  // attachment instead of the internal scratch target.
}
```

**Build-system delta.** We currently fetch wgpu-native via prebuilt
tarballs. The patched version needs either:
- **(a)** a fork built from source with `rules_rust` + `cargo_
      bootstrap_repository` — long-term correct but multi-day
      Bazel/CI investment;
- **(b)** a locally-built tarball uploaded to our release
      artifacts, SHA-pinned in `non_bcr_deps.bzl` — faster to
      land, needs one-time build machinery but no Bazel-Rust
      interop.

Option (b) is the right iteration-speed bet; option (a) is the
long-term home once we grow other Rust deps.

**Effort estimate:** ~3-5 days of Rust work (hal surface already
exists, Mach API integration is straightforward), plus 1-2 days
of Bazel/CI wiring for option (b).

For the current milestone: phase 2 of the composited-rendering
port (G6b tile-level host cache) is still the biggest incremental
win without any GPU plumbing. The bridging work above is phase 3
and needs the Rust patch to actually engage.

### Backup plan: direct Metal bypass (no Rust patch required)

If the Rust patch path proves too expensive — wgpu-native API drift,
Cargo-in-Bazel friction, or remote-cache characterization showing
unacceptable CI runtime regression — there's a simpler alternative
that skips wgpu-native entirely for the final render pass into the
IOSurface.

**Key observation.** wgpu-native's C ABI *does* export a way to pull
the underlying `MTLDevice` out of a `WGPUDevice`:

```c
// wgpu-native/ffi/src/wgpu.h (already present upstream)
WGPU_EXPORT void* wgpuDeviceGetMetalDevice(WGPUDevice device);
```

That handle lets C++ talk directly to Metal, no Rust patch needed.
The backend render flow then becomes:

```
  WGPUDevice   ──wgpuDeviceGetMetalDevice──▶   MTLDevice
                                                  │
                                                  ▼  newTextureWithDescriptor:iosurface:plane:
                                             MTLTexture (aliases the host IOSurface)
                                                  │
                                                  ▼  MTLRenderPassDescriptor.colorAttachments[0].texture = …
                                             Direct Metal render pass
                                                  │
                                                  ▼  Geode draws into this MTLTexture
                                             (via its own wgpu-issued draw calls —
                                              they target the wgpu swapchain, and
                                              we manually blit from the swapchain
                                              texture to our IOSurface-backed one
                                              before presenting)
```

**Shape of the work:**

1. **`BridgeTexture_macOS.mm`** — an Objective-C++ translation unit
   (`.mm` so Metal headers are available). Uses
   `wgpuDeviceGetMetalDevice` + `MTLDevice::
   newTextureWithDescriptor:iosurface:plane:` to wrap the IOSurface
   as an `MTLTexture` at `handleAttachSharedTexture` time.

2. **Blit stage inside the backend's render loop.** After Geode's
   normal `wgpuCommandEncoderCopyTextureToTexture` (or equivalent)
   targets its internal swapchain, insert a manual
   `MTLBlitCommandEncoder` copy from the wgpu swapchain's
   `MTLTexture` into our IOSurface-backed `MTLTexture`. This costs
   one GPU blit per frame (~0.5-1 ms on Apple Silicon for a
   1280×720 BGRA8 texture, negligible vs the readback tax we're
   replacing).

3. **No wire-format change.** The session protocol's
   `kAttachSharedTexture` opcode + `BridgeTextureHandle` are
   already in place and carry an `IOSurfaceRef`/`IOSurfaceID` —
   the backend just interprets them via a different code path.

**Tradeoffs vs. the Rust patch:**

| Dimension | Rust patch | Metal bypass |
|---|---|---|
| Sync cost | Zero-copy — wgpu writes directly into IOSurface | One GPU blit/frame (~0.5-1 ms @ 1280×720) |
| Rust knowledge needed | Yes (wgpu-hal, wgpu-core internals) | No (Objective-C++ + Apple docs) |
| Build-system change | Major (rules_rust + crates_repository) | Minor (`.mm` + `-framework Metal`) |
| Forward-compat with wgpu updates | Fragile (internal API) | Stable (uses only public `wgpuDeviceGetMetalDevice`) |
| Applies to Linux / Windows | Yes (same Rust patch shape) | No (Metal-specific; Linux/Windows need separate fallbacks) |
| Effort estimate | ~1 week | ~2-3 days |

**Recommended decision rule.** Start the Rust patch. If after
~3 days of Rust work the patch isn't landing cleanly (either API
surface drift in wgpu or crate-universe reproducibility problems),
pivot to the Metal bypass for macOS alone. The Metal bypass is
self-contained and blocks nothing else; keep it in the toolbox.

For Linux/Windows, the Rust patch path is strictly preferred —
there's no equivalent "just use the native API" shortcut when the
backend is renderer-agnostic (Vulkan on Linux, D3D12 on Windows)
and we're not investing in per-platform Objective-C-style
bindings.

### Alternatives we considered and rejected

- **Shared memory for the bitmap.** Would avoid one memcpy per frame
  on the Linux subprocess path. Rejected because (a) it requires
  unsandbox-ing an shm fd, re-opening the backend→host attack
  surface we just closed; (b) the in-process and WASM paths don't
  benefit (pointer copy of a `std::vector` is already ~memcpy speed
  on those transports); (c) the bottleneck measured on real
  documents is rasterization, not bitmap transport.

- **Shared GL texture via `EGL_EXT_image_dma_buf_import` or a Mach
  surface.** Would move the work from host CPU to backend GPU, but
  re-introduces a graphics driver into the sandbox — exactly the
  thing we're trying to keep out. Would also make the WASM path
  impossible (browsers don't expose cross-worker texture handles).

- **Render on the backend, ship pixels.** A middle ground: backend
  rasterizes into its own `tiny_skia::Pixmap`, ships the RGBA
  buffer instead of the opcode stream. Rejected because it doubles
  raster cost when the host already has the work to do for
  selection chrome, marquee overlay, and the ImGui compositor; and
  because the opcode stream is *smaller* than a 892×512 RGBA
  buffer for most documents (a filled-rect wire message is 30
  bytes; the equivalent bitmap region is 160 KB).

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

Historical DRNR-first API sketch. The current live editor APIs are
`SandboxSession`, `SessionProtocol`, `EditorApiCodec`, `EditorBackendCore`, and
`EditorBackendClient` as described in S7-S9 above. `SandboxHost` still exists as
the one-shot `donner_parser_child` CLI/debug path.

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

Historical DRNR streaming model. The live desktop editor is intended to use a
`SandboxSession` writer thread and reader thread around session frames, and the
backend returns whole `FramePayload`s with bitmap payloads rather than a
per-command `WireMessage` queue for the UI frame.

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

This table reflects the original DRNR transport. The live editor reports most
backend outcomes through `FramePayload.statusKind` / diagnostics on the DRNS
session response; DRNR-specific rows apply to `.rnr` replay and debug tools.

| Class | Detection | Response |
|---|---|---|
| HTTPS fetch failure | curl return code | Address bar chip, no sandbox round-trip. |
| File read failure | `errno` | Address bar chip, no sandbox round-trip. |
| Parse error (valid wire, malformed SVG) | `kParseError` opcode | Address bar chip with parser message, previous document preserved. |
| Parse warnings | `kDiagnostic` opcodes | Logged to editor console pane; non-blocking. |
| Sandbox crash (SIGSEGV, SIGABRT, OOM) | Child exit non-zero, `read()` returns 0 | `SandboxCrashed` raised, address bar chip, child respawned on next request. Crashing corpus entry logged for fuzzer ingestion. |
| Sandbox hang | Host-side 30 s deadline on `kEndFrame` arrival | Host sends SIGKILL, treats as crash. Deadline is generous; real renders should complete in <1 s. |
| Wire format error (host sees an opcode it doesn't recognize, length overrun, invalid variant tag) | Deserializer check | DRNR replay fails without host crash. **The DRNR path is fuzzed; the live DRNS request/frame path still needs fuzzers.** |
| Backend reports an error during replay | `ReplayingRenderer` catches | Logged, frame completes on best-effort basis. Host never crashes on a replay error. |

**Non-negotiable invariant**: *the host process never crashes due to any
input from the sandbox child, including maliciously-crafted wire messages
targeting the host's deserializer*. `sandbox_wire_fuzzer` holds this line for
DRNR. The open completion item is to add equivalent fuzz coverage for
`SessionCodec` / `EditorApiCodec`.

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
- Send IPC to the editor host beyond the session protocol (`DRNS`) and, for
  debug/replay tools, the render-wire protocol (`DRNR`).
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
3. **Sandbox child → host (session frames)**: `SessionCodec` and
   `EditorApiCodec` are the live trust boundary. They bounds-check payload
   lengths and reject malformed enum tags. They currently have unit tests, but
   no fuzzer; add `editor_backend_request_fuzzer` and `frame_response_fuzzer`
   before treating this as fully fuzzed.
4. **Sandbox child → host (DRNR debug/replay)**: `ReplayingRenderer` and
   `SandboxCodecs` are the render-wire trust boundary. This path is fuzzed by
   `sandbox_wire_fuzzer`, but it is no longer the live editor frame transport.
5. **Sandbox child → OS**: Linux applies seccomp-bpf in fail-open `EACCES`
   mode plus resource limits. macOS has no `sandbox_init` profile yet because
   the desktop build still uses the in-process backend. S6.2 is not closed
   until Linux uses kill-on-deny, macOS applies a deny-by-default profile, and
   denied-operation probes cover both platforms.

### Defensive measures

- **Resource caps**:
  - `RLIMIT_AS` (address space) = 1 GB. A parser OOM kills only the child.
  - `RLIMIT_CPU` = 30 seconds. A pathological SVG can't pin a core forever.
  - `RLIMIT_FSIZE` = 0. No file writes, belt-and-braces.
  - `RLIMIT_NOFILE` = 16. Enough for stdin/stdout/stderr and a handful of
    internal FDs; no extra file descriptors possible.
- **DRNR wire message caps** (enforced in `ReplayingRenderer`):
  - Max frame size: 256 MB. Larger frames are treated as a protocol violation.
  - Max path ops per `PathShape`: 10M.
  - Max clip paths per `ResolvedClip`: 1024.
  - Max filter graph depth: 256.
  - Max command count per frame: 10M.
- **No ambient authority**: child inherits no environment variables beyond
  locale + `PATH`, no file descriptors beyond pipes, no working directory
  beyond `/`, no signal handlers, no `LD_PRELOAD` (wiped from environment),
  no privilege.
- **Opaque forwarding**: unknown or malformed session opcodes produce
  `kError` or fail the request; unknown or malformed DRNR opcodes fail replay.
  The live session boundary still needs fuzz coverage.

### Fuzzing plan

Implemented and planned fuzzers:

1. **Implemented: `sandbox_wire_fuzzer`** feeds random bytes into
   `ReplayingRenderer` and asserts the DRNR decoder never crashes.
2. **Planned: `editor_backend_request_fuzzer`** feeds random session payloads
   into backend request decoding and dispatch.
3. **Planned: `frame_response_fuzzer`** feeds random `FramePayload`s into host
   frame decoding and `EditorBackendClient` state updates.
4. **Planned: parser corpus replay through the address-bar/session path** so
   crashes found by parser fuzzing are classified as child crashes, not host
   crashes.

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

1. **DRNR replay never crashes on malformed render wire.** Enforced by
   `//donner/editor/sandbox/tests:sandbox_wire_fuzzer`.
2. **Live `FramePayload` decoding should never crash on malformed session
   payloads.** Not yet enforced by a fuzzer; currently covered only by
   codec unit tests.
3. **Desktop host parser isolation is not yet enforced.** The intended
   invariant is: Linux and macOS editor host binaries never link parser-owning
   code and always select `EditorBackendClient_Session`. That is currently not
   proven by CI and is false on macOS. The P0 closure targets are the
   editor-binary backend smoke tests plus Bazel visibility that prevents host
   targets from depending on backend/parser implementation targets.
4. **Platform jails are not yet enforced.** The intended invariant is: backend
   children on Linux and macOS cannot read arbitrary files, write files, create
   network sockets, spawn subprocesses, or continue if hardening fails. Current
   Linux seccomp is fail-open `EACCES`, and macOS has no profile. The P0 closure
   targets are `sandbox_hardening_linux_kill_tests` and
   `sandbox_hardening_macos_profile_tests`.
5. **Wire format is versioned.** Breaking changes bump the version field;
   a version mismatch between host and child is a fatal error on handshake.

## Testing and Validation

- **Unit**: `session_codec_tests`, `editor_api_codec_tests`, and
  `wire_format_tests` cover frame/DRNR codec round trips and malformed inputs.
- **Integration**: `sandbox_session_tests`, `editor_backend_integration_tests`,
  and `editor_backend_client_tests` cover the long-lived child, backend binary,
  and backend-client implementations on the platforms where those targets are
  enabled. They do not prove the shipped editor binary selects the session
  backend.
- **P0 editor-binary smoke**:
  - Missing: `editor_binary_backend_smoke_test_linux`.
  - Missing: `editor_binary_backend_smoke_test_macos`.
  - Both tests should invoke the real editor binary's `--backend-smoke-test`
    path and assert `transport=session` and `backend_pid != host_pid`.
- **P0 dependency fence**:
  - Missing: Bazel visibility/target-compatibility refactor that prevents
    desktop host targets from linking parser-owning backend implementation
    targets.
  - Missing: CI coverage that fails if `//donner/editor:editor` depends on
    `EditorBackendClient_InProcess`, `EditorBackendCore`, `backend_lib`, or
    `//donner/svg/parser` on Linux or macOS.
- **P0 S6.2 hardening**:
  - Existing: `sandbox_hardening_tests` covers the env gate, resource limits,
    Linux seccomp installation, and normal rendering under the current fail-open
    filter.
  - Missing: `sandbox_hardening_linux_kill_tests` for denied-operation probes
    under `SECCOMP_RET_KILL_PROCESS`.
  - Missing: `sandbox_hardening_macos_profile_tests` for the macOS
    deny-by-default `sandbox_init` profile.
- **Golden/pixel**: `editor_backend_golden_image_tests` compares backend
  `finalBitmapPixels` against direct renderer output for simple documents and
  reproduces the thin-client filter/drag regressions.
- **Record/replay**: `record_replay_tests`, `sandbox_diff_tests`, and the
  `sandbox_replay` / `sandbox_inspect` tools cover the DRNR `.rnr` path.
- **Fuzzing**:
  - Implemented: `sandbox_wire_fuzzer` for DRNR.
  - Missing: request/frame fuzzers for the live `DRNS` / `FramePayload` path.
- **Address bar / fetch**: `address_bar_tests`, `address_bar_dispatcher_tests`,
  `desktop_fetcher_tests`, `resource_policy_tests`, `curl_availability_tests`,
  `svg_source_tests`, and `url_security_tests` cover the implemented fetch and
  policy behavior. There is no local HTTPS fixture-server integration test yet.

## Dependencies

- **curl CLI**: used by `SvgSource` for desktop HTTP(S) fetches. It is not a
  Bazel module dependency; `CurlAvailability` probes for it at runtime and
  surfaces install guidance when missing.
- **rules_rust / patched wgpu-native source build**: present for the texture
  bridge patch. The exported functions are still stubs until the Rust HAL
  extraction is completed.
- **Bazel `banned-patterns`**: prevents non-exempt editor host code from
  calling `SVGParser::ParseSVG` directly.

## Rollout Plan

- **Linux desktop**: must ship the session-backed backend path and the
  editor-binary smoke test that proves the shipped binary selects it.
- **macOS desktop**: must not ship the in-process backend fallback as the
  default. The P0 rollout gate is `SandboxSession` + `donner_editor_backend` +
  `sandbox_init` + editor-binary smoke coverage.
- **WASM**: ships the in-process backend and relies on browser sandboxing.
- **`.rnr` format**: remains a debug artifact tied to the DRNR wire version.

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
- **HTTPS client**: **Resolved for the MVP** — desktop fetch launches
  `curl` through `posix_spawnp` with a fixed argv and curated environment,
  avoiding both a shell and a libcurl runtime dependency. S11 adds the
  missing-curl diagnostic. A libcurl upgrade remains Future Work.
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
