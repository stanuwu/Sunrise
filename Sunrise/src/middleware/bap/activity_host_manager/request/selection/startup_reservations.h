#pragma once

#include <array>

#include "../../../../encoding/bit_reader.h"
#include "../../../activity_message/telemetry.h"

namespace sunrise::middleware::bap::activity_host_manager::request::selection {

/** Credential-free native identities from a completely validated startup tail. */
struct StartupReservations final {
    std::array<activity_message::telemetry::ReservationRecord,
               activity_message::telemetry::kPeerRecordCapacity>
        identities{};
    std::uint8_t count{};
    bool valid{};
};

/** Reader is immediately after the current descriptor in service-six protobuf field two. */
[[nodiscard]] bool read_startup_reservations(encoding::bits::Reader& reader,
                                             StartupReservations& output) noexcept;
} // namespace sunrise::middleware::bap::activity_host_manager::request::selection
