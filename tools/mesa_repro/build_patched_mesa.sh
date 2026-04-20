#!/usr/bin/env bash
# Build a local Mesa 25.2.8 with debug symbols, installed into
# `${MESA_REPRO_ROOT:-~/Projects/donner-mesa-repro}/mesa-prefix/`
# (kept outside the donner repo so Bazel doesn't stat its ~2 GB tree
# on every build). Used for bisecting and patching the two driver bugs
# that force us to skip Geode tests on Linux CI (see
# docs/design_docs/0031-mesa_vulkan_repro_and_patch.md):
#
#   #542 — Intel Arc ANV cumulative fence-signal race
#   #551 — Mesa llvmpipe Vulkan compute-dispatch heap corruption
#
# Usage:
#     tools/mesa_repro/build_patched_mesa.sh [--ref=<git-ref>] [--asan]
#                                            [--drivers=swrast,intel]
#
# Options:
#   --ref=<ref>       Mesa git ref to check out (tag, branch, or SHA).
#                     Default: mesa-25.2.8 (matches Ubuntu 24.04 noble-updates).
#   --asan            Build with AddressSanitizer for allocation-bug
#                     localization. Incompatible with LD_PRELOAD shims
#                     that don't link libasan themselves.
#   --drivers=<list>  Comma-separated Vulkan drivers. Default: swrast
#                     (lavapipe) + intel (anv). Set to just swrast for the
#                     #551 work to halve build time.
#   --clean           Remove the checkout + build dir before starting.
#   --jobs=<N>        Parallel build jobs. Default: nproc.
#
# Afterwards, use `run_with_patched_mesa.sh <command ...>` to execute
# arbitrary binaries (Bazel test binaries, the lvp_compute_churn repro,
# etc.) against the locally-built Mesa.
#
# Sources Mesa from https://gitlab.freedesktop.org/mesa/mesa.git.
#
# Expected build artifacts:
#   mesa-prefix/lib/libvulkan_lvp.so
#   mesa-prefix/share/vulkan/icd.d/lvp_icd.x86_64.json
#   mesa-prefix/lib/libvulkan_intel.so              (if --drivers includes intel)
#   mesa-prefix/share/vulkan/icd.d/intel_icd.x86_64.json

set -euo pipefail

REF="mesa-25.2.8"
USE_ASAN=0
DRIVERS="swrast,intel"
CLEAN=0
JOBS="$(nproc)"

for arg in "$@"; do
  case "$arg" in
    --ref=*)     REF="${arg#*=}" ;;
    --asan)      USE_ASAN=1 ;;
    --drivers=*) DRIVERS="${arg#*=}" ;;
    --clean)     CLEAN=1 ;;
    --jobs=*)    JOBS="${arg#*=}" ;;
    *)
      echo "usage: $0 [--ref=<git-ref>] [--asan] [--drivers=swrast,intel] [--clean] [--jobs=<N>]" >&2
      exit 2
      ;;
  esac
done

# Anchor to the directory holding this script so log messages look right,
# but keep the Mesa checkout / build / install OUTSIDE the donner repo so
# Bazel doesn't see a big `mesa-work/` tree and try to stat every file on
# every build. Override with `MESA_REPRO_ROOT=...` for a custom location.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MESA_REPRO_ROOT="${MESA_REPRO_ROOT:-${HOME}/Projects/donner-mesa-repro}"
WORK_DIR="${MESA_REPRO_ROOT}/mesa-work"
SRC_DIR="${WORK_DIR}/src"
BUILD_DIR="${WORK_DIR}/build"
PREFIX_DIR="${MESA_REPRO_ROOT}/mesa-prefix"

if [[ "$CLEAN" == "1" ]]; then
  echo "==> Cleaning ${WORK_DIR} and ${PREFIX_DIR}"
  rm -rf "${WORK_DIR}" "${PREFIX_DIR}"
fi

mkdir -p "${WORK_DIR}"

