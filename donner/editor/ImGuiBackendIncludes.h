#pragma once
/// @file

#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnontrivial-memcall"
#endif

#include "backends/imgui_impl_glfw.h"
#ifdef DONNER_EDITOR_WGPU
#include "backends/imgui_impl_wgpu.h"
#else
#include "backends/imgui_impl_opengl3.h"
#endif
#include "imgui.h"

#if defined(__clang__)
#pragma clang diagnostic pop
#endif
