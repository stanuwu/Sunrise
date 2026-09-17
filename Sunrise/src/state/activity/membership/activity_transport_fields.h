#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "../../../core/network_capacity.h"
#include "../../../middleware/gameplay/descriptor/join_descriptor.h"

namespace sunrise::state::activity::membership {

/** A peer NetAddr is exactly 86 bytes wherever it appears. */
inline constexpr std::size_t kTransportAddressSize = middleware::gameplay::descriptor::kNetAddrSize;

/** The client sets this transport flag once its own carrier is ready. */
inline constexpr std::uint8_t kTransportReadyFlag = 0x10;

/** Each supported live connection may publish a distinct current or target member identity. */
inline constexpr std::size_t kTransportFieldCapacity = core::network_capacity::kConnections;

/** The carrier one client published about itself, exactly as it wrote it. */
struct TransportFields final {
    std::array<std::byte, kTransportAddressSize> address{};
    std::array<std::byte, kTransportAddressSize> addressAlt{};
    std::uint8_t flags{};
    bool hasFlags{};
    bool hasAddress{};
    bool hasAddressAlt{};
};

/**
 * Mirrors one connection's published carrier where every lane can read it.
 * The BAP connection owns these bytes; this table only republishes them under its own lock, so a
 * gameplay lane holding the admitted lock never has to reach into a BAP session to learn the one
 * address a peer must be named by.
 * @param memberKey Member key the peer is named by on every lane. Zero is refused.
 * @param accountSoid Owning account, so a lane holding only a soid can resolve the same bytes.
 * @param characterSoid Selected character the carrier belongs to.
 * @param fields Complete image selected from the live connections, replacing all retained fields.
 * @return True when the retained record changed.
 */
bool observe_transport_fields(std::uint64_t memberKey,
                              std::uint64_t accountSoid,
                              std::uint64_t characterSoid,
                              const TransportFields& fields) noexcept;

/** Withdraws one member's carrier when its connection's activity binding ends or restarts. */
void forget_transport_fields(std::uint64_t memberKey) noexcept;

/** Monotonic change cursor for carrier replacements, identity changes and withdrawals. */
[[nodiscard]] std::uint64_t transport_fields_generation() noexcept;

/**
 * Reads the carrier published for one exact member key.
 * @return True when a record exists and carries at least one address.
 */
[[nodiscard]] bool transport_fields_exact(std::uint64_t memberKey,
                                          TransportFields& fields) noexcept;

/**
 * Reads the carrier published for one account's selected character.
 * Two live records that disagree refuse rather than pick, so no lane can publish an address
 * another lane would contradict.
 * @param characterSoid Selected character, or zero to accept any character of that account.
 * @return True when exactly one carrier answers.
 */
[[nodiscard]] bool transport_fields_for_account(std::uint64_t accountSoid,
                                                std::uint64_t characterSoid,
                                                TransportFields& fields) noexcept;

} // namespace sunrise::state::activity::membership
