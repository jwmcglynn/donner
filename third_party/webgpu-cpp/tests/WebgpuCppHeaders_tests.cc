/// @file
/// Compile and link the WebGPU C++ declarations without the native implementation archive.

#include <webgpu/webgpu.hpp>

#include <type_traits>

static_assert(std::is_convertible_v<decltype(wgpu::TextureFormat::RGBA8Unorm), WGPUTextureFormat>);
static_assert(std::is_convertible_v<decltype(wgpu::ShaderStage::Compute), WGPUShaderStage>);

int main() {
  return 0;
}
