#pragma once
/// @file
/// Renderer-internal delivery of the warnings an external sub-document's preparation finds.

#include "donner/base/EcsRegistry.h"
#include "donner/base/ParseWarningSink.h"

namespace donner::svg::details {

/**
 * Drop held warnings found before the sub-document was last mutated, and mark it so the next
 * collecting draw restyles it to find its current warnings. A draw that does not collect never
 * restyles for this. A document-level warning reported once per document is let through again,
 * since the copy that reported it is dropped.
 *
 * @param registry The sub-document's registry.
 */
void DropStaleSubDocumentWarnings(Registry& registry);

/**
 * Deliver @p warnings from preparing a sub-document to @p hostSink, skipping warnings already
 * delivered for that sub-document and marking @p hostSink truncated when any were dropped.
 *
 * @param registry The sub-document's registry, which records the delivered warnings.
 * @param warnings Warnings collected while preparing the sub-document.
 * @param hostSink The host's collecting sink.
 */
void ReportSubDocumentWarnings(Registry& registry, const ParseWarningSink& warnings,
                               ParseWarningSink& hostSink);

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

}  // namespace donner::svg::details
