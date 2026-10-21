#pragma once
/// @file
#include "donner/svg/SVGDocument.h"
namespace donner::svg::test {
/// Change one source-recovery policy dimension without invoking a resource callback.
/// @param document Document whose live policy changes.
/// @param mode Loader, mode, callback, source, payload or family cap selector.
/// @param callbacks Counter for any unexpected external callback.
void ChangeSourceRecoveryPolicy(SVGDocument& document, int mode, int& callbacks);
}  // namespace donner::svg::test
