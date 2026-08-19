#include "transform.h"

#include <cmath>

namespace sunrise::izanami::core {

namespace {

[[nodiscard]] bool finite(float value) noexcept {
    return std::isfinite(value);
}

} // namespace

/** @return True when every transform lane is finite. */
bool Transform::is_finite() const noexcept {
    return finite(translation.x) && finite(translation.y) && finite(translation.z) && finite(rotation.x)
           && finite(rotation.y) && finite(rotation.z) && finite(rotation.w) && finite(uniformScale);
}

} // namespace sunrise::izanami::core