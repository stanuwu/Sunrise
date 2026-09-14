#include "startup_reservations.h"

namespace sunrise::middleware::bap::activity_host_manager::request::selection {
bool read_startup_reservations(encoding::bits::Reader& reader,
                               StartupReservations& output) noexcept {
    output = {};
    // Native rows five/six are fixed arrays. They are skipped in place and never retained.
    constexpr std::size_t before = 2048 + 8192;
    constexpr std::size_t after = 64 + 64 + 32 + 10;
    std::uint64_t count{};
    if (!reader.skip(before) || !reader.read(6, count)
        || count > activity_message::telemetry::kPeerRecordCapacity) {
        return false;
    }
    StartupReservations parsed{};
    for (std::size_t i = 0; i < count; ++i) {
        // The same native BC schema as message 13: adjacent 362-bit identities, no row padding.
        if (!activity_message::telemetry::read_reservation_identity(reader, parsed.identities[i])) {
            return false;
        }
    }
    if (!reader.skip(after) || reader.remaining_bits() > 7) {
        return false;
    }
    std::uint64_t padding{};
    if (!reader.read(static_cast<std::uint8_t>(reader.remaining_bits()), padding) || padding) {
        return false;
    }
    parsed.count = static_cast<std::uint8_t>(count);
    parsed.valid = true;
    output = parsed;
    return true;
}
} // namespace sunrise::middleware::bap::activity_host_manager::request::selection
