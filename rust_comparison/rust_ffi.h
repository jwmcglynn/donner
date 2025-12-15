#pragma once
/// @file

#include <cstddef>
#include <cstdint>

extern "C" {
uint32_t tiny_skia_rust_reference_width();
uint32_t tiny_skia_rust_reference_height();
size_t tiny_skia_rust_reference_stride();
bool tiny_skia_rust_render_reference(uint8_t* buffer, size_t buffer_len);
}
