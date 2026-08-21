#pragma once

#include <cstdint>

#include "../../middleware/web_service/web_service_envelope.h"
#include "web_service_runtime.h"

namespace sunrise::server::web_service {

void select_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept;
void mutate_equipment(const middleware::web_service::Message& message,
                      bool unequip,
                      Outcome& outcome) noexcept;
void mutate_socket_plug(const middleware::web_service::Message& message, Outcome& outcome) noexcept;
void mutate_subclass_selection(const middleware::web_service::Message& message,
                               Outcome& outcome) noexcept;
void mutate_equipped_socket_plug(const middleware::web_service::Message& message,
                                 Outcome& outcome) noexcept;
void mutate_item_state(const middleware::web_service::Message& message, Outcome& outcome) noexcept;
/** Decodes and prepares one WS-701 settings update without publishing State. */
[[nodiscard]] state::SettingsUpdateDisposition
mutate_settings(const middleware::web_service::Message& message, Outcome& outcome) noexcept;
void dismantle_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept;
void acquire_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept;

} // namespace sunrise::server::web_service
