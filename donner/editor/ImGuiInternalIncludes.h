#pragma once
/// @file

#if defined(__clang__)
#if __has_warning("-Wnontrivial-memcall")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnontrivial-memcall"
#endif
#endif

#include "imgui.h"
#include "imgui_internal.h"

#if defined(__clang__)
#if __has_warning("-Wnontrivial-memcall")
#pragma clang diagnostic pop
#endif
#endif
