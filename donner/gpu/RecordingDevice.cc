#include "donner/gpu/RecordingDevice.h"

#include <format>
#include <locale>
#include <sstream>
#include <string_view>

namespace donner::gpu {

namespace {

/// Creates a serialization stream imbued with the classic "C" locale, so captured numbers are
/// byte-identical regardless of the process's global locale (no digit grouping).
std::ostringstream MakeLineStream() {
  std::ostringstream os;
  os.imbue(std::locale::classic());
  return os;
}

/// Formats a double deterministically: std::format's shortest round-trip representation is
/// locale-independent, unlike printf-family formatting which follows LC_NUMERIC.
std::string FormatDouble(double value) {
  return std::format("{}", value);
}

/// Quotes and escapes a label: `"`, `\`, and bytes outside the printable ASCII range are escaped
/// so labels cannot break the line-based format. The explicit range test avoids the
/// locale-dependent std::isprint.
std::string QuoteLabel(std::string_view label) {
  std::string result = "\"";
  for (const char ch : label) {
    const unsigned char byte = static_cast<unsigned char>(ch);
    if (ch == '"' || ch == '\\') {
      result += '\\';
      result += ch;
    } else if (byte >= 0x20 && byte < 0x7F) {
      result += ch;
    } else {
      result += std::format("\\x{:02x}", byte);
    }
  }
  result += '"';
  return result;
}

/// FNV-1a 64-bit hash of a byte payload, formatted as 16 hex digits. Content-sensitive but
/// compact, so recordings stay deterministic without dumping payload bytes.
std::string HashBytes(std::span<const uint8_t> data) {
  uint64_t hash = 14695981039346656037ull;
  for (const uint8_t byte : data) {
    hash ^= byte;
    hash *= 1099511628211ull;
  }
  return std::format("{:016x}", hash);
}

/// Formats a slot-based resource identifier, e.g. `buffer#0`.
std::string RefId(std::string_view resourceName, uint32_t slotIndex) {
  return std::format("{}#{}", resourceName, slotIndex);
}

/// Serializes the vertex buffer layout list of a pipeline descriptor.
void AppendVertexBufferLayouts(std::ostringstream& os,
                               const std::vector<VertexBufferLayout>& buffers) {
  os << "buffers=[";
  for (size_t i = 0; i < buffers.size(); ++i) {
    if (i > 0) {
      os << " ";
    }
    const VertexBufferLayout& layout = buffers[i];
    os << "{strideBytes=" << layout.strideBytes << " stepMode=" << layout.stepMode
       << " attributes=[";
    for (size_t j = 0; j < layout.attributes.size(); ++j) {
      if (j > 0) {
        os << " ";
      }
      const VertexAttribute& attribute = layout.attributes[j];
      os << "{format=" << attribute.format << " offsetBytes=" << attribute.offsetBytes
         << " shaderLocation=" << attribute.shaderLocation << "}";
    }
    os << "]}";
  }
  os << "]";
}

/// Serializes a blend component, e.g. `{srcFactor=One dstFactor=OneMinusSrcAlpha operation=Add}`.
void AppendBlendComponent(std::ostringstream& os, const BlendComponent& component) {
  os << "{srcFactor=" << component.srcFactor << " dstFactor=" << component.dstFactor
     << " operation=" << component.operation << "}";
}

/// Serializes one recorded command as a single line (no indentation or newline).
struct CommandSerializer {
  std::ostringstream& os;  //!< Destination stream.

