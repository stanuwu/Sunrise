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
void dismantle_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept;
void acquire_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept;

/**
 * Decodes one opcode-1801 Triumphs claim request and reports the record it names.
 *
 * The request carries a record row index and nothing else. No state transition is prepared yet:
 * the state separating a claimed record from a merely completed one is not identified, since every
 * record completion flag can be set while the client still offers the claim. This is the seam that
 * transition attaches to once that state is known.
 *
 * @param message Parsed Web Service envelope.
 * @param outcome Left untouched, so the shared reply path still answers the claim with success.
 */
void claim_record(const middleware::web_service::Message& message, Outcome& outcome) noexcept;

} // namespace sunrise::server::web_service
