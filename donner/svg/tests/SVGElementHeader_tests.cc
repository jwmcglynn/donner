#include "donner/svg/SVGElement.h"

namespace donner::svg {
namespace {

template <typename T>
concept CompleteType = requires { sizeof(T); };

static_assert(!CompleteType<PropertyRegistry>,
              "SVGElement must not expose the complete property registry through its header.");

}  // namespace
}  // namespace donner::svg
