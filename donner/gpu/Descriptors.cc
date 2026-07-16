#include "donner/gpu/Descriptors.h"

#include <initializer_list>
#include <string_view>

namespace donner::gpu {

namespace {

/// One (flag, name) pair for bitmask serialization.
struct FlagName {
  uint32_t flag;          //!< Flag bit value.
  std::string_view name;  //!< Flag name.
};

/// Outputs set flags joined by `|`, or `None` if no flags are set. Flags print in ascending bit
/// order so output is deterministic.
std::ostream& OutputFlags(std::ostream& os, uint32_t value,
                          std::initializer_list<FlagName> flagNames) {
  if (value == 0) {
    return os << "None";
  }

  bool first = true;
  for (const FlagName& flagName : flagNames) {
    if ((value & flagName.flag) == flagName.flag) {
      if (!first) {
        os << "|";
      }
      os << flagName.name;
      first = false;
    }
  }
  return os;
}

}  // namespace

std::ostream& operator<<(std::ostream& os, TextureFormat value) {
  switch (value) {
    case TextureFormat::RGBA8Unorm: return os << "RGBA8Unorm";
    case TextureFormat::BGRA8Unorm: return os << "BGRA8Unorm";
    case TextureFormat::R8Unorm: return os << "R8Unorm";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, TextureUsage value) {
  return OutputFlags(os, static_cast<uint32_t>(value),
                     {{static_cast<uint32_t>(TextureUsage::RenderAttachment), "RenderAttachment"},
                      {static_cast<uint32_t>(TextureUsage::Sampled), "Sampled"},
                      {static_cast<uint32_t>(TextureUsage::CopySrc), "CopySrc"},
                      {static_cast<uint32_t>(TextureUsage::CopyDst), "CopyDst"}});
}

std::ostream& operator<<(std::ostream& os, BufferUsage value) {
  return OutputFlags(os, static_cast<uint32_t>(value),
                     {{static_cast<uint32_t>(BufferUsage::Vertex), "Vertex"},
                      {static_cast<uint32_t>(BufferUsage::Index), "Index"},
                      {static_cast<uint32_t>(BufferUsage::Uniform), "Uniform"},
                      {static_cast<uint32_t>(BufferUsage::Storage), "Storage"},
                      {static_cast<uint32_t>(BufferUsage::CopySrc), "CopySrc"},
                      {static_cast<uint32_t>(BufferUsage::CopyDst), "CopyDst"},
                      {static_cast<uint32_t>(BufferUsage::MapRead), "MapRead"}});
}

std::ostream& operator<<(std::ostream& os, ShaderStage value) {
  return OutputFlags(os, static_cast<uint32_t>(value),
                     {{static_cast<uint32_t>(ShaderStage::Vertex), "Vertex"},
                      {static_cast<uint32_t>(ShaderStage::Fragment), "Fragment"},
                      {static_cast<uint32_t>(ShaderStage::Compute), "Compute"}});
}

std::ostream& operator<<(std::ostream& os, ColorWriteMask value) {
  return OutputFlags(os, static_cast<uint32_t>(value),
                     {{static_cast<uint32_t>(ColorWriteMask::Red), "Red"},
                      {static_cast<uint32_t>(ColorWriteMask::Green), "Green"},
                      {static_cast<uint32_t>(ColorWriteMask::Blue), "Blue"},
                      {static_cast<uint32_t>(ColorWriteMask::Alpha), "Alpha"}});
}

std::ostream& operator<<(std::ostream& os, FilterMode value) {
  switch (value) {
    case FilterMode::Nearest: return os << "Nearest";
    case FilterMode::Linear: return os << "Linear";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, AddressMode value) {
  switch (value) {
    case AddressMode::ClampToEdge: return os << "ClampToEdge";
    case AddressMode::Repeat: return os << "Repeat";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, VertexFormat value) {
  switch (value) {
    case VertexFormat::Float32x2: return os << "Float32x2";
    case VertexFormat::Float32x4: return os << "Float32x4";
    case VertexFormat::Uint32: return os << "Uint32";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, VertexStepMode value) {
  switch (value) {
    case VertexStepMode::Vertex: return os << "Vertex";
    case VertexStepMode::Instance: return os << "Instance";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, PrimitiveTopology value) {
  switch (value) {
    case PrimitiveTopology::TriangleList: return os << "TriangleList";
    case PrimitiveTopology::TriangleStrip: return os << "TriangleStrip";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, CullMode value) {
  switch (value) {
    case CullMode::None: return os << "None";
    case CullMode::Back: return os << "Back";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, BlendFactor value) {
  switch (value) {
    case BlendFactor::Zero: return os << "Zero";
    case BlendFactor::One: return os << "One";
    case BlendFactor::SrcAlpha: return os << "SrcAlpha";
    case BlendFactor::OneMinusSrcAlpha: return os << "OneMinusSrcAlpha";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, BlendOperation value) {
  switch (value) {
    case BlendOperation::Add: return os << "Add";
    case BlendOperation::Max: return os << "Max";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, BindingType value) {
  switch (value) {
    case BindingType::UniformBuffer: return os << "UniformBuffer";
    case BindingType::ReadOnlyStorageBuffer: return os << "ReadOnlyStorageBuffer";
    case BindingType::SampledTexture2dFloat: return os << "SampledTexture2dFloat";
    case BindingType::FilteringSampler: return os << "FilteringSampler";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, LoadOp value) {
  switch (value) {
    case LoadOp::Clear: return os << "Clear";
    case LoadOp::Load: return os << "Load";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, StoreOp value) {
  switch (value) {
    case StoreOp::Store: return os << "Store";
    case StoreOp::Discard: return os << "Discard";
  }
  return os << "Unknown";
}

std::ostream& operator<<(std::ostream& os, ShaderSourceKind value) {
  switch (value) {
    case ShaderSourceKind::Wgsl: return os << "Wgsl";
    case ShaderSourceKind::Msl: return os << "Msl";
  }
  return os << "Unknown";
}

uint32_t TextureFormatBytesPerTexel(TextureFormat format) {
  switch (format) {
    case TextureFormat::RGBA8Unorm:
    case TextureFormat::BGRA8Unorm: return 4;
    case TextureFormat::R8Unorm: return 1;
  }
  return 0;
}

uint32_t VertexFormatByteSize(VertexFormat format) {
  switch (format) {
    case VertexFormat::Float32x2: return 8;
    case VertexFormat::Float32x4: return 16;
    case VertexFormat::Uint32: return 4;
  }
  return 0;
}

}  // namespace donner::gpu
