#pragma once
/// @file

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"

namespace donner::svg {

/**
 * Hold @p warnings found while preparing a sub-document without a collecting host sink, for the
 * next draw with one, skipping warnings already delivered or held, and warnings and truncation that
 * a host which already received the sub-document's truncation would drop. Held warnings found
 * before a mutation of the sub-document are dropped first. A batch that repeats the last one held
 * exactly is skipped without matching its warnings.
 *
 * @param registry The sub-document's registry, which holds the warnings.
 * @param warnings Warnings collected while preparing the sub-document.
 */
void HoldSubDocumentWarnings(Registry& registry, const ParseWarningSink& warnings);

/**
 * Before a draw with a collecting host sink prepares a sub-document, deliver the warnings it holds,
 * and restyle it on this preparation if a mutation dropped held warnings, so the preparation finds
 * its current ones.
 *
 * @param registry The sub-document's registry.
 * @param hostSink The host's collecting sink.
 */
void DeliverHeldSubDocumentWarnings(Registry& registry, ParseWarningSink& hostSink);

}  // namespace donner::svg
