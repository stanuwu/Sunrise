#pragma once

#include <algorithm>
#include <array>
#include <span>

#include "../../middleware/gameplay/descriptor/net_addr.h"
#include "../social/native_presence.h"

namespace sunrise::state::network {
/**
 * The native join descriptor names the fireteam host, not necessarily its publisher.
 * Retain the publisher's own solo endpoint while it joins someone else's fireteam.
 * This is connection-local routing authorization, never a replacement native descriptor.
 */
struct PeerPublication {
    using Endpoint = middleware::gameplay::descriptor::PeerEndpoint;
    std::uint64_t character{};
    std::array<Endpoint, middleware::gameplay::descriptor::kPeerEndpointCount> endpoints{};
    std::size_t count{};

    void observe(const social::NativePresence& native) noexcept {
        namespace descriptor = middleware::gameplay::descriptor;
        if (!native.published || character != native.characterSoid) {
            *this = {};
            character = native.characterSoid;
        }
        if (!native.published || !character || !native.hasGroup || native.memberCount != 1
            || native.descriptorSize != descriptor::kDescriptorSize)
            return;
        std::array<std::byte, descriptor::kNetAddrSize> address{};
        std::copy_n(native.descriptor.begin() + 8, address.size(), address.begin());
        std::array<Endpoint, descriptor::kPeerEndpointCount> candidate{};
        const auto size = descriptor::net_addr_endpoints(address, candidate);
        if (!size) return;
        endpoints = candidate;
        count = size;
    }

    [[nodiscard]] std::size_t snapshot(std::span<Endpoint> output) const noexcept {
        if (output.size() < count) return 0;
        std::copy_n(endpoints.begin(), count, output.begin());
        return count;
    }
};
} // namespace sunrise::state::network
