#pragma once

/// @file
/// Browser worker connection for GPU image presentation.

namespace donner::editor {

/** Register the current WebAssembly worker for direct image messages.
 * @param rasterWorker True for the raster pthread, false for the application pthread.
 */
void RegisterBrowserImageTransportWorker(bool rasterWorker);

}  // namespace donner::editor
