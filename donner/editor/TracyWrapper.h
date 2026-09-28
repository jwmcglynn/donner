#pragma once
/// @file
///
/// Conditional Tracy profiling macros for the editor. When `ENABLE_TRACY` is
/// defined, the real Tracy profiler client is included; otherwise the macros
/// expand to no-ops so editor code can be instrumented unconditionally.
///
/// Native editor builds enable Tracy by default; RE and WASM builds compile
/// this wrapper as no-ops.

#ifdef ENABLE_TRACY
#include "tracy/Tracy.hpp"
#endif

#ifndef ENABLE_TRACY
// No-op expansions that compile cleanly under clang-tidy's
// `avoid-do-while-macros` (hence no `do { } while(0)` idiom). Each
// expands to a single void-cast expression; `(name)` references
// suppress unused-argument warnings without doing any work.
/// No-op scoped profiling zone when Tracy is disabled.
#define ZoneScoped static_cast<void>(0)
/// No-op named profiling zone when Tracy is disabled.
/// @param name Profiling label.
#define ZoneScopedN(name) static_cast<void>(name)
/// No-op frame boundary when Tracy is disabled.
#define FrameMark static_cast<void>(0)
/// No-op named frame start when Tracy is disabled.
/// @param name Profiling label.
#define FrameMarkStart(name) static_cast<void>(name)
/// No-op named frame end when Tracy is disabled.
/// @param name Profiling label.
#define FrameMarkEnd(name) static_cast<void>(name)
#endif
