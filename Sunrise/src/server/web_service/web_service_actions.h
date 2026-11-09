#pragma once

#include <cstdint>

#include "../../middleware/web_service/web_service_envelope.h"
#include "web_service_runtime.h"

namespace sunrise::server::web_service {

/** Prepares an installed vendor's identity-preserving Postmaster claim. */
void claim_postmaster_item(const middleware::web_service::Message& message,
                           Outcome& outcome) noexcept;

void select_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept;
/** Saves the character an opcode-501 request describes, with its starter set. */
void create_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept;
/** Removes the character an opcode-502 request names. */
void delete_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept;
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

/**
 * Draws the seed, prepares the grant and stages the claim flag of one pass row, as an
 * opcode-2400 claim does once its request is decoded. Call inside an investment transaction.
 * @param reason Receives a static label on failure.
 */
[[nodiscard]] bool prepare_season_pass_claim(std::uint16_t rewardIndex,
                                             state::PendingSeasonPassReward& claim,
                                             const char*& reason) noexcept;

/** Decodes and applies one opcode-1821 earned-title selection. */
void equip_title(const middleware::web_service::Message& message, Outcome& outcome) noexcept;

} // namespace sunrise::server::web_service
