#pragma once
/// @file

#if defined(__clang__)
#if __has_warning("-Wnontrivial-memcall")
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wnontrivial-memcall"
#endif
#endif

#include "backends/imgui_impl_glfw.h"
// The WebGPU tier draws its own draw data through the GPU runtime, so it needs no renderer
// backend here; the OpenGL tier still uses the stock one.
#ifndef DONNER_EDITOR_WGPU
#include "backends/imgui_impl_opengl3.h"
#endif
#include "imgui.h"

#if defined(__clang__)
#if __has_warning("-Wnontrivial-memcall")
#pragma clang diagnostic pop
#endif
#endif
