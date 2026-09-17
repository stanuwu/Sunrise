#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../core/network_capacity.h"

namespace sunrise::state::social::lobby {

// Platform-owned Steam lobby traffic. These records never enter a game protocol.
inline constexpr std::size_t kPayloadCapacity = 4096;
inline constexpr std::size_t kLobbyCapacity = 16;
inline constexpr std::size_t kBatchCapacity = 2;
inline constexpr std::size_t kQueueCapacity = 16;
inline constexpr std::size_t kAccountCapacity = core::network_capacity::kPlayers + 1;
inline constexpr std::uint64_t kLobbyPrefix = 0x0184000000000000ULL;
inline constexpr std::uint64_t kLobbyRandomMask = 0x0001FFFFFFFFFFFFULL;

[[nodiscard]] constexpr bool valid_id(std::uint64_t id) noexcept {
    return (id >> 56) == 1 && ((id >> 52) & 15) == 8 && ((id >> 32) & 0x40000) != 0
           && (id & 0xFFFFFFFFULL) != 0;
}

struct Message {
    std::uint64_t sequence{};
    std::uint64_t lobby{};
    std::uint64_t sender{};
    std::uint16_t size{};
    std::array<std::byte, kPayloadCapacity> body{};
};

struct Request {
    std::uint64_t epoch{};
    std::uint64_t membershipRevision{};
    std::uint64_t acceptedThrough{};
    std::uint64_t receivedThrough{};
    std::array<std::uint64_t, kLobbyCapacity> memberships{};
    std::size_t membershipCount{};
    std::array<Message, kBatchCapacity> messages{};
    std::size_t messageCount{};
};

struct Reply {
    std::uint64_t epoch{};
    std::uint64_t membershipRevision{};
    std::uint64_t acceptedThrough{};
    std::uint64_t receivedThrough{};
    std::array<Message, kBatchCapacity> messages{};
    std::size_t messageCount{};
};

class Hub {
public:
    void sync(std::size_t account, std::uint64_t sender, const Request& request) noexcept;
    void feed(std::size_t account, Reply& reply) const noexcept;
    void disconnect(std::size_t account) noexcept;
    void forget(std::size_t account) noexcept;
    void reset() noexcept;
    /** Advances whenever this account's reply body would change. */
    [[nodiscard]] std::uint64_t publication(std::size_t account) const noexcept;

private:
    struct Account {
        std::uint64_t publication{};
        std::uint64_t epoch{};
        std::uint64_t membershipRevision{};
        std::uint64_t acceptedThrough{};
        std::uint64_t receivedThrough{};
        std::uint64_t nextDelivery{1};
        std::array<std::uint64_t, kLobbyCapacity> memberships{};
        std::size_t membershipCount{};
        std::array<Message, kQueueCapacity> pending{};
        std::size_t pendingCount{};
    };
    std::array<Account, kAccountCapacity> accounts_{};
};

/** Local Steam client's queues. Delivery is acknowledged only after callback enqueue succeeds. */
class Client {
public:
    void initialize(std::uint64_t epoch) noexcept;
    [[nodiscard]] bool join(std::uint64_t id) noexcept;
    void leave(std::uint64_t id) noexcept;
    [[nodiscard]] bool contains(std::uint64_t id) const noexcept;
    [[nodiscard]] bool send(std::uint64_t id, std::span<const std::byte> bytes) noexcept;
    /** Also stages how far the outgoing batch is being offered; only a reply commits that mark. */
    void snapshot(Request& request) noexcept;
    void receive(const Reply& reply) noexcept;
    [[nodiscard]] bool pending(Message& message) const noexcept;
    void delivered(std::uint64_t sequence) noexcept;
    [[nodiscard]] int read(std::uint64_t id,
                           int index,
                           std::span<std::byte> output,
                           std::uint64_t* sender) const noexcept;
    [[nodiscard]] bool dirty() const noexcept;

private:
    Request request_{};
    std::uint64_t membershipAck_{};
    std::uint64_t receiptSent_{};
    /** Outgoing sequence the last accepted reply answered for, and the one being offered now. */
    std::uint64_t offeredThrough_{};
    std::uint64_t stagedThrough_{};
    std::uint64_t nextSequence_{1};
    std::array<Message, kQueueCapacity> outgoing_{};
    std::size_t outgoingCount_{};
    std::array<Message, kQueueCapacity> incoming_{};
    std::size_t incomingCount_{};
    std::array<Message, 64> history_{};
    std::size_t historyNext_{};
};

// Production instances are protected by an independent mutex, not the game State lock.
[[nodiscard]] bool join(std::uint64_t id) noexcept;
[[nodiscard]] bool contains(std::uint64_t id) noexcept;
void leave(std::uint64_t id) noexcept;
[[nodiscard]] bool send(std::uint64_t id, std::span<const std::byte> bytes) noexcept;
void snapshot(Request& request) noexcept;
void receive(const Reply& reply) noexcept;
[[nodiscard]] bool pending(Message& message) noexcept;
void delivered(std::uint64_t sequence) noexcept;
[[nodiscard]] int
read(std::uint64_t id, int index, std::span<std::byte> output, std::uint64_t* sender) noexcept;
[[nodiscard]] bool dirty() noexcept;
void sync(std::size_t account, std::uint64_t sender, const Request& request) noexcept;
void feed(std::size_t account, Reply& reply) noexcept;
void disconnect(std::size_t account) noexcept;
void reset() noexcept;

} // namespace sunrise::state::social::lobby
