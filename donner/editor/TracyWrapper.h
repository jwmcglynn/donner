#pragma once
/// @file
///
/// Conditional Tracy profiling macros for the editor. When `ENABLE_TRACY` is
/// defined, the real Tracy profiler client is included; otherwise the macros
/// expand to no-ops so editor code can be instrumented unconditionally.
///
/// Tracy is linked into every editor build (release + WASM included), but
/// runtime-gated by a UI Help/Devtools toggle. See the Security section of
/// `docs/design_docs/editor.md` for the trade-off.

#ifdef ENABLE_TRACY
#include "tracy/Tracy.hpp"
#endif

#ifndef ENABLE_TRACY
#define ZoneScoped \
  do {             \
  } while (0, 0)
#define FrameMarkStart(name) \
  do {                       \
  } while (0, 0)
#define FrameMarkEnd(name) \
  do {                     \
  } while (0, 0)
#endif
