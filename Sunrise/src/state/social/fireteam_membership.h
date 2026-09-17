#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../core/network_capacity.h"

namespace sunrise::state::social::fireteams {

inline constexpr std::size_t kEdgeCapacity =
    core::network_capacity::kPlayers * (core::network_capacity::kPlayers - 1) / 2;

/** A lookup records intent. Only native admission can establish the relationship. */
struct Edge final {
    std::uint64_t joiner{};
    std::uint64_t target{};
    bool established{};
    bool operator==(const Edge&) const noexcept = default;
};

/** Caller owns synchronization and account corroboration; this object owns no player identity. */
class Membership final {
public:
    /** One outstanding lookup per joiner; a later one supersedes it. Refuses a full table. */
    [[nodiscard]] bool request(std::uint64_t joiner, std::uint64_t target) noexcept;
    /** Called only when the corresponding native reservation commits. */
    [[nodiscard]] bool establish(std::uint64_t joiner, std::uint64_t target) noexcept;
    /** Pending lookup paths never appear in this membership query. */
    [[nodiscard]] bool connected(std::uint64_t first, std::uint64_t second) const noexcept;
    /** Minimum account key of an established component, or zero for an unconnected account. */
    [[nodiscard]] std::uint64_t representative(std::uint64_t account) const noexcept;
    /** Explicit fireteam departure; ordinary activity travel does not call this. */
    [[nodiscard]] bool depart(std::uint64_t account) noexcept;
    /** Explicit native release for one relation. */
    [[nodiscard]] bool release(std::uint64_t first, std::uint64_t second) noexcept;
    /** Moved only by admission, release and departure; recorded intent is not a membership fact. */
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }
    [[nodiscard]] std::span<const Edge> edges() const noexcept {
        return edges_;
    }

private:
    [[nodiscard]] std::size_t
    component(std::uint64_t account,
              std::array<std::uint64_t, kEdgeCapacity + 1>& members) const noexcept;
    std::array<Edge, kEdgeCapacity> edges_{};
    std::uint64_t revision_{1};
};
} // namespace sunrise::state::social::fireteams
