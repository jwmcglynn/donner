#pragma once
/// @file

#include <memory>

#include "donner/svg/DocumentState.h"

namespace donner::svg {

/// Shared internal document state used by \ref SVGDocument's by-value facade.
using SVGDocumentHandle = std::shared_ptr<DocumentState>;

}  // namespace donner::svg
