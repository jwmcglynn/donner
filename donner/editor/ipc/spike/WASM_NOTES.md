# Teleport IPC — M0.3 Wasm toolchain investigation

**Status: BLOCKED on heavy path.** The Bloomberg `clang-p2996` fork image
that we use for every prior milestone (M0.1, M0.2, M1, M2) ships a clang
built with **only AArch64 (or X86 on the amd64 tag) registered as an LLVM
target.** There is no `wasm32` codegen in either image, and no wasm
runtime libraries (`libc++`, `libc++abi`, `compiler-rt.builtins`) for
`wasm32-unknown-emscripten`. Per the M0.3 decision rule
(`A=no → heavy path → stop and document`), we did not start the
4–8 hour from-source LLVM build.

This file records the verbatim Phase 1 evidence, the recommended path
forward, and the exact commands the next attempt should run so it does
not have to re-discover the same dead ends.

Host context for this run:

* Bare-metal aarch64 Linux fork runner (Ubuntu, kernel ≥ 6.x).
* 128 logical CPUs, 102 GiB RAM, 1.4 TiB free disk on `/`.
* Docker 29.4.1 (rootful, `--network=host` required — see paper cut #1).
* `node` v22.22.2 already on `PATH`. **No `emcc` / `emsdk` installed.**
* Both fork images are pre-pulled:
  * `vsavkov/clang-p2996:arm64` (digest `6119bda9ce78`, native).
  * `vsavkov/clang-p2996:amd64` (digest `6672c4227b09`, **cannot run** —
    binfmt_misc has no `qemu-x86_64` entry on this host, so the image's
    init binary fails with `exec format error`).
* Both fork builds pin Bloomberg commit
  `f72d85e5a0fd9c960b07b6f1c8875110701cb627` (LLVM `clang version
  21.0.0git`).

The investigation therefore ran inside `vsavkov/clang-p2996:arm64` (the
image that actually executes here and that M1/M2 also use).

---

## Phase 1 investigation — verbatim answers

### A. Does `vsavkov/clang-p2996:arm64` ship a clang with `WebAssembly` target compiled in?

**Answer: NO.** The fork's clang is configured with
`LLVM_TARGETS_TO_BUILD=AArch64` only.

Reproduction:

```sh
docker run --rm --network=host vsavkov/clang-p2996:arm64 \
  /opt/p2996/clang/bin/clang --print-targets
```

Output (verbatim):

```
  Registered Targets:
    aarch64    - AArch64 (little endian)
    aarch64_32 - AArch64 (little endian ILP32)
    aarch64_be - AArch64 (big endian)
    arm64      - ARM64 (little endian)
    arm64_32   - ARM64 (little endian ILP32)
```

Note: the brief's suggested probe was
`/opt/p2996/clang/bin/llc --version | grep -A100 'Registered Targets'`,
but the fork image does **not** ship a standalone `llc` binary. Only the
following LLVM tools are present in `/opt/p2996/clang/bin/`:

```
clang, clang++, clang-21, clang-cpp, clang-format, clang-tidy,
clang-repl, clang-scan-deps, clangd, clang-check, clang-extdef-mapping,
clang-installapi, clang-linker-wrapper, clang-nvlink-wrapper,
clang-offload-bundler, clang-offload-packager, clang-refactor,
clang-sycl-linker, diagtool, git-clang-format, hmaptool,
intercept-build, llvm-ar, llvm-cov, llvm-cxxfilt, llvm-dlltool,
llvm-dwp, llvm-lib, llvm-mca, ...
```

`clang --print-targets` is the equivalent — it enumerates the same
"Registered Targets" list that `llc --version` would have produced. There
is no `wasm32` and no `wasm64` line.

`grep -E 'wasm|emscripten'` against
`/opt/p2996/clang/lib/clang/21/include/` returns exactly one file —
`wasm_simd128.h` — which is a header that ships with every clang regardless
of whether the `WebAssembly` backend is enabled. It is **not** evidence
that wasm codegen exists; consuming code is rejected at codegen time
because the backend itself isn't linked in.

The amd64 fork image (`vsavkov/clang-p2996:amd64`) is built with the same
configuration (X86 only, no wasm) per the upstream Dockerfile in the
Bloomberg repo. It cannot be exercised on this host because x86_64
emulation is not registered (`/proc/sys/fs/binfmt_misc/qemu-x86_64`
absent), so this run cannot independently confirm — but the same
SPIKE_NOTES history (M0.1 used the amd64 image and never observed wasm
support) is consistent.

### B. Does the image ship libc++ / libc++abi / compiler-rt built for `wasm32-unknown-emscripten` (or `wasm32-unknown-unknown`)?

**Answer: NO.** Only AArch64 runtimes are present.

Reproduction:

```sh
docker run --rm --network=host vsavkov/clang-p2996:arm64 bash -c '
  ls /opt/p2996/clang/lib/ | grep -Ei "wasm|emscripten" || echo "(none)"
  echo "---"
  ls /opt/p2996/clang/lib/
  echo "---"
  find /opt/p2996/clang/lib -name "libc++.a"
  echo "---"
  find /opt/p2996/clang/lib -name "libclang_rt.*.a"
  echo "---"
  find /opt/p2996 -iname "*wasm*"
'
```

Output (verbatim):

```
(none)
---
aarch64-unknown-linux-gnu
clang
cmake
libLTO.so
libLTO.so.21.0git
libRemarks.so
libRemarks.so.21.0git
libclang.so
libclang.so.21.0.0git
libclang.so.21.0git
libear
libscanbuild
---
/opt/p2996/clang/lib/aarch64-unknown-linux-gnu/libc++.a
---
/opt/p2996/clang/lib/clang/21/lib/aarch64-unknown-linux-gnu/libclang_rt.builtins.a
---
/opt/p2996/clang/lib/clang/21/include/wasm_simd128.h
```

The only wasm-related artifact in the entire fork install is the SIMD128
intrinsic *header*. There is no `lib/wasm32-unknown-emscripten/`, no
`lib/clang/21/lib/wasm32-*`, no `libc++` static archive for wasm, no
`libclang_rt.builtins-wasm32.a`. Even if A had been "yes," B alone
would have demoted us to the medium path; with A=no the medium path is
not reachable either (we can't build runtimes for a target the host
clang can't codegen for).

### C. From-source build path

Since A=no, this section documents the recommended invocation but does
**not** execute it (per the decision rule).

Source:

```sh
git clone https://github.com/bloomberg/clang-p2996.git
cd clang-p2996
git checkout f72d85e5a0fd9c960b07b6f1c8875110701cb627
```

Configure (drops X86 since this is an aarch64 host; we only need the host
target plus WebAssembly):

```sh
cmake -S llvm -B build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DLLVM_TARGETS_TO_BUILD="AArch64;WebAssembly" \
  -DLLVM_ENABLE_PROJECTS="clang;lld" \
  -DLLVM_ENABLE_RUNTIMES="libcxx;libcxxabi;compiler-rt" \
  -DLLVM_ENABLE_LLD=ON \
  -DCMAKE_INSTALL_PREFIX=/opt/p2996-wasm \
  -DLLVM_RUNTIME_TARGETS="aarch64-unknown-linux-gnu;wasm32-unknown-emscripten"

ninja -C build -j$(nproc)
ninja -C build install
```

Then point Emscripten at the result:

```sh
git clone https://github.com/emscripten-core/emsdk /opt/emsdk
/opt/emsdk/emsdk install latest
/opt/emsdk/emsdk activate latest
source /opt/emsdk/emsdk_env.sh
export EM_LLVM_ROOT=/opt/p2996-wasm/bin
```

Build-time estimate: the brief's "4–8 hours on a reasonable machine"
applies to typical desktop hardware (8–16 cores). On *this* host
(128 cores, 102 GiB RAM, fast NVMe) a comparable upstream
`llvm+clang+lld+libcxx+libcxxabi+compiler-rt` two-stage build typically
finishes in **45–90 minutes** wall time, with the bulk being clang
codegen for the runtime targets (libcxx for wasm32 in particular
re-builds clang's frontend pieces multiple times via the runtimes
sub-build). The `ninja -t commands | head` dry-run probe required by the
brief was **not** executed because the brief's decision rule
(`A=no → stop`) blocks Phase 2 entirely; running the dry-run would
require checking out and configuring the LLVM tree, which is itself a
multi-GB clone + multi-minute cmake step that we are deferring.

---

## Decision

Per the brief's table:

| Condition | Path | Action |
|---|---|---|
| A=yes ∧ B=yes | easy | install emsdk, set `EM_LLVM_ROOT`, build the spike |
| A=yes ∧ B=no | medium | build runtimes only (~1 h), proceed if ≤ 90 min |
| A=no | heavy | **stop, document, do not start the build without check-in** |

We are in the **A=no** row. **Stopped per the rule.** No LLVM source has
been cloned, no build has been kicked off, no `emsdk` has been installed.

The wasm spike skeleton (`donner/editor/ipc/spike/wasm/`) is **not**
created in this commit because per the brief it is a Phase 2 deliverable
contingent on the toolchain existing. Creating empty / unbuildable
scaffolding would be lower-signal than this notes file.

---

## Recommended next attempt

The next person to pick this up should:

1. **Decide whether to bake wasm into the fork image.** The cleanest fix
   is to send a PR to `bloomberg/clang-p2996`'s Docker recipe adding
   `WebAssembly` to `LLVM_TARGETS_TO_BUILD` and a runtimes sub-build for
   `wasm32-unknown-emscripten`. Once a new image tag exists,
   `donner/editor/ipc/spike/SPIKE_NOTES.md` and
   `build_defs/teleport_p2996_toolchain/extensions.bzl` get bumped to it
   and M0.3 reduces to "write the spike, run under node." This is the
   right strategic move because every future Teleport milestone that
   needs wasm (debugger doc 0033, perf analyzer doc 0035) will benefit.

2. **Or, if a one-off local build is preferred,** execute Phase 1C above
   on a host with ≥ 16 cores and ≥ 64 GiB RAM. Budget 90 min on this
   class of hardware, 4–8 h on a laptop. Then resume Phase 2 from the
   brief's step list verbatim.

3. **Either way**, the spike source itself should be a
   ~50-line `wasm_spike.cc` that `#include`s the existing
   `donner/editor/ipc/spike/teleport_codec.h` directly (no copy). The
   codec is already wasm-clean: it depends only on `<bit>`, `<cstdint>`,
   `<cstring>`, `<expected>`, `<experimental/meta>`, `<string>`,
   `<type_traits>`, `<vector>`. Everything except `<experimental/meta>`
   is in libc++ proper; `<experimental/meta>` is a clang-p2996 fork-shipped
   header and will Just Work as long as `EM_LLVM_ROOT` points at the
   wasm-capable fork build (i.e. its `lib/clang/21/include/` is on the
   resource-dir path). No structural changes to the codec are expected.

4. The `Makefile` should mirror
   `donner/editor/ipc/spike/Makefile` but swap `clang++` for
   `${EMSCRIPTEN_ROOT}/emcc` and add `-sENVIRONMENT=node
   -sNODERAWFS=0 -sEXIT_RUNTIME=1` plus
   `-std=c++26 -freflection-latest -fexpansion-statements`. The `run`
   target should be `node wasm_spike.js`.

---

## Paper cuts encountered (not blockers, but worth recording)

1. **`docker run` requires `--network=host` on this host.** Default
   networking fails with
   `OCI runtime create failed: ... open sysctl
   net.ipv4.ip_unprivileged_port_start file: reopen fd 8: permission
   denied`. `--network=host` (or the equivalent `--sysctl
   net.ipv4.ip_unprivileged_port_start=0` if the namespace allowed it)
   sidesteps it. SPIKE_NOTES should pick this up — M1/M2's docker
   invocations would hit the same wall on a fresh runner.
2. **The fork image lacks a standalone `llc` binary.** The brief's
   probe (`llc --version | grep 'Registered Targets'`) returned empty
   simply because `llc` is not installed. `clang --print-targets` is the
   equivalent and works.
3. **The amd64 fork image cannot run on this aarch64 host.** No
   `binfmt_misc` registration for `qemu-x86_64`. `qemu-user-static` would
   fix this but requires root install + a `--privileged` register step.
   Not relevant to M0.3 (the answer to A is the same regardless of
   architecture for this commit of the fork) but worth noting.

---

## Verification of "no regression"

This change is **documentation-only** plus appending to
`SPIKE_NOTES.md`. No `BUILD.bazel` files, no `bazel_dep`s, no source
under `donner/` C++ tree are touched. Per AGENTS.md / CLAUDE.md,
doc-only changes skip the formatter and the `bazel test //...` gate is
not affected. The branch tip after this commit must still build and
test exactly as `253d2752` did.