# ---- Step 1: fetch ----
if [[ ! -d "${SRC_DIR}/.git" ]]; then
  echo "==> Cloning Mesa into ${SRC_DIR}"
  git clone --depth=1 --branch="${REF}" \
    https://gitlab.freedesktop.org/mesa/mesa.git "${SRC_DIR}" || {
    echo "==> Shallow clone of '${REF}' failed; falling back to full clone"
    rm -rf "${SRC_DIR}"
    git clone https://gitlab.freedesktop.org/mesa/mesa.git "${SRC_DIR}"
    git -C "${SRC_DIR}" checkout "${REF}"
  }
else
  echo "==> Reusing existing checkout at ${SRC_DIR} (ref: $(git -C "${SRC_DIR}" rev-parse --short HEAD))"
fi

# Apply patches, if any, from patches/*.patch in sorted order. Patches are
# expected to apply cleanly against the current checkout; if not, the
# script fails fast so the maintainer can resolve the conflict before
# investigating driver-level behaviour.
if compgen -G "${SCRIPT_DIR}/patches/*.patch" > /dev/null; then
  echo "==> Applying patches from ${SCRIPT_DIR}/patches/"
  for p in "${SCRIPT_DIR}"/patches/*.patch; do
    echo "    - $(basename "$p")"
    (cd "${SRC_DIR}" && git apply --check "$p")
    (cd "${SRC_DIR}" && git apply "$p")
  done
else
  echo "==> No patches found at ${SCRIPT_DIR}/patches/ — building stock ${REF}"
fi

# ---- Step 2: configure ----
#
# `-Dvulkan-drivers=${DRIVERS}` builds the requested Vulkan ICDs.
# `-Dgallium-drivers=` disables the OpenGL / gallium drivers (we don't
# need them for WebGPU). Mesa's build system still requires the
# `swrast` gallium driver for some lavapipe internals, so we enable
# that conditionally if swrast is in the Vulkan driver list.
GALLIUM_DRIVERS=""
if [[ "$DRIVERS" == *"swrast"* ]]; then
  GALLIUM_DRIVERS="swrast"
fi

MESON_ARGS=(
  "-Dbuildtype=debug"
  "-Dprefix=${PREFIX_DIR}"
  "-Dvulkan-drivers=${DRIVERS}"
  "-Dgallium-drivers=${GALLIUM_DRIVERS}"
  "-Dplatforms=x11,wayland"
  "-Dglx=disabled"
  "-Dvideo-codecs="
  "-Dvulkan-layers="
)

if [[ "$USE_ASAN" == "1" ]]; then
  echo "==> Enabling AddressSanitizer"
  MESON_ARGS+=("-Db_sanitize=address")
fi

if [[ ! -d "${BUILD_DIR}" ]]; then
  echo "==> Configuring Mesa build (meson)"
  (cd "${SRC_DIR}" && meson setup "${BUILD_DIR}" "${MESON_ARGS[@]}")
else
  echo "==> Reusing existing build dir at ${BUILD_DIR} (reconfiguring)"
  (cd "${SRC_DIR}" && meson setup --reconfigure "${BUILD_DIR}" "${MESON_ARGS[@]}")
fi

# ---- Step 3: build + install ----
echo "==> Compiling (jobs=${JOBS})"
meson compile -C "${BUILD_DIR}" -j "${JOBS}"
echo "==> Installing into ${PREFIX_DIR}"
meson install -C "${BUILD_DIR}"

# ---- Step 4: verify ----
LVP_ICD="$(ls "${PREFIX_DIR}"/share/vulkan/icd.d/lvp_icd.*.json 2>/dev/null || true)"
if [[ -z "${LVP_ICD}" ]]; then
  echo "!! Expected llvmpipe ICD JSON not found in ${PREFIX_DIR}/share/vulkan/icd.d/" >&2
  exit 1
fi

echo
echo "==> Mesa patched build installed."
echo "    Prefix:      ${PREFIX_DIR}"
echo "    ICD (lvp):   ${LVP_ICD}"
if ls "${PREFIX_DIR}"/share/vulkan/icd.d/intel_icd.*.json >/dev/null 2>&1; then
  echo "    ICD (intel): $(ls "${PREFIX_DIR}"/share/vulkan/icd.d/intel_icd.*.json)"
fi
echo
echo "    To use:  tools/mesa_repro/run_with_patched_mesa.sh <command ...>"
