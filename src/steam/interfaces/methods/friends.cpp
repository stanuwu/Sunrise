#include "../../../core/settings/settings.h"
#include "../internal.h"

namespace sunrise::steam::interfaces::methods {

/** @return Persona name from settings. It lasts for the whole process. */
const char* persona_name([[maybe_unused]] void* self) noexcept {
    return core::settings::get().steam.user.personaName.data();
}

} // namespace sunrise::steam::interfaces::methods
