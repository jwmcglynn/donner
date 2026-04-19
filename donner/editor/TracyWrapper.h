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
// No-op expansions that compile cleanly under clang-tidy's
// `avoid-do-while-macros` (hence no `do { } while(0)` idiom). Each
// expands to a single void-cast expression; `(name)` references
// suppress unused-argument warnings without doing any work.
#define ZoneScoped static_cast<void>(0)
#define ZoneScopedN(name) static_cast<void>(name)
#define FrameMark static_cast<void>(0)
#define FrameMarkStart(name) static_cast<void>(name)
#define FrameMarkEnd(name) static_cast<void>(name)
#endif
