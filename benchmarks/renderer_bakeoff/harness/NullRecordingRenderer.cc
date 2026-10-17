#include "harness/NullRecordingRenderer.h"

namespace donner::bakeoff {

NullRecordingRenderer::NullRecordingRenderer()
    : AdapterBase(CapabilityTable::All(Support::Native)) {}

}  // namespace donner::bakeoff
