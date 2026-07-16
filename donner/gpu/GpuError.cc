#include "donner/gpu/GpuError.h"

namespace donner::gpu {

std::string_view GpuError::TypeToString(GpuErrorType type) {
  switch (type) {
    case GpuErrorType::InvalidDescriptor: return "InvalidDescriptor";
    case GpuErrorType::InvalidHandle: return "InvalidHandle";
    case GpuErrorType::DeviceMismatch: return "DeviceMismatch";
    case GpuErrorType::UsageMismatch: return "UsageMismatch";
    case GpuErrorType::OutOfBounds: return "OutOfBounds";
    case GpuErrorType::LimitExceeded: return "LimitExceeded";
    case GpuErrorType::InvalidState: return "InvalidState";
    case GpuErrorType::Unsupported: return "Unsupported";
  }

  return "Unknown";
}

std::ostream& operator<<(std::ostream& os, GpuErrorType value) {
  return os << GpuError::TypeToString(value);
}

std::string GpuError::toString() const {
  std::string result(TypeToString(type));
  result += ": ";
  result += message;
  return result;
}

std::ostream& operator<<(std::ostream& os, const GpuError& error) {
  return os << error.type << ": " << error.message;
}

}  // namespace donner::gpu
