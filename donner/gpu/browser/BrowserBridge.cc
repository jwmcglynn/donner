#include "donner/gpu/browser/BrowserBridge.h"

namespace donner::gpu::browser {

BrowserBridge::BrowserBridge() = default;

BrowserBridge::~BrowserBridge() = default;

std::ostream& operator<<(std::ostream& os, BridgeStatus value) {
  switch (value) {
    case BridgeStatus::Success: return os << "Success";
    case BridgeStatus::UnknownObject: return os << "UnknownObject";
    case BridgeStatus::WrongObjectKind: return os << "WrongObjectKind";
    case BridgeStatus::NotOwner: return os << "NotOwner";
    case BridgeStatus::DeviceLost: return os << "DeviceLost";
    case BridgeStatus::Failed: return os << "Failed";
  }
  return os << "BridgeStatus(" << static_cast<int>(value) << ")";
}

std::ostream& operator<<(std::ostream& os, BrowserDeviceRequestState value) {
  switch (value) {
    case BrowserDeviceRequestState::Pending: return os << "Pending";
    case BrowserDeviceRequestState::Ready: return os << "Ready";
    case BrowserDeviceRequestState::Unavailable: return os << "Unavailable";
    case BrowserDeviceRequestState::Failed: return os << "Failed";
  }
  return os << "BrowserDeviceRequestState(" << static_cast<int>(value) << ")";
}

std::ostream& operator<<(std::ostream& os, BrowserBindingResource value) {
  switch (value) {
    case BrowserBindingResource::Buffer: return os << "Buffer";
    case BrowserBindingResource::TextureView: return os << "TextureView";
    case BrowserBindingResource::Sampler: return os << "Sampler";
  }
  return os << "BrowserBindingResource(" << static_cast<int>(value) << ")";
}

}  // namespace donner::gpu::browser
