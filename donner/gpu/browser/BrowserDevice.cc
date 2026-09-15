#include "donner/gpu/browser/BrowserDevice.h"

#include <cstddef>
#include <format>
#include <optional>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "donner/base/Utils.h"
#include "donner/gpu/browser/BrowserWireCodes.h"

namespace donner::gpu::browser {

namespace {

/// Builds an error of \p type carrying \p message. @param type Error category.
/// @param message Failure reason.
GpuError Err(GpuErrorType type, std::string message) {
  return GpuError{type, std::move(message)};
}

/// Translates what the browser side reported into the runtime's error vocabulary.
///
/// The two identifier refusals become \ref GpuErrorType::InvalidHandle because that is what they
/// are from the caller's side: a name that no longer resolves. Ownership, loss and a browser-side
/// rejection become \ref GpuErrorType::InvalidState because the call was well formed and the
/// device was not in a position to perform it.
///
/// @param status Status the bridge returned. @param operation Operation name for the message.
GpuError ErrorForBridgeStatus(BridgeStatus status, std::string_view operation) {
  switch (status) {
    case BridgeStatus::Success:
      return Err(GpuErrorType::InvalidState, std::format("{}: succeeded unexpectedly", operation));
    case BridgeStatus::UnknownObject:
      return Err(GpuErrorType::InvalidHandle,
                 std::format("{}: the browser has no object under this identifier", operation));
    case BridgeStatus::WrongObjectKind:
      return Err(
          GpuErrorType::InvalidHandle,
          std::format("{}: this identifier names a browser object of another kind", operation));
    case BridgeStatus::NotOwner:
      return Err(GpuErrorType::InvalidState,
                 std::format("{}: the browser device belongs to another worker", operation));
    case BridgeStatus::DeviceLost:
      return Err(GpuErrorType::InvalidState,
                 std::format("{}: the browser device was lost", operation));
    case BridgeStatus::Failed:
      return Err(GpuErrorType::InvalidState,
                 std::format("{}: the browser refused the operation", operation));
  }
  return Err(GpuErrorType::InvalidState, std::format("{}: unrecognized bridge status", operation));
}

/// Bounds a caller-supplied wait slice to something a backend can express.
///
/// The runtime only checks that a slice is above zero, so the value arriving here can be anything,
/// including a value no fixed-width unit can hold. Clamping does not shorten the wait, because the
/// runtime re-enters this hook until its own budget elapses; it only means the browser gets the
/// thread back more often. A value that is not a number yields the shortest slice rather than
/// propagating into the conversion.
///
/// @param seconds Slice the caller asked for.
/// @param maxSeconds Longest slice this device will hand over.
double ClampYieldSeconds(double seconds, double maxSeconds) {
  if (!(seconds > 0.0)) {
    return 0.0;
  }
  return seconds < maxSeconds ? seconds : maxSeconds;
}

/// Success, or the error \p status stands for. @param status Status the bridge returned.
/// @param operation Operation name for the message.
Status StatusForBridge(BridgeStatus status, std::string_view operation) {
  if (status == BridgeStatus::Success) {
    return OkStatus();
  }
  return ErrorForBridgeStatus(status, operation);
}

/// Encodes \p value, failing closed when it is not a value this protocol can express.
/// @param encoded Result of one of the \ref BrowserWireCodes.h translations.
/// @param field Field name for the message. @param operation Operation name for the message.
Result<uint32_t> RequireCode(std::optional<uint32_t> encoded, std::string_view field,
                             std::string_view operation) {
  if (!encoded.has_value()) {
    return Err(
        GpuErrorType::InvalidDescriptor,
        std::format("{}: {} is not a value the browser bridge can express", operation, field));
  }
  return *encoded;
}

/// Translates one blend term, failing closed on a factor or operation this protocol cannot
/// express. @param component Validated blend term.
Result<BrowserBlendComponent> TranslateBlendComponent(const BlendComponent& component) {
  static constexpr std::string_view kOperation = "createRenderPipeline";

  Result<uint32_t> srcFactor =
      RequireCode(WireBlendFactor(component.srcFactor), "BlendComponent.srcFactor", kOperation);
  if (srcFactor.hasError()) {
    return std::move(srcFactor).error();
  }
  Result<uint32_t> dstFactor =
      RequireCode(WireBlendFactor(component.dstFactor), "BlendComponent.dstFactor", kOperation);
  if (dstFactor.hasError()) {
    return std::move(dstFactor).error();
  }
  Result<uint32_t> operation =
      RequireCode(WireBlendOperation(component.operation), "BlendComponent.operation", kOperation);
  if (operation.hasError()) {
    return std::move(operation).error();
  }
  return BrowserBlendComponent{srcFactor.result(), dstFactor.result(), operation.result()};
}

/// The object kind the runtime's diagnostic name for a resource stands for, or nullopt for a
/// resource this backend keeps no browser object for.
/// @param resourceName Name the runtime passes to its destruction hook.
std::optional<BrowserObjectKind> KindForResourceName(std::string_view resourceName) {
  if (resourceName == "buffer") {
    return BrowserObjectKind::Buffer;
  } else if (resourceName == "texture") {
    return BrowserObjectKind::Texture;
  } else if (resourceName == "textureView") {
    return BrowserObjectKind::TextureView;
  } else if (resourceName == "sampler") {
    return BrowserObjectKind::Sampler;
  } else if (resourceName == "bindGroupLayout") {
    return BrowserObjectKind::BindGroupLayout;
  } else if (resourceName == "bindGroup") {
    return BrowserObjectKind::BindGroup;
  } else if (resourceName == "pipelineLayout") {
    return BrowserObjectKind::PipelineLayout;
  } else if (resourceName == "shaderModule") {
    return BrowserObjectKind::ShaderModule;
  } else if (resourceName == "renderPipeline") {
    return BrowserObjectKind::RenderPipeline;
  } else if (resourceName == "computePipeline") {
    return BrowserObjectKind::ComputePipeline;
  }
  return std::nullopt;
}

/// Returns the enumerators of \p known whose codes appear in \p codes, in the order \p codes
/// lists them, and drops any code \p known has no enumerator for.
///
/// What a browser reports describes the browser, not this process, so an unrecognized code is
/// dropped rather than cast into an enumerator that would then flow into format and layout
/// decisions. Dropping is right here and refusing is right on the sending side: this is a menu the
/// caller chooses from, so a entry that cannot be named is simply not offered.
///
/// @tparam Enum Runtime enumeration being decoded.
/// @param codes Codes the browser reported.
/// @param known Every enumerator this protocol can express.
/// @param encode Translation from enumerator to code.
template <typename Enum, size_t N>
std::vector<Enum> DecodeList(const std::vector<uint32_t>& codes, const Enum (&known)[N],
                             std::optional<uint32_t> (*encode)(Enum)) {
  std::vector<Enum> decoded;
  for (const uint32_t code : codes) {
    for (const Enum candidate : known) {
      if (encode(candidate) == code) {
        decoded.push_back(candidate);
      }
    }
  }
  return decoded;
}

/// Returns the flags of \p known whose codes are set in \p bits, ignoring any bit \p known has no
/// flag for, for the same reason \ref DecodeList drops an unrecognized code.
///
/// @tparam Enum Runtime bitmask enumeration being decoded.
/// @param bits Mask the browser reported.
/// @param known Every flag this protocol can express.
/// @param encode Translation from flag to code.
template <typename Enum, size_t N>
Enum DecodeMask(uint32_t bits, const Enum (&known)[N], std::optional<uint32_t> (*encode)(Enum)) {
  Enum decoded = {};
  for (const Enum candidate : known) {
    const std::optional<uint32_t> bit = encode(candidate);
    if (bit.has_value() && (bits & *bit) != 0) {
      decoded |= candidate;
    }
  }
  return decoded;
}

/// Decodes what a browser reported about a surface into the runtime's own terms.
/// @param reported Capabilities the browser side supplied.
SurfaceCapabilities DecodeSurfaceCapabilities(const BrowserSurfaceCapabilities& reported) {
  static constexpr TextureFormat kFormats[] = {TextureFormat::RGBA8Unorm, TextureFormat::BGRA8Unorm,
                                               TextureFormat::R8Unorm, TextureFormat::RGBA32Float};
  static constexpr TextureUsage kUsages[] = {TextureUsage::RenderAttachment, TextureUsage::Sampled,
                                             TextureUsage::CopySrc, TextureUsage::CopyDst,
                                             TextureUsage::StorageBinding};
  static constexpr PresentMode kPresentModes[] = {PresentMode::Fifo, PresentMode::Immediate,
                                                  PresentMode::Mailbox};
  static constexpr SurfaceAlphaMode kAlphaModes[] = {
      SurfaceAlphaMode::Opaque, SurfaceAlphaMode::Premultiplied, SurfaceAlphaMode::Inherit};

  SurfaceCapabilities capabilities;
  capabilities.formats = DecodeList(reported.formatCodes, kFormats, &WireTextureFormat);
  capabilities.presentModes =
      DecodeList(reported.presentModeCodes, kPresentModes, &WirePresentMode);
  capabilities.alphaModes = DecodeList(reported.alphaModeCodes, kAlphaModes, &WireSurfaceAlphaMode);
  capabilities.usages = DecodeMask(reported.usageBits, kUsages, &WireTextureUsage);
  return capabilities;
}

}  // namespace

BrowserDeviceRequest::BrowserDeviceRequest(std::unique_ptr<BrowserBridge> bridge,
                                           RcString beginError)
    : bridge_(std::move(bridge)), beginError_(std::move(beginError)) {}

BrowserDeviceRequest::BrowserDeviceRequest(BrowserDeviceRequest&& other) noexcept = default;

BrowserDeviceRequest& BrowserDeviceRequest::operator=(BrowserDeviceRequest&& other) noexcept =
    default;

BrowserDeviceRequest::~BrowserDeviceRequest() = default;

BrowserDeviceRequest BrowserDeviceRequest::Begin(std::unique_ptr<BrowserBridge> bridge) {
  if (bridge == nullptr) {
    return BrowserDeviceRequest(nullptr, RcString("no bridge to the browser's GPU service"));
  }
  const BridgeStatus status = bridge->beginDeviceRequest();
  if (status != BridgeStatus::Success) {
    // The bridge may already know more than the status says - a protocol table that disagreed
    // names the entry it disagreed on - and that detail is the whole diagnosis, so it is carried
    // out rather than flattened into "the browser refused the operation".
    const GpuError error = ErrorForBridgeStatus(status, "BrowserDeviceRequest::Begin");
    const RcString detail = bridge->deviceRequestError();
    RcString reason = detail.empty() ? RcString(error.message)
                                     : RcString(std::format("{}: {}", error.message, detail.str()));
    return BrowserDeviceRequest(std::move(bridge), std::move(reason));
  }
  return BrowserDeviceRequest(std::move(bridge), RcString());
}

BrowserDeviceRequestState BrowserDeviceRequest::state() const {
  if (bridge_ == nullptr || !beginError_.empty()) {
    return BrowserDeviceRequestState::Failed;
  }
  return bridge_->deviceRequestState();
}

RcString BrowserDeviceRequest::error() const {
  if (!beginError_.empty()) {
    return beginError_;
  }
  if (bridge_ == nullptr) {
    return RcString();
  }
  return bridge_->deviceRequestError();
}

Result<std::unique_ptr<BrowserDevice>> BrowserDeviceRequest::take() && {
  if (!beginError_.empty()) {
    return Err(GpuErrorType::InvalidState,
               std::format("BrowserDeviceRequest::take: the request was never begun: {}",
                           beginError_.str()));
  }
  std::unique_ptr<BrowserBridge> bridge = std::move(bridge_);
  if (bridge == nullptr) {
    return Err(GpuErrorType::InvalidState, "BrowserDeviceRequest::take: the request was consumed");
  }

  const BrowserDeviceRequestState state = bridge->deviceRequestState();
  switch (state) {
    case BrowserDeviceRequestState::Ready: break;
    case BrowserDeviceRequestState::Pending:
      return Err(GpuErrorType::InvalidState,
                 "BrowserDeviceRequest::take: the browser has not settled the request yet");
    case BrowserDeviceRequestState::Unavailable:
      return Err(GpuErrorType::Unsupported,
                 "BrowserDeviceRequest::take: this browser exposes no GPU service");
    case BrowserDeviceRequestState::Failed:
      return Err(GpuErrorType::InvalidState,
                 std::format("BrowserDeviceRequest::take: the browser refused to supply a "
                             "device: {}",
                             bridge->deviceRequestError().str()));
  }

  return std::unique_ptr<BrowserDevice>(new BrowserDevice(std::move(bridge)));
}

BrowserDevice::BrowserDevice(std::unique_ptr<BrowserBridge> bridge)
    : bridge_(std::move(bridge)), ownerThread_(std::this_thread::get_id()) {}

BrowserDevice::~BrowserDevice() {
  // Destroying a device from inside its own wait would free this object while the wait is still
  // going to read through it when the browser hands the thread back. It cannot be guarded against
  // from here - the storage is already going away - so it is named instead: this aborts where the
  // contract was broken rather than reading freed memory a step later.
  UTILS_RELEASE_ASSERT_MSG(!yielding_,
                           "a browser device was destroyed while one of its waits had handed the "
                           "thread to the browser; release the device from outside the wait");

  // Hand back any frame a surface still holds first: a frame texture belongs to the canvas that
  // supplied it, so the sweep below must not reach one.
  for (uint32_t surfaceSlotIndex = 0;
       surfaceSlotIndex < static_cast<uint32_t>(acquiredTextureBySurface_.size());
       ++surfaceSlotIndex) {
    if (acquiredTextureBySurface_[surfaceSlotIndex] != kNoAcquiredTexture) {
      releaseAcquiredFrame(surfaceSlotIndex);
    }
  }

  // The base destructor has not run yet, so handles may still name live resources; releasing the
  // browser objects here and clearing the table keeps each one released exactly once whichever
  // order the remaining handles unwind in.
  for (const std::pair<BrowserObjectKind, BrowserObjectId>& entry : objects_.takeAll()) {
    bridge_->destroyObject(entry.first, entry.second);
  }
}

uint64_t BrowserDevice::completedSerial() const {
  return bridge_->completedSerial();
}

bool BrowserDevice::isDeviceLost() const {
  return bridge_->isDeviceLost();
}

RcString BrowserDevice::deviceLostReason() const {
  return bridge_->deviceLostReason();
}

Status BrowserDevice::checkUsable(std::string_view operation) const {
  if (std::this_thread::get_id() != ownerThread_) {
    return Err(GpuErrorType::InvalidState,
               std::format("{}: this browser device belongs to the context that obtained it and "
                           "cannot be used from another thread",
                           operation));
  }
  if (!bridge_->ownsDevice()) {
    return Err(GpuErrorType::InvalidState,
               std::format("{}: the browser device belongs to another worker", operation));
  }
  if (bridge_->isDeviceLost()) {
    return Err(GpuErrorType::InvalidState,
               std::format("{}: the browser device was lost: {}", operation,
                           bridge_->deviceLostReason().str()));
  }
  return OkStatus();
}

Result<BrowserObjectId> BrowserDevice::objectFor(BrowserObjectKind kind, uint32_t slotIndex,
                                                 std::string_view operation) const {
  const std::optional<BrowserObjectId> id = objects_.find(kind, slotIndex);
  if (!id.has_value()) {
    return Err(GpuErrorType::InvalidHandle,
               std::format("{}: this device has no browser {} for slot {}", operation,
                           BrowserObjectKindName(kind), slotIndex));
  }
  return *id;
}

Result<BrowserObjectId> BrowserDevice::registerObject(BrowserObjectKind kind, uint32_t slotIndex,
                                                      std::string_view operation) {
  // A texture slot about to be reused may still be named as some surface's frame, because dropping
  // a Surface handle reaches no backend hook: the runtime releases the surface and its frame
  // without telling this device. Hand that frame back before the slot changes hands, or the record
  // would go on naming a slot the caller now owns and teardown would take the caller's texture
  // while orphaning the frame.
  if (kind == BrowserObjectKind::Texture) {
    releaseFramesNaming(slotIndex);
  } else if (kind == BrowserObjectKind::Surface) {
    // The mirror of the texture case: a surface slot can be reused while this device still records
    // a frame against it, and the frame has to go back to the surface that supplied it before that
    // surface is displaced, or the record would outlive the surface it names.
    releaseAcquiredFrame(slotIndex);
  }

  const BrowserObjectInsertion insertion = objects_.insert(kind, slotIndex);
  if (insertion.displaced != kNoBrowserObject) {
    bridge_->destroyObject(kind, insertion.displaced);
  }
  if (insertion.id == kNoBrowserObject) {
    return Err(GpuErrorType::LimitExceeded,
               std::format("{}: the browser object identifier space is exhausted", operation));
  }
  return insertion.id;
}

void BrowserDevice::setAcquiredTexture(uint32_t surfaceSlotIndex, uint32_t textureSlotIndex) {
  if (surfaceSlotIndex >= acquiredTextureBySurface_.size()) {
    acquiredTextureBySurface_.resize(static_cast<size_t>(surfaceSlotIndex) + 1, kNoAcquiredTexture);
  }
  acquiredTextureBySurface_[surfaceSlotIndex] = textureSlotIndex;
}

uint32_t BrowserDevice::acquiredTexture(uint32_t surfaceSlotIndex) const {
  if (surfaceSlotIndex >= acquiredTextureBySurface_.size()) {
    return kNoAcquiredTexture;
  }
  return acquiredTextureBySurface_[surfaceSlotIndex];
}

bool BrowserDevice::isAcquiredFrame(uint32_t textureSlotIndex) const {
  for (const uint32_t acquired : acquiredTextureBySurface_) {
    if (acquired == textureSlotIndex) {
      return true;
    }
  }
  return false;
}

void BrowserDevice::releaseFramesNaming(uint32_t textureSlotIndex) {
  for (uint32_t surfaceSlotIndex = 0;
       surfaceSlotIndex < static_cast<uint32_t>(acquiredTextureBySurface_.size());
       ++surfaceSlotIndex) {
    if (acquiredTextureBySurface_[surfaceSlotIndex] == textureSlotIndex) {
      releaseAcquiredFrame(surfaceSlotIndex);
    }
  }
}

void BrowserDevice::releaseObject(BrowserObjectKind kind, uint32_t slotIndex) {
  if (!onOwnerThread()) {
    // Keep the entry: teardown runs on the owning thread and frees it there, whereas naming it to
    // this worker would release nothing and lose the identifier.
    foreignThreadReleases_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  if (kind == BrowserObjectKind::Texture && isAcquiredFrame(slotIndex)) {
    // The canvas owns this texture; handing the frame back is the release, and destroying it would
    // take away the surface's own texture instead.
    releaseFramesNaming(slotIndex);
    return;
  }
  const std::optional<BrowserObjectId> id = objects_.remove(kind, slotIndex);
  if (id.has_value()) {
    bridge_->destroyObject(kind, *id);
  }
}

Status BrowserDevice::onCreateBuffer(uint32_t slotIndex, const BufferDescriptor& descriptor) {
  static constexpr std::string_view kOperation = "createBuffer";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  Result<uint32_t> usage =
      RequireCode(WireBufferUsage(descriptor.usage), "BufferDescriptor.usage", kOperation);
  if (usage.hasError()) {
    return std::move(usage).error();
  }
  Result<BrowserObjectId> id = registerObject(BrowserObjectKind::Buffer, slotIndex, kOperation);
  if (id.hasError()) {
    return std::move(id).error();
  }

  const BridgeStatus status =
      bridge_->createBuffer(id.result(), descriptor.byteSize, usage.result());
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::Buffer, slotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

Status BrowserDevice::onCreateTexture(uint32_t slotIndex, const TextureDescriptor& descriptor) {
  static constexpr std::string_view kOperation = "createTexture";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  Result<uint32_t> format =
      RequireCode(WireTextureFormat(descriptor.format), "TextureDescriptor.format", kOperation);
  if (format.hasError()) {
    return std::move(format).error();
  }
  Result<uint32_t> usage =
      RequireCode(WireTextureUsage(descriptor.usage), "TextureDescriptor.usage", kOperation);
  if (usage.hasError()) {
    return std::move(usage).error();
  }
  Result<BrowserObjectId> id = registerObject(BrowserObjectKind::Texture, slotIndex, kOperation);
  if (id.hasError()) {
    return std::move(id).error();
  }

  const BridgeStatus status = bridge_->createTexture(
      id.result(), descriptor.size.width, descriptor.size.height, format.result(), usage.result());
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::Texture, slotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

Status BrowserDevice::onCreateTextureView(uint32_t slotIndex, uint32_t textureSlotIndex,
                                          const TextureViewDescriptor& /*descriptor*/) {
  static constexpr std::string_view kOperation = "createTextureView";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  Result<BrowserObjectId> textureId =
      objectFor(BrowserObjectKind::Texture, textureSlotIndex, kOperation);
  if (textureId.hasError()) {
    return std::move(textureId).error();
  }
  Result<BrowserObjectId> id =
      registerObject(BrowserObjectKind::TextureView, slotIndex, kOperation);
  if (id.hasError()) {
    return std::move(id).error();
  }

  const BridgeStatus status = bridge_->createTextureView(id.result(), textureId.result());
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::TextureView, slotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

Status BrowserDevice::onCreateSampler(uint32_t slotIndex, const SamplerDescriptor& descriptor) {
  static constexpr std::string_view kOperation = "createSampler";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  Result<uint32_t> magFilter =
      RequireCode(WireFilterMode(descriptor.magFilter), "SamplerDescriptor.magFilter", kOperation);
  if (magFilter.hasError()) {
    return std::move(magFilter).error();
  }
  Result<uint32_t> minFilter =
      RequireCode(WireFilterMode(descriptor.minFilter), "SamplerDescriptor.minFilter", kOperation);
  if (minFilter.hasError()) {
    return std::move(minFilter).error();
  }
  Result<uint32_t> addressU = RequireCode(WireAddressMode(descriptor.addressModeU),
                                          "SamplerDescriptor.addressModeU", kOperation);
  if (addressU.hasError()) {
    return std::move(addressU).error();
  }
  Result<uint32_t> addressV = RequireCode(WireAddressMode(descriptor.addressModeV),
                                          "SamplerDescriptor.addressModeV", kOperation);
  if (addressV.hasError()) {
    return std::move(addressV).error();
  }
  Result<BrowserObjectId> id = registerObject(BrowserObjectKind::Sampler, slotIndex, kOperation);
  if (id.hasError()) {
    return std::move(id).error();
  }

  const BridgeStatus status = bridge_->createSampler(
      id.result(), magFilter.result(), minFilter.result(), addressU.result(), addressV.result());
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::Sampler, slotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

Status BrowserDevice::onCreateBindGroupLayout(uint32_t slotIndex,
                                              const BindGroupLayoutDescriptor& descriptor) {
  static constexpr std::string_view kOperation = "createBindGroupLayout";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }

  std::vector<BrowserBindGroupLayoutEntry> entries;
  entries.reserve(descriptor.entries.size());
  for (const BindGroupLayoutEntry& entry : descriptor.entries) {
    Result<uint32_t> visibility = RequireCode(WireShaderStage(entry.visibility),
                                              "BindGroupLayoutEntry.visibility", kOperation);
    if (visibility.hasError()) {
      return std::move(visibility).error();
    }
    Result<uint32_t> type =
        RequireCode(WireBindingType(entry.type), "BindGroupLayoutEntry.type", kOperation);
    if (type.hasError()) {
      return std::move(type).error();
    }
    Result<uint32_t> storageFormat =
        RequireCode(WireTextureFormat(entry.storageTextureFormat),
                    "BindGroupLayoutEntry.storageTextureFormat", kOperation);
    if (storageFormat.hasError()) {
      return std::move(storageFormat).error();
    }
    entries.push_back(BrowserBindGroupLayoutEntry{entry.binding, visibility.result(), type.result(),
                                                  storageFormat.result()});
  }

  Result<BrowserObjectId> id =
      registerObject(BrowserObjectKind::BindGroupLayout, slotIndex, kOperation);
  if (id.hasError()) {
    return std::move(id).error();
  }

  const BridgeStatus status = bridge_->createBindGroupLayout(id.result(), entries);
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::BindGroupLayout, slotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

Result<BrowserBindGroupEntry> BrowserDevice::translateBindGroupEntry(
    const BindGroupEntry& entry, std::string_view operation) const {
  BrowserBindGroupEntry translated;
  translated.binding = entry.binding;

  if (const BufferBinding* binding = std::get_if<BufferBinding>(&entry.resource)) {
    Result<BrowserObjectId> bufferId =
        objectFor(BrowserObjectKind::Buffer, binding->buffer.slotIndex(), operation);
    if (bufferId.hasError()) {
      return std::move(bufferId).error();
    }
    translated.resource = BrowserBindingResource::Buffer;
    translated.resourceId = bufferId.result();
    translated.offsetBytes = binding->offsetBytes;
    translated.sizeBytes = binding->sizeBytes;
    return translated;
  }

  if (const TextureViewBinding* binding = std::get_if<TextureViewBinding>(&entry.resource)) {
    Result<BrowserObjectId> viewId =
        objectFor(BrowserObjectKind::TextureView, binding->view.slotIndex(), operation);
    if (viewId.hasError()) {
      return std::move(viewId).error();
    }
    translated.resource = BrowserBindingResource::TextureView;
    translated.resourceId = viewId.result();
    return translated;
  }

  if (const SamplerBinding* binding = std::get_if<SamplerBinding>(&entry.resource)) {
    Result<BrowserObjectId> samplerId =
        objectFor(BrowserObjectKind::Sampler, binding->sampler.slotIndex(), operation);
    if (samplerId.hasError()) {
      return std::move(samplerId).error();
    }
    translated.resource = BrowserBindingResource::Sampler;
    translated.resourceId = samplerId.result();
    return translated;
  }

  return Err(GpuErrorType::InvalidDescriptor,
             std::format("{}: binding {} holds no recognized resource", operation, entry.binding));
}

Status BrowserDevice::onCreateBindGroup(uint32_t slotIndex, const BindGroupDescriptor& descriptor) {
  static constexpr std::string_view kOperation = "createBindGroup";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  Result<BrowserObjectId> layoutId =
      objectFor(BrowserObjectKind::BindGroupLayout, descriptor.layout.slotIndex(), kOperation);
  if (layoutId.hasError()) {
    return std::move(layoutId).error();
  }

  std::vector<BrowserBindGroupEntry> entries;
  entries.reserve(descriptor.entries.size());
  for (const BindGroupEntry& entry : descriptor.entries) {
    Result<BrowserBindGroupEntry> translated = translateBindGroupEntry(entry, kOperation);
    if (translated.hasError()) {
      return std::move(translated).error();
    }
    entries.push_back(std::move(translated).result());
  }

  Result<BrowserObjectId> id = registerObject(BrowserObjectKind::BindGroup, slotIndex, kOperation);
  if (id.hasError()) {
    return std::move(id).error();
  }

  const BridgeStatus status = bridge_->createBindGroup(id.result(), layoutId.result(), entries);
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::BindGroup, slotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

Status BrowserDevice::onCreatePipelineLayout(uint32_t slotIndex,
                                             const PipelineLayoutDescriptor& descriptor) {
  static constexpr std::string_view kOperation = "createPipelineLayout";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }

  std::vector<BrowserObjectId> groupLayoutIds;
  groupLayoutIds.reserve(descriptor.bindGroupLayouts.size());
  for (const BindGroupLayoutRef& layout : descriptor.bindGroupLayouts) {
    Result<BrowserObjectId> layoutId =
        objectFor(BrowserObjectKind::BindGroupLayout, layout.slotIndex(), kOperation);
    if (layoutId.hasError()) {
      return std::move(layoutId).error();
    }
    groupLayoutIds.push_back(layoutId.result());
  }

  Result<BrowserObjectId> id =
      registerObject(BrowserObjectKind::PipelineLayout, slotIndex, kOperation);
  if (id.hasError()) {
    return std::move(id).error();
  }

  const BridgeStatus status = bridge_->createPipelineLayout(id.result(), groupLayoutIds);
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::PipelineLayout, slotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

Status BrowserDevice::onCreateShaderModule(uint32_t slotIndex,
                                           const ShaderModuleDescriptor& descriptor) {
  static constexpr std::string_view kOperation = "createShaderModule";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  if (descriptor.sourceKind != ShaderSourceKind::Wgsl) {
    return Err(GpuErrorType::Unsupported,
               std::format("{}: the browser bridge accepts the WGSL projection only", kOperation));
  }

  Result<BrowserObjectId> id =
      registerObject(BrowserObjectKind::ShaderModule, slotIndex, kOperation);
  if (id.hasError()) {
    return std::move(id).error();
  }

  const BridgeStatus status = bridge_->createShaderModule(id.result(), descriptor.sourceText.str());
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::ShaderModule, slotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

Status BrowserDevice::buildVertexBuffers(const VertexState& vertex,
                                         std::vector<BrowserVertexBufferLayout>& layouts) const {
  static constexpr std::string_view kOperation = "createRenderPipeline";

  layouts.reserve(vertex.buffers.size());
  for (const VertexBufferLayout& layout : vertex.buffers) {
    Result<uint32_t> stepMode =
        RequireCode(WireVertexStepMode(layout.stepMode), "VertexBufferLayout.stepMode", kOperation);
    if (stepMode.hasError()) {
      return std::move(stepMode).error();
    }

    BrowserVertexBufferLayout translated;
    translated.strideBytes = layout.strideBytes;
    translated.stepModeCode = stepMode.result();
    translated.attributes.reserve(layout.attributes.size());
    for (const VertexAttribute& attribute : layout.attributes) {
      Result<uint32_t> format =
          RequireCode(WireVertexFormat(attribute.format), "VertexAttribute.format", kOperation);
      if (format.hasError()) {
        return std::move(format).error();
      }
      translated.attributes.push_back(
          BrowserVertexAttribute{format.result(), attribute.offsetBytes, attribute.shaderLocation});
    }
    layouts.push_back(std::move(translated));
  }
  return OkStatus();
}

Status BrowserDevice::buildColorTargets(const FragmentState& fragment,
                                        std::vector<BrowserColorTarget>& targets) const {
  static constexpr std::string_view kOperation = "createRenderPipeline";

  targets.reserve(fragment.targets.size());
  for (const ColorTargetState& target : fragment.targets) {
    Result<uint32_t> format =
        RequireCode(WireTextureFormat(target.format), "ColorTargetState.format", kOperation);
    if (format.hasError()) {
      return std::move(format).error();
    }
    Result<uint32_t> writeMask =
        RequireCode(WireColorWriteMask(target.writeMask), "ColorTargetState.writeMask", kOperation);
    if (writeMask.hasError()) {
      return std::move(writeMask).error();
    }

    BrowserColorTarget translated;
    translated.formatCode = format.result();
    translated.writeMaskBits = writeMask.result();
    translated.blendEnabled = target.blend.has_value();
    if (target.blend.has_value()) {
      Result<BrowserBlendComponent> color = TranslateBlendComponent(target.blend->color);
      if (color.hasError()) {
        return std::move(color).error();
      }
      Result<BrowserBlendComponent> alpha = TranslateBlendComponent(target.blend->alpha);
      if (alpha.hasError()) {
        return std::move(alpha).error();
      }
      translated.colorBlend = color.result();
      translated.alphaBlend = alpha.result();
    }
    targets.push_back(std::move(translated));
  }
  return OkStatus();
}

Status BrowserDevice::buildRenderPipelineRequest(const RenderPipelineDescriptor& descriptor,
                                                 BrowserRenderPipelineRequest& request) const {
  static constexpr std::string_view kOperation = "createRenderPipeline";

  Result<BrowserObjectId> layoutId =
      objectFor(BrowserObjectKind::PipelineLayout, descriptor.layout.slotIndex(), kOperation);
  if (layoutId.hasError()) {
    return std::move(layoutId).error();
  }
  Result<BrowserObjectId> vertexModuleId =
      objectFor(BrowserObjectKind::ShaderModule, descriptor.vertex.module.slotIndex(), kOperation);
  if (vertexModuleId.hasError()) {
    return std::move(vertexModuleId).error();
  }
  Result<BrowserObjectId> fragmentModuleId = objectFor(
      BrowserObjectKind::ShaderModule, descriptor.fragment.module.slotIndex(), kOperation);
  if (fragmentModuleId.hasError()) {
    return std::move(fragmentModuleId).error();
  }
  Result<uint32_t> topology = RequireCode(WirePrimitiveTopology(descriptor.topology),
                                          "RenderPipelineDescriptor.topology", kOperation);
  if (topology.hasError()) {
    return std::move(topology).error();
  }
  Result<uint32_t> cullMode = RequireCode(WireCullMode(descriptor.cullMode),
                                          "RenderPipelineDescriptor.cullMode", kOperation);
  if (cullMode.hasError()) {
    return std::move(cullMode).error();
  }

  request.layoutId = layoutId.result();
  request.vertexModuleId = vertexModuleId.result();
  request.vertexEntryPoint = descriptor.vertex.entryPoint;
  request.fragmentModuleId = fragmentModuleId.result();
  request.fragmentEntryPoint = descriptor.fragment.entryPoint;
  request.topologyCode = topology.result();
  request.cullModeCode = cullMode.result();

  if (Status status = buildVertexBuffers(descriptor.vertex, request.vertexBuffers);
      status.hasError()) {
    return status;
  }
  return buildColorTargets(descriptor.fragment, request.colorTargets);
}

Status BrowserDevice::onCreateRenderPipeline(uint32_t slotIndex,
                                             const RenderPipelineDescriptor& descriptor) {
  static constexpr std::string_view kOperation = "createRenderPipeline";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }

  BrowserRenderPipelineRequest request;
  if (Status status = buildRenderPipelineRequest(descriptor, request); status.hasError()) {
    return status;
  }

  Result<BrowserObjectId> id =
      registerObject(BrowserObjectKind::RenderPipeline, slotIndex, kOperation);
  if (id.hasError()) {
    return std::move(id).error();
  }

  const BridgeStatus status = bridge_->createRenderPipeline(id.result(), request);
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::RenderPipeline, slotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

Status BrowserDevice::onCreateComputePipeline(uint32_t slotIndex,
                                              const ComputePipelineDescriptor& descriptor) {
  static constexpr std::string_view kOperation = "createComputePipeline";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  Result<BrowserObjectId> layoutId =
      objectFor(BrowserObjectKind::PipelineLayout, descriptor.layout.slotIndex(), kOperation);
  if (layoutId.hasError()) {
    return std::move(layoutId).error();
  }
  Result<BrowserObjectId> moduleId =
      objectFor(BrowserObjectKind::ShaderModule, descriptor.compute.module.slotIndex(), kOperation);
  if (moduleId.hasError()) {
    return std::move(moduleId).error();
  }

  BrowserComputePipelineRequest request;
  request.layoutId = layoutId.result();
  request.moduleId = moduleId.result();
  request.entryPoint = descriptor.compute.entryPoint;

  Result<BrowserObjectId> id =
      registerObject(BrowserObjectKind::ComputePipeline, slotIndex, kOperation);
  if (id.hasError()) {
    return std::move(id).error();
  }

  const BridgeStatus status = bridge_->createComputePipeline(id.result(), request);
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::ComputePipeline, slotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

void BrowserDevice::onDestroyResource(std::string_view resourceName, uint32_t slotIndex) {
  const std::optional<BrowserObjectKind> kind = KindForResourceName(resourceName);
  if (kind.has_value()) {
    releaseObject(*kind, slotIndex);
  }
}

Status BrowserDevice::onWriteBuffer(uint32_t slotIndex, uint64_t offsetBytes,
                                    std::span<const uint8_t> data) {
  static constexpr std::string_view kOperation = "writeBuffer";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  Result<BrowserObjectId> bufferId = objectFor(BrowserObjectKind::Buffer, slotIndex, kOperation);
  if (bufferId.hasError()) {
    return std::move(bufferId).error();
  }
  return StatusForBridge(bridge_->writeBuffer(bufferId.result(), offsetBytes, data), kOperation);
}

Status BrowserDevice::onWriteTexture(uint32_t slotIndex, std::span<const uint8_t> data,
                                     const TexelCopyBufferLayout& dataLayout,
                                     const Extent2d& writeSize, const Origin2d& destinationOrigin) {
  static constexpr std::string_view kOperation = "writeTexture";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  Result<BrowserObjectId> textureId = objectFor(BrowserObjectKind::Texture, slotIndex, kOperation);
  if (textureId.hasError()) {
    return std::move(textureId).error();
  }

  const BrowserTexelLayout layout{dataLayout.offsetBytes, dataLayout.bytesPerRow,
                                  dataLayout.rowsPerImage};
  BrowserCopyRegion region;
  region.width = writeSize.width;
  region.height = writeSize.height;
  return StatusForBridge(bridge_->writeTexture(textureId.result(), data, layout, region),
                         kOperation);
}

Status BrowserDevice::replay(const BeginRenderPassCommand& command, std::string_view operation) {
  std::vector<BrowserColorAttachment> attachments;
  attachments.reserve(command.descriptor.colorAttachments.size());
  for (const RenderPassColorAttachment& attachment : command.descriptor.colorAttachments) {
    Result<BrowserObjectId> viewId =
        objectFor(BrowserObjectKind::TextureView, attachment.view.slotIndex(), operation);
    if (viewId.hasError()) {
      return std::move(viewId).error();
    }
    Result<uint32_t> loadOp =
        RequireCode(WireLoadOp(attachment.loadOp), "RenderPassColorAttachment.loadOp", operation);
    if (loadOp.hasError()) {
      return std::move(loadOp).error();
    }
    Result<uint32_t> storeOp = RequireCode(WireStoreOp(attachment.storeOp),
                                           "RenderPassColorAttachment.storeOp", operation);
    if (storeOp.hasError()) {
      return std::move(storeOp).error();
    }

    BrowserColorAttachment translated;
    translated.viewId = viewId.result();
    translated.loadOpCode = loadOp.result();
    translated.storeOpCode = storeOp.result();
    translated.clearColor = attachment.clearColor;
    attachments.push_back(translated);
  }
  return StatusForBridge(bridge_->beginRenderPass(attachments), operation);
}

Status BrowserDevice::replay(const SetPipelineCommand& command, std::string_view operation) {
  Result<BrowserObjectId> pipelineId =
      objectFor(BrowserObjectKind::RenderPipeline, command.pipelineId.slotIndex, operation);
  if (pipelineId.hasError()) {
    return std::move(pipelineId).error();
  }
  return StatusForBridge(bridge_->setRenderPipeline(pipelineId.result()), operation);
}

Status BrowserDevice::replay(const SetBindGroupCommand& command, std::string_view operation) {
  Result<BrowserObjectId> bindGroupId =
      objectFor(BrowserObjectKind::BindGroup, command.bindGroupId.slotIndex, operation);
  if (bindGroupId.hasError()) {
    return std::move(bindGroupId).error();
  }
  return StatusForBridge(bridge_->setBindGroup(command.index, bindGroupId.result()), operation);
}

Status BrowserDevice::replay(const SetVertexBufferCommand& command, std::string_view operation) {
  Result<BrowserObjectId> bufferId =
      objectFor(BrowserObjectKind::Buffer, command.bufferId.slotIndex, operation);
  if (bufferId.hasError()) {
    return std::move(bufferId).error();
  }
  return StatusForBridge(
      bridge_->setVertexBuffer(command.slot, bufferId.result(), command.offsetBytes), operation);
}

Status BrowserDevice::replay(const SetIndexBufferCommand& command, std::string_view operation) {
  Result<BrowserObjectId> bufferId =
      objectFor(BrowserObjectKind::Buffer, command.bufferId.slotIndex, operation);
  if (bufferId.hasError()) {
    return std::move(bufferId).error();
  }
  Result<uint32_t> format =
      RequireCode(WireIndexFormat(command.format), "SetIndexBufferCommand.format", operation);
  if (format.hasError()) {
    return std::move(format).error();
  }
  return StatusForBridge(
      bridge_->setIndexBuffer(bufferId.result(), format.result(), command.offsetBytes), operation);
}

Status BrowserDevice::replay(const SetScissorRectCommand& command, std::string_view operation) {
  return StatusForBridge(
      bridge_->setScissorRect(command.x, command.y, command.width, command.height), operation);
}

Status BrowserDevice::replay(const SetViewportCommand& command, std::string_view operation) {
  return StatusForBridge(bridge_->setViewport(command.x, command.y, command.width, command.height,
                                              command.minDepth, command.maxDepth),
                         operation);
}

Status BrowserDevice::replay(const DrawCommand& command, std::string_view operation) {
  return StatusForBridge(bridge_->draw(command.vertexCount, command.instanceCount,
                                       command.firstVertex, command.firstInstance),
                         operation);
}

Status BrowserDevice::replay(const DrawIndexedCommand& command, std::string_view operation) {
  return StatusForBridge(
      bridge_->drawIndexed(command.indexCount, command.instanceCount, command.firstIndex,
                           command.baseVertex, command.firstInstance),
      operation);
}

Status BrowserDevice::replay(const EndRenderPassCommand& /*command*/, std::string_view operation) {
  return StatusForBridge(bridge_->endRenderPass(), operation);
}

Status BrowserDevice::replay(const BeginComputePassCommand& /*command*/,
                             std::string_view operation) {
  return StatusForBridge(bridge_->beginComputePass(), operation);
}

Status BrowserDevice::replay(const SetComputePipelineCommand& command, std::string_view operation) {
  Result<BrowserObjectId> pipelineId =
      objectFor(BrowserObjectKind::ComputePipeline, command.pipelineId.slotIndex, operation);
  if (pipelineId.hasError()) {
    return std::move(pipelineId).error();
  }
  return StatusForBridge(bridge_->setComputePipeline(pipelineId.result()), operation);
}

Status BrowserDevice::replay(const DispatchWorkgroupsCommand& command, std::string_view operation) {
  return StatusForBridge(
      bridge_->dispatchWorkgroups(command.workgroupCountX, command.workgroupCountY,
                                  command.workgroupCountZ),
      operation);
}

Status BrowserDevice::replay(const EndComputePassCommand& /*command*/, std::string_view operation) {
  return StatusForBridge(bridge_->endComputePass(), operation);
}

Status BrowserDevice::replay(const CopyTextureToBufferCommand& command,
                             std::string_view operation) {
  Result<BrowserObjectId> textureId =
      objectFor(BrowserObjectKind::Texture, command.textureId.slotIndex, operation);
  if (textureId.hasError()) {
    return std::move(textureId).error();
  }
  Result<BrowserObjectId> bufferId =
      objectFor(BrowserObjectKind::Buffer, command.bufferId.slotIndex, operation);
  if (bufferId.hasError()) {
    return std::move(bufferId).error();
  }

  const BrowserTexelLayout layout{command.layout.offsetBytes, command.layout.bytesPerRow,
                                  command.layout.rowsPerImage};
  BrowserCopyRegion region;
  region.width = command.copySize.width;
  region.height = command.copySize.height;
  return StatusForBridge(
      bridge_->copyTextureToBuffer(textureId.result(), bufferId.result(), layout, region),
      operation);
}

Status BrowserDevice::replay(const CopyTextureToTextureCommand& command,
                             std::string_view operation) {
  Result<BrowserObjectId> sourceId =
      objectFor(BrowserObjectKind::Texture, command.textureSrcId.slotIndex, operation);
  if (sourceId.hasError()) {
    return std::move(sourceId).error();
  }
  Result<BrowserObjectId> destinationId =
      objectFor(BrowserObjectKind::Texture, command.textureDstId.slotIndex, operation);
  if (destinationId.hasError()) {
    return std::move(destinationId).error();
  }

  BrowserCopyRegion region;
  region.sourceX = command.sourceOrigin.x;
  region.sourceY = command.sourceOrigin.y;
  region.destinationX = command.destinationOrigin.x;
  region.destinationY = command.destinationOrigin.y;
  region.width = command.copySize.width;
  region.height = command.copySize.height;
  return StatusForBridge(
      bridge_->copyTextureToTexture(sourceId.result(), destinationId.result(), region), operation);
}

Status BrowserDevice::replayCommand(const Command& command, std::string_view operation) {
  return std::visit([&](const auto& typed) { return replay(typed, operation); }, command);
}

Status BrowserDevice::onSubmit(uint64_t submissionSerial, uint32_t /*commandBufferSlotIndex*/,
                               std::span<const Command> commands) {
  static constexpr std::string_view kOperation = "submit";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  if (Status status = StatusForBridge(bridge_->beginCommandBuffer(submissionSerial), kOperation);
      status.hasError()) {
    return status;
  }
  for (const Command& command : commands) {
    // A refused command leaves the browser-side recording open and unsubmitted; the next
    // beginCommandBuffer discards it, so nothing recorded before the refusal reaches the queue.
    if (Status status = replayCommand(command, kOperation); status.hasError()) {
      return status;
    }
  }
  return StatusForBridge(bridge_->endCommandBuffer(submissionSerial), kOperation);
}

Status BrowserDevice::onMapBufferAsync(uint32_t mappingSlotIndex, uint32_t bufferSlotIndex,
                                       MapMode mode, uint64_t offsetBytes, uint64_t byteCount) {
  static constexpr std::string_view kOperation = "mapBufferAsync";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  if (mode != MapMode::Read) {
    // Only host reads cross this bridge. A mode added later would otherwise be mapped as a read,
    // which is the wrong access for whatever it turns out to mean.
    return Err(GpuErrorType::Unsupported,
               std::format("{}: the browser bridge maps buffers for host reads only", kOperation));
  }
  Result<BrowserObjectId> bufferId =
      objectFor(BrowserObjectKind::Buffer, bufferSlotIndex, kOperation);
  if (bufferId.hasError()) {
    return std::move(bufferId).error();
  }
  Result<BrowserObjectId> mappingId =
      registerObject(BrowserObjectKind::BufferMapping, mappingSlotIndex, kOperation);
  if (mappingId.hasError()) {
    return std::move(mappingId).error();
  }

  const BridgeStatus status =
      bridge_->mapBufferAsync(mappingId.result(), bufferId.result(), offsetBytes, byteCount);
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::BufferMapping, mappingSlotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

MapSliceState BrowserDevice::onWaitMappingSlice(uint32_t mappingSlotIndex, double sliceSeconds) {
  if (bridge_->isDeviceLost()) {
    return MapSliceState::DeviceLost;
  }
  const std::optional<BrowserObjectId> mappingId =
      objects_.find(BrowserObjectKind::BufferMapping, mappingSlotIndex);
  if (!mappingId.has_value()) {
    return MapSliceState::Failed;
  }

  if (const MapSliceState state = bridge_->mappingState(*mappingId);
      state != MapSliceState::Pending) {
    return state;
  }

  if (yielding_) {
    // Entered from inside this device's own yield. Handing the thread over again would start a
    // second stack unwind on top of the first, which the runtime underneath cannot represent, so
    // the nested wait is refused instead of taken.
    ++nestedWaitRefusals_;
    return MapSliceState::Failed;
  }

  // Still pending, so spend the slice giving the browser the thread rather than returning at once.
  // What settles a mapping is a promise callback, and that cannot run while this thread holds the
  // event loop; a slice spent resting instead of yielding would let the whole budget elapse with
  // the browser never getting the chance to finish the mapping it was asked for.
  yielding_ = true;
  bridge_->yieldToBrowser(ClampYieldSeconds(sliceSeconds, kMaxYieldSeconds));
  yielding_ = false;

  // Anything may have happened while the browser had the thread, including this mapping being
  // released, so the identifier is looked up again rather than reused: the one from before the
  // yield could now name nothing.
  const std::optional<BrowserObjectId> afterYield =
      objects_.find(BrowserObjectKind::BufferMapping, mappingSlotIndex);
  if (!afterYield.has_value()) {
    return MapSliceState::Failed;
  }
  return bridge_->mappingState(*afterYield);
}

Result<std::span<const uint8_t>> BrowserDevice::onMappedBytes(uint32_t mappingSlotIndex) const {
  static constexpr std::string_view kOperation = "mappedBytes";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return std::move(status).error();
  }
  Result<BrowserObjectId> mappingId =
      objectFor(BrowserObjectKind::BufferMapping, mappingSlotIndex, kOperation);
  if (mappingId.hasError()) {
    return std::move(mappingId).error();
  }

  std::span<const uint8_t> bytes;
  const BridgeStatus status = bridge_->mappedBytes(mappingId.result(), bytes);
  if (status != BridgeStatus::Success) {
    return ErrorForBridgeStatus(status, kOperation);
  }
  return bytes;
}

void BrowserDevice::onUnmapBuffer(uint32_t mappingSlotIndex) {
  if (!onOwnerThread()) {
    // Same reasoning as \ref releaseObject: the mapping belongs to the owning context, so it is
    // kept for teardown there rather than named to a worker that does not hold it.
    foreignThreadReleases_.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const std::optional<BrowserObjectId> mappingId =
      objects_.remove(BrowserObjectKind::BufferMapping, mappingSlotIndex);
  if (mappingId.has_value()) {
    bridge_->unmapBuffer(*mappingId);
  }
}

Status BrowserDevice::onCreateSurface(uint32_t slotIndex, const SurfaceDescriptor& descriptor) {
  static constexpr std::string_view kOperation = "createSurface";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  if (descriptor.native.kind != NativeSurfaceKind::CanvasSelector) {
    return Err(GpuErrorType::Unsupported,
               std::format("{}: the browser bridge presents to a canvas named by a CSS selector, "
                           "and this surface names a different kind of platform object",
                           kOperation));
  }

  Result<BrowserObjectId> id = registerObject(BrowserObjectKind::Surface, slotIndex, kOperation);
  if (id.hasError()) {
    return std::move(id).error();
  }

  const BridgeStatus status = bridge_->createSurface(id.result(), descriptor.native.selector.str());
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::Surface, slotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  return OkStatus();
}

Result<SurfaceCapabilities> BrowserDevice::onSurfaceCapabilities(uint32_t slotIndex) const {
  static constexpr std::string_view kOperation = "surfaceCapabilities";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return std::move(status).error();
  }
  Result<BrowserObjectId> surfaceId = objectFor(BrowserObjectKind::Surface, slotIndex, kOperation);
  if (surfaceId.hasError()) {
    return std::move(surfaceId).error();
  }

  BrowserSurfaceCapabilities reported;
  const BridgeStatus status = bridge_->surfaceCapabilities(surfaceId.result(), reported);
  if (status != BridgeStatus::Success) {
    return ErrorForBridgeStatus(status, kOperation);
  }
  return DecodeSurfaceCapabilities(reported);
}

Status BrowserDevice::onConfigureSurface(uint32_t slotIndex,
                                         const SurfaceConfiguration& configuration) {
  static constexpr std::string_view kOperation = "configureSurface";
  if (Status status = checkUsable(kOperation); status.hasError()) {
    return status;
  }
  Result<BrowserObjectId> surfaceId = objectFor(BrowserObjectKind::Surface, slotIndex, kOperation);
  if (surfaceId.hasError()) {
    return std::move(surfaceId).error();
  }
  Result<uint32_t> format = RequireCode(WireTextureFormat(configuration.format),
                                        "SurfaceConfiguration.format", kOperation);
  if (format.hasError()) {
    return std::move(format).error();
  }
  Result<uint32_t> usage =
      RequireCode(WireTextureUsage(configuration.usage), "SurfaceConfiguration.usage", kOperation);
  if (usage.hasError()) {
    return std::move(usage).error();
  }
  Result<uint32_t> alphaMode = RequireCode(WireSurfaceAlphaMode(configuration.alphaMode),
                                           "SurfaceConfiguration.alphaMode", kOperation);
  if (alphaMode.hasError()) {
    return std::move(alphaMode).error();
  }

  return StatusForBridge(bridge_->configureSurface(surfaceId.result(), format.result(),
                                                   usage.result(), configuration.size.width,
                                                   configuration.size.height, alphaMode.result()),
                         kOperation);
}

Result<SurfaceStatus> BrowserDevice::onAcquireCurrentTexture(uint32_t slotIndex,
                                                             uint32_t textureSlotIndex) {
  static constexpr std::string_view kOperation = "acquireCurrentTexture";
  if (Status usable = checkUsable(kOperation); usable.hasError()) {
    // A lost device has no frame to give, and the runtime's own status vocabulary says so
    // precisely, so the caller learns it without having to read an error message.
    if (bridge_->isDeviceLost()) {
      return SurfaceStatus::DeviceLost;
    }
    return std::move(usable).error();
  }
  Result<BrowserObjectId> surfaceId = objectFor(BrowserObjectKind::Surface, slotIndex, kOperation);
  if (surfaceId.hasError()) {
    return std::move(surfaceId).error();
  }
  Result<BrowserObjectId> textureId =
      registerObject(BrowserObjectKind::Texture, textureSlotIndex, kOperation);
  if (textureId.hasError()) {
    return std::move(textureId).error();
  }

  SurfaceStatus surfaceStatus = SurfaceStatus::Success;
  const BridgeStatus status =
      bridge_->acquireCurrentTexture(surfaceId.result(), textureId.result(), surfaceStatus);
  if (status != BridgeStatus::Success) {
    objects_.remove(BrowserObjectKind::Texture, textureSlotIndex);
    return ErrorForBridgeStatus(status, kOperation);
  }
  if (surfaceStatus == SurfaceStatus::Lost || surfaceStatus == SurfaceStatus::DeviceLost ||
      surfaceStatus == SurfaceStatus::Timeout) {
    // The runtime releases the texture slot it speculatively allocated for these outcomes, so the
    // identifier minted for it must go with it rather than outliving the slot it names.
    releaseObject(BrowserObjectKind::Texture, textureSlotIndex);
    return surfaceStatus;
  }

  setAcquiredTexture(slotIndex, textureSlotIndex);
  return surfaceStatus;
}

Result<SurfaceStatus> BrowserDevice::onPresentSurface(uint32_t slotIndex) {
  // The runtime invalidates the acquired texture whether or not presenting was performed, so the
  // frame is handed back here too rather than left named until the slot is reused.
  releaseAcquiredFrame(slotIndex);
  return GpuError{GpuErrorType::Unsupported,
                  "presentSurface: a browser shows a canvas on its own frame loop, so a frame "
                  "ends by abandoning its acquired texture rather than by presenting it"};
}

void BrowserDevice::releaseAcquiredFrame(uint32_t surfaceSlotIndex) {
  const std::optional<BrowserObjectId> surfaceId =
      objects_.find(BrowserObjectKind::Surface, surfaceSlotIndex);
  if (surfaceId.has_value()) {
    bridge_->abandonCurrentTexture(*surfaceId);
  }

  // Only the identifier is dropped here. A frame texture belongs to the surface that handed it
  // over, so destroying it would take away the canvas's own texture rather than release something
  // this device owns; the surface let go of it in the call above.
  const uint32_t textureSlotIndex = acquiredTexture(surfaceSlotIndex);
  if (textureSlotIndex != kNoAcquiredTexture) {
    objects_.remove(BrowserObjectKind::Texture, textureSlotIndex);
    setAcquiredTexture(surfaceSlotIndex, kNoAcquiredTexture);
  }
}

void BrowserDevice::onAbandonCurrentTexture(uint32_t slotIndex) {
  releaseAcquiredFrame(slotIndex);
}

}  // namespace donner::gpu::browser