  void operator()(const BeginRenderPassCommand& command) {
    os << "beginRenderPass label=" << QuoteLabel(command.descriptor.label) << " colorAttachments=[";
    const std::vector<RenderPassColorAttachment>& attachments = command.descriptor.colorAttachments;
    for (size_t i = 0; i < attachments.size(); ++i) {
      if (i > 0) {
        os << " ";
      }
      const RenderPassColorAttachment& attachment = attachments[i];
      os << "{view=" << RefId(TextureViewTag::kName, attachment.view.slotIndex())
         << " loadOp=" << attachment.loadOp << " storeOp=" << attachment.storeOp << " clearColor=("
         << FormatDouble(attachment.clearColor[0]) << " " << FormatDouble(attachment.clearColor[1])
         << " " << FormatDouble(attachment.clearColor[2]) << " "
         << FormatDouble(attachment.clearColor[3]) << ")}";
    }
    os << "]";
  }
  void operator()(const SetPipelineCommand& command) {
    os << "setPipeline " << RefId(RenderPipelineTag::kName, command.pipelineId.slotIndex);
  }
  void operator()(const SetBindGroupCommand& command) {
    os << "setBindGroup index=" << command.index
       << " bindGroup=" << RefId(BindGroupTag::kName, command.bindGroupId.slotIndex);
  }
  void operator()(const SetVertexBufferCommand& command) {
    os << "setVertexBuffer slot=" << command.slot
       << " buffer=" << RefId(BufferTag::kName, command.bufferId.slotIndex)
       << " offsetBytes=" << command.offsetBytes;
  }
  void operator()(const SetScissorRectCommand& command) {
    os << "setScissorRect x=" << command.x << " y=" << command.y << " width=" << command.width
       << " height=" << command.height;
  }
  void operator()(const SetViewportCommand& command) {
    os << "setViewport x=" << FormatDouble(command.x) << " y=" << FormatDouble(command.y)
       << " width=" << FormatDouble(command.width) << " height=" << FormatDouble(command.height)
       << " minDepth=" << FormatDouble(command.minDepth)
       << " maxDepth=" << FormatDouble(command.maxDepth);
  }
  void operator()(const DrawCommand& command) {
    os << "draw vertexCount=" << command.vertexCount << " instanceCount=" << command.instanceCount
       << " firstVertex=" << command.firstVertex << " firstInstance=" << command.firstInstance;
  }
  void operator()(const EndRenderPassCommand&) { os << "endRenderPass"; }
  void operator()(const CopyTextureToBufferCommand& command) {
    os << "copyTextureToBuffer texture=" << RefId(TextureTag::kName, command.textureId.slotIndex)
       << " buffer=" << RefId(BufferTag::kName, command.bufferId.slotIndex)
       << " offsetBytes=" << command.layout.offsetBytes
       << " bytesPerRow=" << command.layout.bytesPerRow
       << " rowsPerImage=" << command.layout.rowsPerImage << " copySize=" << command.copySize;
  }
};

}  // namespace

std::string RecordingDevice::serialize() const {
  std::string result;
  for (const std::string& line : lines_) {
    result += line;
    result += '\n';
  }
  return result;
}

Status RecordingDevice::onCreateBuffer(uint32_t slotIndex, const BufferDescriptor& descriptor) {
  std::ostringstream os = MakeLineStream();
  os << "createBuffer " << RefId(BufferTag::kName, slotIndex)
     << " label=" << QuoteLabel(descriptor.label) << " byteSize=" << descriptor.byteSize
     << " usage=" << descriptor.usage;
  lines_.push_back(os.str());
  return OkStatus();
}

Status RecordingDevice::onCreateTexture(uint32_t slotIndex, const TextureDescriptor& descriptor) {
  std::ostringstream os = MakeLineStream();
  os << "createTexture " << RefId(TextureTag::kName, slotIndex)
     << " label=" << QuoteLabel(descriptor.label) << " size=" << descriptor.size
     << " format=" << descriptor.format << " usage=" << descriptor.usage
     << " sampleCount=" << descriptor.sampleCount;
  lines_.push_back(os.str());
  return OkStatus();
}

Status RecordingDevice::onCreateTextureView(uint32_t slotIndex, uint32_t textureSlotIndex,
                                            const TextureViewDescriptor& descriptor) {
  std::ostringstream os = MakeLineStream();
  os << "createTextureView " << RefId(TextureViewTag::kName, slotIndex)
     << " label=" << QuoteLabel(descriptor.label)
     << " texture=" << RefId(TextureTag::kName, textureSlotIndex);
  lines_.push_back(os.str());
  return OkStatus();
}

Status RecordingDevice::onCreateSampler(uint32_t slotIndex, const SamplerDescriptor& descriptor) {
  std::ostringstream os = MakeLineStream();
  os << "createSampler " << RefId(SamplerTag::kName, slotIndex)
     << " label=" << QuoteLabel(descriptor.label) << " magFilter=" << descriptor.magFilter
     << " minFilter=" << descriptor.minFilter << " addressModeU=" << descriptor.addressModeU
     << " addressModeV=" << descriptor.addressModeV;
  lines_.push_back(os.str());
  return OkStatus();
}

Status RecordingDevice::onCreateBindGroupLayout(uint32_t slotIndex,
                                                const BindGroupLayoutDescriptor& descriptor) {
  std::ostringstream os = MakeLineStream();
  os << "createBindGroupLayout " << RefId(BindGroupLayoutTag::kName, slotIndex)
     << " label=" << QuoteLabel(descriptor.label) << " entries=[";
  for (size_t i = 0; i < descriptor.entries.size(); ++i) {
    if (i > 0) {
      os << " ";
    }
    const BindGroupLayoutEntry& entry = descriptor.entries[i];
    os << "{binding=" << entry.binding << " visibility=" << entry.visibility
       << " type=" << entry.type << "}";
  }
  os << "]";
  lines_.push_back(os.str());
  return OkStatus();
}

Status RecordingDevice::onCreateBindGroup(uint32_t slotIndex,
                                          const BindGroupDescriptor& descriptor) {
  std::ostringstream os = MakeLineStream();
  os << "createBindGroup " << RefId(BindGroupTag::kName, slotIndex)
     << " label=" << QuoteLabel(descriptor.label)
     << " layout=" << RefId(BindGroupLayoutTag::kName, descriptor.layout.slotIndex())
     << " entries=[";
  for (size_t i = 0; i < descriptor.entries.size(); ++i) {
    if (i > 0) {
      os << " ";
    }
    const BindGroupEntry& entry = descriptor.entries[i];
    os << "{binding=" << entry.binding << " ";
    if (const BufferBinding* bufferBinding = std::get_if<BufferBinding>(&entry.resource)) {
      os << "buffer=" << RefId(BufferTag::kName, bufferBinding->buffer.slotIndex())
         << " offsetBytes=" << bufferBinding->offsetBytes
         << " sizeBytes=" << bufferBinding->sizeBytes;
    } else if (const TextureViewBinding* viewBinding =
                   std::get_if<TextureViewBinding>(&entry.resource)) {
      os << "textureView=" << RefId(TextureViewTag::kName, viewBinding->view.slotIndex());
    } else if (const SamplerBinding* samplerBinding =
                   std::get_if<SamplerBinding>(&entry.resource)) {
      os << "sampler=" << RefId(SamplerTag::kName, samplerBinding->sampler.slotIndex());
    }
    os << "}";
  }
  os << "]";
  lines_.push_back(os.str());
  return OkStatus();
}

Status RecordingDevice::onCreatePipelineLayout(uint32_t slotIndex,
                                               const PipelineLayoutDescriptor& descriptor) {
  std::ostringstream os = MakeLineStream();
  os << "createPipelineLayout " << RefId(PipelineLayoutTag::kName, slotIndex)
     << " label=" << QuoteLabel(descriptor.label) << " bindGroupLayouts=[";
  for (size_t i = 0; i < descriptor.bindGroupLayouts.size(); ++i) {
    if (i > 0) {
      os << " ";
    }
    os << RefId(BindGroupLayoutTag::kName, descriptor.bindGroupLayouts[i].slotIndex());
  }
  os << "]";
  lines_.push_back(os.str());
  return OkStatus();
}

Status RecordingDevice::onCreateShaderModule(uint32_t slotIndex,
                                             const ShaderModuleDescriptor& descriptor) {
  std::ostringstream os = MakeLineStream();
  os << "createShaderModule " << RefId(ShaderModuleTag::kName, slotIndex)
     << " label=" << QuoteLabel(descriptor.label) << " sourceKind=" << descriptor.sourceKind;
  if (descriptor.sourceKind == ShaderSourceKind::Spirv) {
    // Binary kinds record word count + content hash, mirroring the sourceBytes/sourceHash format
    // below. Words are hashed as their in-memory bytes; all supported targets are little-endian.
    os << " spirvWords=" << descriptor.spirvWords.size() << " spirvHash="
       << HashBytes(std::span<const uint8_t>(
              reinterpret_cast<const uint8_t*>(descriptor.spirvWords.data()),
              descriptor.spirvWords.size() * sizeof(uint32_t)));
  } else {
    const std::string_view source(descriptor.sourceText);
    os << " sourceBytes=" << source.size() << " sourceHash="
       << HashBytes(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(source.data()),
                                             source.size()));
  }
  lines_.push_back(os.str());
  return OkStatus();
}

