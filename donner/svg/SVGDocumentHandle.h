#pragma once
/// @file

#include <memory>

#include "donner/base/EcsRegistry_fwd.h"

namespace donner::svg {

/// Shared internal document state used by \ref SVGDocument's by-value facade.
using SVGDocumentHandle = std::shared_ptr<Registry>;

}  // namespace donner::svg
