#include "../internal.h"

namespace sunrise::steam::interfaces::methods {
namespace {

/** An empty country token turns region filtering off. */
constexpr char kCountry[] = "";

} // namespace

/** @return Empty token, so region filtering stays off. */
const char* country([[maybe_unused]] void* self) noexcept {
    return kCountry;
}

/** @return Active application id. */
DWORD get_app_id([[maybe_unused]] void* self) noexcept {
    return app_id();
}

} // namespace sunrise::steam::interfaces::methods