Status RecordingDevice::onCreateRenderPipeline(uint32_t slotIndex,
                                               const RenderPipelineDescriptor& descriptor) {
  std::ostringstream os = MakeLineStream();
  os << "createRenderPipeline " << RefId(RenderPipelineTag::kName, slotIndex)
     << " label=" << QuoteLabel(descriptor.label)
     << " layout=" << RefId(PipelineLayoutTag::kName, descriptor.layout.slotIndex());

  os << " vertex={module=" << RefId(ShaderModuleTag::kName, descriptor.vertex.module.slotIndex())
     << " entryPoint=" << QuoteLabel(descriptor.vertex.entryPoint) << " ";
  AppendVertexBufferLayouts(os, descriptor.vertex.buffers);
  os << "}";

  os << " fragment={module="
     << RefId(ShaderModuleTag::kName, descriptor.fragment.module.slotIndex())
     << " entryPoint=" << QuoteLabel(descriptor.fragment.entryPoint) << " targets=[";
  for (size_t i = 0; i < descriptor.fragment.targets.size(); ++i) {
    if (i > 0) {
      os << " ";
    }
    const ColorTargetState& target = descriptor.fragment.targets[i];
    os << "{format=" << target.format << " blend=";
    if (target.blend) {
      os << "{color=";
      AppendBlendComponent(os, target.blend->color);
      os << " alpha=";
      AppendBlendComponent(os, target.blend->alpha);
      os << "}";
    } else {
      os << "none";
    }
    os << " writeMask=" << target.writeMask << "}";
  }
  os << "]}";

  os << " topology=" << descriptor.topology << " cullMode=" << descriptor.cullMode
     << " multisampleCount=" << descriptor.multisampleCount;
  lines_.push_back(os.str());
  return OkStatus();
}

