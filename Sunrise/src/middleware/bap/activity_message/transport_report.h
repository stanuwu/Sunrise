#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::middleware::bap::activity_message {

/** Client-authored B2 transport fields. Presence distinguishes omission from withdrawal. */
struct TransportReport final {
    std::array<std::byte, 86> address{};
    std::array<std::byte, 86> alternate{};
    std::uint8_t flags{};
    bool hasFlags{};
    bool hasAddress{};
    bool hasAlternate{};
    bool operator==(const TransportReport&) const noexcept = default;
};

/** Applies only fields carried by this sparse native report, including explicit zero values. */
inline bool merge_transport(TransportReport& retained, const TransportReport& report) noexcept {
    const auto before = retained;
    if (report.hasFlags) {
        retained.flags = report.flags;
        retained.hasFlags = true;
    }
    if (report.hasAddress) {
        retained.address = report.address;
        retained.hasAddress = true;
    }
    if (report.hasAlternate) {
        retained.alternate = report.alternate;
        retained.hasAlternate = true;
    }
    return retained != before;
}

} // namespace sunrise::middleware::bap::activity_message
