# Teleport M1

Teleport M1 is the minimum viable framework layer built on top of the M0.1
reflection codec. It proves one synchronous request/response path across a
pipe-connected subprocess:

- `transport.h`: length-prefixed pipe framing with a 64 MiB hard cap.
- `service_runner.h`: synchronous server loop for one request type and one
  response type.
- `client.h`: subprocess client that spawns a child, sends one request frame,
  reads one response frame, and tears the child down on destruction.

What M1 does **not** include yet:

- Multiple services or methods.
- Method IDs or a dispatch table.
- Async calls, multiplexing, or pipelining.
- Version negotiation, schema hashes, or authentication.

## Build and run

Teleport M1 stays behind the same opt-in P2996 toolchain gate as the original
codec spike:

```sh
bazel build --config=teleport_spike //donner/editor/ipc/echo_demo:echo_demo
bazel run --config=teleport_spike //donner/editor/ipc/echo_demo:echo_demo
```

Toolchain setup for Bloomberg's `clang-p2996` fork is unchanged from M0.2; see
[`donner/editor/ipc/spike/SPIKE_NOTES.md`](../spike/SPIKE_NOTES.md).

## Pipe framing

Every message on the pipe is:

1. A 4-byte little-endian `uint32_t` body length.
2. Exactly that many body bytes from `Encode<T>()`.

Framing invariants:

- Frames larger than 64 MiB are rejected with `TransportError::kFrameTooLarge`.
- `PipeReader::readFrame()` returns `TransportError::kEof` only when EOF is
  observed exactly at the next frame boundary.
- EOF after a partial length prefix or partial payload is treated as
  `TransportError::kShortRead`.

## Demo layout

The M1 demo lives in `donner/editor/ipc/echo_demo/`. `echo_demo` resolves its
child service by first checking for a sibling `echo_service` binary next to
`argv[0]`, which works under `bazel run` because Bazel places same-package
executables side-by-side in `bazel-bin`. It then falls back to the runfiles
tree (`RUNFILES_DIR`) before trying `PATH`.

## M2: render demo

`donner/editor/ipc/render_demo/` ships the Teleport M2 proof of life:

```sh
bazel build //donner/svg/tool:donner-svg                 # default toolchain
bazel run --config=teleport_spike \
    //donner/editor/ipc/render_demo:render_demo -- \
    "$PWD/donner_splash.svg" /tmp/teleport_m2_out.png
```

The demo proves the full picture cross-process: a real SVG file is sent
over the Teleport pipe transport to a `render_service` subprocess, which
in turn shells to `donner-svg` to produce a PNG, returns the bytes as a
`RenderResponse`, and the demo writes them to disk. On success it
prints `Teleport M2 proof of life: PASS`, the output size, and the
end-to-end round-trip in milliseconds.

See [`spike/SPIKE_NOTES.md`](../spike/SPIKE_NOTES.md) "M2 status" for the
design rationale (Option D: shell to `donner-svg` to avoid toolchain
mixing) and observed numbers.