void RecordingDevice::onDestroyResource(std::string_view resourceName, uint32_t slotIndex) {
  lines_.push_back("destroy " + RefId(resourceName, slotIndex));
}

Status RecordingDevice::onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                                      std::span<const uint8_t> data) {
  std::ostringstream os = MakeLineStream();
  os << "writeBuffer " << RefId(BufferTag::kName, slotIndex) << " offsetBytes=" << offsetBytes
     << " byteCount=" << data.size() << " dataHash=" << HashBytes(data);
  lines_.push_back(os.str());
  return OkStatus();
}

Status RecordingDevice::onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                                       const TexelCopyBufferLayout& dataLayout,
                                       const Extent2d& writeSize) {
  std::ostringstream os = MakeLineStream();
  os << "writeTexture " << RefId(TextureTag::kName, slotIndex)
     << " offsetBytes=" << dataLayout.offsetBytes << " bytesPerRow=" << dataLayout.bytesPerRow
     << " rowsPerImage=" << dataLayout.rowsPerImage << " writeSize=" << writeSize
     << " byteCount=" << data.size() << " dataHash=" << HashBytes(data);
  lines_.push_back(os.str());
  return OkStatus();
}

Status RecordingDevice::onSubmit(uint64_t submissionSerial, uint32_t commandBufferSlotIndex,
                                 std::span<const Command> commands) {
  std::ostringstream os = MakeLineStream();
  os << "submit serial=" << submissionSerial << " "
     << RefId(CommandBufferTag::kName, commandBufferSlotIndex)
     << " commandCount=" << commands.size();
  lines_.push_back(os.str());

  for (const Command& command : commands) {
    std::ostringstream commandStream = MakeLineStream();
    commandStream << "  ";
    std::visit(CommandSerializer{commandStream}, command);
    lines_.push_back(commandStream.str());
  }
  return OkStatus();
}

}  // namespace donner::gpu
