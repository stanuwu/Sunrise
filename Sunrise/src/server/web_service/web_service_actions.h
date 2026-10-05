#pragma once

#include <cstdint>

#include "../../middleware/web_service/web_service_envelope.h"
#include "web_service_runtime.h"

namespace sunrise::server::web_service {

/**
 * Stages local WS-702 presence using the scalar selected-character query and seen-item
 * receipt. The caller supplies a fresh outcome. Refusal stages no presence; the enclosing
 * BAP request publishes it only after committing its reply.
 */
[[nodiscard]] bool
note_character_writeback(const middleware::web_service::Message& message,
                         std::span<const state::account::inventory::PresentedItemRow> presentation,
                         Outcome& outcome) noexcept;

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
void purchase_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept;
void acquire_quest(const middleware::web_service::Message& message, Outcome& outcome) noexcept;

/** Persists an opcode-1801 Triumph claim and prepares its optional reward. */
void claim_record(const middleware::web_service::Message& message, Outcome& outcome) noexcept;

/** Decodes and prepares one opcode-2400 active-season reward claim. */
void claim_season_pass_reward(const middleware::web_service::Message& message,
                              Outcome& outcome) noexcept;

/** Decodes and applies one opcode-1821 earned-title selection. */
void equip_title(const middleware::web_service::Message& message, Outcome& outcome) noexcept;

} // namespace sunrise::server::web_service
