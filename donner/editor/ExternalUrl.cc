#include "donner/editor/ExternalUrlLauncher.h"

namespace donner::editor {

std::string_view ExternalUrlValue(ExternalUrlTarget target) {
  switch (target) {
    case ExternalUrlTarget::DonnerRepository: return "https://github.com/jwmcglynn/donner";
  }

  return {};
}

}  // namespace donner::editor
