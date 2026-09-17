#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../account/account_handle.h"
#include "social_feed.h"

namespace sunrise::state::social {

inline constexpr std::size_t kRosterCapacity = feed::kRowCapacity;
inline constexpr std::size_t kMailboxCapacity = feed::kInviteCapacity;
using Invite = feed::WireInvite;
using RosterEntry = feed::WireRow;

/**
 * Everything that would change one account's published feed body, as separate fields.
 * Separate rather than folded together: a single mixed counter can collide, and a collision here
 * is an invisible change rather than a late one.
 */
struct Stamp {
    /** Hub revision: the rows and links this account sees. */
    std::uint64_t directory{};
    /** This account's own publication counter: its mailbox, cursors and epoch. */
    std::uint64_t account{};
    /** Its lobby publication counter: memberships, chat cursors and queued messages. */
    std::uint64_t lobby{};
    /** Public route generation, supplied by the caller that owns the account projection. */
    std::uint32_t routes{};
    [[nodiscard]] bool operator==(const Stamp&) const noexcept = default;
};

/** Shared-session state. Only authenticated connection owners may call apply/publish. */
class Hub {
public:
    void opened(AccountHandle account) noexcept;
    void closed(AccountHandle account) noexcept;
    /** Retires an offline cached identity before its account handle is reused. */
    [[nodiscard]] bool forget(AccountHandle account) noexcept;
    [[nodiscard]] bool publish(AccountHandle account, const RosterEntry& row) noexcept;
    /** Applies one account's request. The mutating half; the caller stages the Hub as before. */
    [[nodiscard]] bool apply(AccountHandle account, const feed::Sync& request) noexcept;
    /** Builds the account's current feed. Read-only, so it needs no staging copy. */
    void publish(AccountHandle account, feed::Feed& output) const noexcept;
    /**
     * Change stamp for one account's published feed.
     * @param routeGeneration Public route generation read by the caller. This layer never reaches
     *        into the account projection, so the one route input the feed carries is passed in.
     */
    [[nodiscard]] Stamp stamp(AccountHandle account, std::uint32_t routeGeneration) const noexcept;
    /** Registers the one BAP connection that currently carries this account's social traffic. */
    void delivery(AccountHandle account, std::uint32_t connection, std::uint64_t serial) noexcept;
    [[nodiscard]] bool
    delivers(AccountHandle account, std::uint32_t connection, std::uint64_t serial) const noexcept;
    [[nodiscard]] std::size_t link_count(AccountHandle account) const noexcept;

private:
    struct Account {
        RosterEntry row{};
        std::size_t links{};
        bool syncing{};
        std::uint64_t epoch{};
        std::uint64_t acceptedThrough{};
        std::uint64_t receivedThrough{};
        std::uint64_t nextDelivery{1};
        /** Advanced by every mutation that changes this account's published feed body. */
        std::uint64_t publication{};
        /** Primary SOID of the target whose mailbox or registration blocked an accepted invite. */
        std::uint64_t waitingOn{};
        /** Connection that owns delivery, with a monotonic serial so a reused slot cannot alias. */
        std::uint32_t deliveryConnection{};
        std::uint64_t deliverySerial{};
        std::array<Invite, kMailboxCapacity> inbox{};
        std::size_t inboxCount{};
    };
    /** Republishes senders whose accepted invite this target's mailbox or absence had blocked. */
    void release_waiters(const Account& target) noexcept;
    std::array<Account, kRosterCapacity> accounts_{};
    std::uint64_t revision_{};
    lobby::Hub chat_{};
};

/** Platform-owned client queues. Network receipt is distinct from explicit invitation acceptance.
 */
class Client {
public:
    void initialize(std::uint64_t primarySoid, std::uint64_t epoch) noexcept;
    void disconnected() noexcept;
    [[nodiscard]] bool post(const Invite& invite) noexcept;
    /** Also stages how far the outgoing queue is being offered; only a feed commits that mark. */
    void snapshot(feed::Sync& output) noexcept;
    [[nodiscard]] bool receive(const feed::Feed& value) noexcept;
    [[nodiscard]] bool take(Invite& output) noexcept;
    [[nodiscard]] std::size_t peers(std::span<RosterEntry> output) const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }
    /**
     * Work the host has not been shown yet: an invitation past the offered mark, or a receipt the
     * host has not confirmed. An invitation the host has seen and refused for mailbox pressure is
     * not local work; the host's publication is what invites it back.
     */
    [[nodiscard]] bool pending_local_work() const noexcept {
        return (outgoingCount_ != 0 && outgoing_[outgoingCount_ - 1].sequence > offeredThrough_)
               || receivedThrough_ != receiptSent_
               || (incomingDeferred_ && incomingCount_ < incoming_.size());
    }
    /** Highest publication seen, from an accepted feed or a host notice. Never moves backwards. */
    [[nodiscard]] std::uint64_t known_publication() const noexcept {
        return knownPublication_;
    }
    void note_publication(std::uint64_t publication) noexcept;
    void reset_publication() noexcept {
        knownPublication_ = 0;
    }

private:
    std::uint64_t primarySoid_{};
    std::uint64_t epoch_{};
    std::uint64_t nextSequence_{1};
    std::uint64_t acceptedThrough_{};
    std::uint64_t receivedThrough_{};
    /** Receipt mark the host has confirmed, mirroring lobby::Client's own confirmed high-water. */
    std::uint64_t receiptSent_{};
    /** Outgoing sequence the last accepted feed answered for, and the one being offered now. */
    std::uint64_t offeredThrough_{};
    std::uint64_t stagedThrough_{};
    std::uint64_t knownPublication_{};
    std::uint64_t revision_{};
    std::array<RosterEntry, kRosterCapacity> rows_{};
    std::size_t rowCount_{};
    std::array<Invite, kMailboxCapacity> outgoing_{};
    std::size_t outgoingCount_{};
    std::array<Invite, kMailboxCapacity> incoming_{};
    std::size_t incomingCount_{};
    bool incomingDeferred_{};
};

/** The server directory is serialized by the existing BAP session lock. */
[[nodiscard]] Hub& session_directory() noexcept;
void reset_directory() noexcept;
/** Client-facing functions use their own lock and never read native client hook state. */
[[nodiscard]] bool initialize_client(std::uint64_t primarySoid) noexcept;
void client_disconnected() noexcept;
void snapshot_sync(feed::Sync& output) noexcept;
[[nodiscard]] bool apply_feed(const feed::Feed& value) noexcept;
[[nodiscard]] bool post_invite(const Invite& value) noexcept;
[[nodiscard]] bool take_invite(std::uint64_t targetSoid, Invite& output) noexcept;
[[nodiscard]] std::size_t snapshot_peers(AccountHandle viewer,
                                         std::span<RosterEntry> output) noexcept;
[[nodiscard]] std::uint64_t revision() noexcept;
/** True while the local client owes the host something it has not yet been shown. */
[[nodiscard]] bool pending_local_work() noexcept;
[[nodiscard]] std::uint64_t known_publication() noexcept;
void note_publication(std::uint64_t publication) noexcept;
void reset_publication() noexcept;
[[nodiscard]] std::uint64_t platform_id_for_soid(std::uint64_t soid) noexcept;
[[nodiscard]] std::uint64_t soid_for_steam_id(std::uint64_t platformId) noexcept;

} // namespace sunrise::state::social
