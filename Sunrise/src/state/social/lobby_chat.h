#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../core/network_capacity.h"

namespace sunrise::state::social::lobby {

// Platform-owned Steam lobby traffic. These records never enter a game protocol.

/** The 4 KiB body Steam's lobby chat send documents. A longer send is refused, not truncated. */
inline constexpr std::size_t kPayloadCapacity = 4096;
/** Local policy: at most 128 bytes of lobby ids per account; further joins are refused. */
inline constexpr std::size_t kLobbyCapacity = 16;
/** Local wire budget: at most 8 KiB of chat payload per feed, plus metadata. Excess stays queued.
 */
inline constexpr std::size_t kBatchCapacity = 2;
/**
 * Local burst budget: 64 KiB of payload per queue, plus message metadata. A full sender queue
 * refuses sends; a full recipient queue keeps existing messages and skips new deliveries.
 */
inline constexpr std::size_t kQueueCapacity = 16;
/** Every player, plus one slot for the local installation's own account. */
inline constexpr std::size_t kAccountCapacity = core::network_capacity::kPlayers + 1;
/**
 * The fixed half of a Steam lobby id: universe 1 in bits 56-63, account type 8 (chat) in bits
 * 52-55, and the lobby flag inside the 20-bit instance field below them.
 */
inline constexpr std::uint64_t kLobbyPrefix = 0x0184000000000000ULL;
/** The bits a generated id may randomise, which is everything under that flag. */
inline constexpr std::uint64_t kLobbyRandomMask = 0x0001FFFFFFFFFFFFULL;

/** The same four fields, read back: public universe, chat type, lobby flag, nonzero account id. */
[[nodiscard]] constexpr bool valid_id(std::uint64_t id) noexcept {
    return (id >> 56) == 1 && ((id >> 52) & 15) == 8 && ((id >> 32) & 0x40000) != 0
           && (id & 0xFFFFFFFFULL) != 0;
}

/** One lobby chat message; `size` bounds the payload actually used in `body`. */
struct Message {
    std::uint64_t sequence{};
    std::uint64_t lobby{};
    std::uint64_t sender{};
    std::uint16_t size{};
    std::array<std::byte, kPayloadCapacity> body{};
};

/** The local client's staged sync: its memberships, receipt cursors, and outgoing batch. */
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

/** The host's reply: accepted cursors and the next batch of messages to deliver. */
struct Reply {
    std::uint64_t epoch{};
    std::uint64_t membershipRevision{};
    std::uint64_t acceptedThrough{};
    std::uint64_t receivedThrough{};
    std::array<Message, kBatchCapacity> messages{};
    std::size_t messageCount{};
};

/** Server-side chat state for every account. */
class Hub {
public:
    /**
     * Applies one account's staged request: admits its memberships, delivers batched messages
     * to every current member's queue, and advances that account's cursors. A refused request
     * (bad epoch, an oversized batch, or an invalid lobby id) changes nothing.
     */
    void sync(std::size_t account, std::uint64_t sender, const Request& request) noexcept;
    /** Builds the account's current reply: cursors and its next pending message batch. */
    void feed(std::size_t account, Reply& reply) const noexcept;
    /** Clears memberships and pending deliveries for a disconnected account. */
    void disconnect(std::size_t account) noexcept;
    /** Retires an offline account before its handle is reused. */
    void forget(std::size_t account) noexcept;
    /** Clears every account, as at a fresh boot. */
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
    /** Discards any prior registration and starts fresh under a new epoch. */
    void initialize(std::uint64_t epoch) noexcept;
    /** @return True once `id` is a joined membership; false when invalid or the table is full. */
    [[nodiscard]] bool join(std::uint64_t id) noexcept;
    /** Leaves a lobby, if currently a member. */
    void leave(std::uint64_t id) noexcept;
    /** @return True while `id` is a current membership. */
    [[nodiscard]] bool contains(std::uint64_t id) const noexcept;
    /**
     * Queues a message for a joined lobby.
     * @return False when not a member, empty, oversized, or the outgoing queue is full.
     */
    [[nodiscard]] bool send(std::uint64_t id, std::span<const std::byte> bytes) noexcept;
    /** Also stages how far the outgoing batch is being offered; only a reply commits that mark. */
    void snapshot(Request& request) noexcept;
    /**
     * Applies an accepted reply: advances cursors and admits new messages into the incoming
     * queue. A reply for the wrong epoch or an impossible cursor is ignored.
     */
    void receive(const Reply& reply) noexcept;
    /** @return True and copies the oldest undelivered incoming message, without dequeuing it. */
    [[nodiscard]] bool pending(Message& message) const noexcept;
    /** Confirms delivery of the oldest incoming message, moving it into `history_`. */
    void delivered(std::uint64_t sequence) noexcept;
    /**
     * @return Bytes copied from the delivered message named by `id` and sequence `index`, or
     * zero when no such message is retained.
     */
    [[nodiscard]] int read(std::uint64_t id,
                           int index,
                           std::span<std::byte> output,
                           std::uint64_t* sender) const noexcept;
    /** @return True while the host has not yet been shown this client's latest state. */
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
    /** Local read-history budget: 256 KiB of payload plus metadata; oldest entries are overwritten.
     */
    std::array<Message, 64> history_{};
    std::size_t historyNext_{};
};

// Production instances are protected by an independent mutex, not the game State lock.

/** Lazily initializes the local client's epoch on first use, then joins as `Client::join`. */
[[nodiscard]] bool join(std::uint64_t id) noexcept;
/** @return True while `id` is a current membership of the local client. */
[[nodiscard]] bool contains(std::uint64_t id) noexcept;
/** Leaves a lobby for the local client, if currently a member. */
void leave(std::uint64_t id) noexcept;
/** Queues a message for the local client, as `Client::send`. */
[[nodiscard]] bool send(std::uint64_t id, std::span<const std::byte> bytes) noexcept;
/** Stages the local client's outgoing sync request. */
void snapshot(Request& request) noexcept;
/** Applies an accepted reply to the local client. */
void receive(const Reply& reply) noexcept;
/**
 * @return True and copies the local client's oldest deliverable message. A queued message for
 * a lobby it has since left is confirmed and skipped instead.
 */
[[nodiscard]] bool pending(Message& message) noexcept;
/** Confirms delivery of the local client's oldest incoming message. */
void delivered(std::uint64_t sequence) noexcept;
/**
 * @return Bytes copied from the local client's retained message named by `id` and sequence
 * `index`, or zero when none is retained.
 */
[[nodiscard]] int
read(std::uint64_t id, int index, std::span<std::byte> output, std::uint64_t* sender) noexcept;
/** @return True while the host has not yet been shown the local client's latest state. */
[[nodiscard]] bool dirty() noexcept;
/** Applies one account's staged request to the session directory, as `Hub::sync`. */
void sync(std::size_t account, std::uint64_t sender, const Request& request) noexcept;
/** Builds one account's reply from the session directory, as `Hub::feed`. */
void feed(std::size_t account, Reply& reply) noexcept;
/** Clears one account's memberships and pending deliveries in the session directory. */
void disconnect(std::size_t account) noexcept;
/**
 * Clears the session directory; the local client's next `join` reinitializes it under a
 * fresh epoch.
 */
void reset() noexcept;

} // namespace sunrise::state::social::lobby
