#pragma once

#include <cstddef>

#include "../../../../../middleware/bap/activity_message/definition.h"
#include "../../../../../state/activity/receipts/definition.h"

namespace sunrise::server::bap::encrypted::activity_message::receipts {

/** One message framed by the route, which changed no State. */
struct Framed {
    /** How completely the body was read. */
    state::activity::receipts::Verdict verdict{state::activity::receipts::Verdict::framed};
    /** Bits the parser consumed. Below the payload's own bit count means a tail was left. */
    std::size_t consumedBits{};
};

namespace message = middleware::bap::activity_message;

/** Frames a sensor sense update and reports its epoch. */
[[nodiscard]] Framed frame_sense_update(const message::Request& request) noexcept;

/** Records a service-8 envelope carrying the local-only activity-host request type. */
[[nodiscard]] Framed frame_route_misuse(const message::Request& request) noexcept;

/** Frames a start-new-activity request without applying any transition policy to it. */
[[nodiscard]] Framed frame_start_activity(const message::Request& request) noexcept;

/** Frames a peer-reservation request as far as its revision. */
[[nodiscard]] Framed frame_reservation_request(const message::Request& request) noexcept;

/** Frames a reservation release. */
[[nodiscard]] Framed frame_reservation_release(const message::Request& request) noexcept;

/** Frames a peer leave notice. */
[[nodiscard]] Framed frame_peer_leave(const message::Request& request) noexcept;

/** Records a debug command without reading or running it. */
[[nodiscard]] Framed frame_debug_command(const message::Request& request) noexcept;

/** Frames a connectivity failure report. */
[[nodiscard]] Framed frame_connectivity_failure(const message::Request& request) noexcept;

/** Records a client heartbeat as a bounded body. */
[[nodiscard]] Framed frame_heartbeat(const message::Request& request) noexcept;

/** Frames a lag-switch report as far as its record count. */
[[nodiscard]] Framed frame_lag_switch(const message::Request& request) noexcept;

/** Records a connection-quality report as a bounded body. */
[[nodiscard]] Framed frame_connection_quality(const message::Request& request) noexcept;

/** Frames a speculative migration proposal without acting on it. */
[[nodiscard]] Framed frame_migration(const message::Request& request) noexcept;

/** Frames the fixed high-water telemetry block. */
[[nodiscard]] Framed frame_high_water(const message::Request& request) noexcept;

/** Frames one of the two opaque scalar messages. */
[[nodiscard]] Framed frame_opaque_scalar(const message::Request& request) noexcept;

/** Frames the one-byte activity keepalive. */
[[nodiscard]] Framed frame_client_keepalive(const message::Request& request) noexcept;

/** Frames one incident and quarantines a poison target. */
[[nodiscard]] Framed frame_incident(const message::Request& request) noexcept;

/** Frames one authority release, which records authority and returns no lease. */
[[nodiscard]] Framed frame_authority_release(const message::Request& request,
                                             bool expectReason) noexcept;

/** Frames one purge request. Nothing answers it. */
[[nodiscard]] Framed frame_request_purge(const message::Request& request) noexcept;

/** Frames one authority query answer. */
[[nodiscard]] Framed frame_query_answer(const message::Request& request) noexcept;

/** Records an envelope whose message type has no recovered body grammar. */
[[nodiscard]] Framed frame_unknown(const message::Request& request) noexcept;

} // namespace sunrise::server::bap::encrypted::activity_message::receipts
