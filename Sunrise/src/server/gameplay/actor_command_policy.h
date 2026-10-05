#pragma once

#include <cstdint>

#include "../../middleware/encoding/bit_reader.h"
#include "../../middleware/encoding/bit_writer.h"
#include "../../middleware/gameplay/external/external_entity_codec.h"
#include "../../middleware/gameplay/external/simulation_event_codec.h"
#include "../../middleware/gameplay/peer/established_packet.h"
#include "peer/peer_transport.h"

namespace sunrise::server::gameplay::actor_command_policy {

/** Installs the Mission ActorCommandPolicy callback and clears prior policy state. */
void initialize() noexcept;

/** Releases retained activity bindings and clears all bounded policy state. */
void shutdown() noexcept;

/** Advances replay barriers and removes policy rows whose exact bindings expired. */
void service(std::uint64_t now) noexcept;

/** Installs the session-scoped channel-2 baseline and actor registry transport. */
[[nodiscard]] bool install_entity_transport() noexcept;

/** @return The SDK-backed event payload codec installed by the peer transport root. */
[[nodiscard]] middleware::gameplay::external::SimulationEventPayloadCodec
lane0_payload_codec() noexcept;

/** @return Session-aware lane-0 callbacks bound to this policy runtime. */
[[nodiscard]] peer::Lane0Transport lane0_transport() noexcept;

/** Accepts one channel-2 batch only after the peer committed its external wrapper. */
[[nodiscard]] bool
accept_entity_batch(const state::gameplay::entity_identity::Source& source,
                    const middleware::gameplay::external::EntityBatch& batch) noexcept;

/** Decodes one lane-0 batch without assigning it to a peer session. */
[[nodiscard]] bool
decode_lane0(middleware::encoding::bits::Reader& reader,
             middleware::gameplay::external::SimulationEventBatch& output) noexcept;

/** Accepts one decoded lane-0 batch only after the peer committed its wrapper. */
[[nodiscard]] bool
accept_lane0(std::uint64_t groupSessionId,
             const middleware::gameplay::external::SimulationEventBatch& batch) noexcept;

/** Writes one session's queued lane-0 events and binds them to a transmission id. */
[[nodiscard]] bool write_lane0(std::uint64_t groupSessionId,
                               std::uint64_t transmissionId,
                               middleware::encoding::bits::Writer& writer) noexcept;

/** Commits or retries one exact session-scoped lane-0 transmission outcome. */
void lane0_outcome(std::uint64_t groupSessionId,
                   std::uint64_t transmissionId,
                   middleware::gameplay::peer::AckOutcome outcome) noexcept;

/** Clears one peer session without affecting another session's actor tokens or queue. */
void reset_group_session(std::uint64_t groupSessionId) noexcept;

} // namespace sunrise::server::gameplay::actor_command_policy
