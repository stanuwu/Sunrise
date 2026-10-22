#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../account/account_handle.h"
#include "social_feed.h"

namespace sunrise::state::social {

/** Roster rows one account's feed can carry, mirroring the wire row budget. */
inline constexpr std::size_t kRosterCapacity = feed::kRowCapacity;
/** Invites one account's mailbox can queue, mirroring the wire invite budget. */
inline constexpr std::size_t kMailboxCapacity = feed::kInviteCapacity;
/** One queued invitation, in the same layout the feed carries it. */
using Invite = feed::WireInvite;
/** One roster row, in the same layout the feed carries it. */
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
    /** Registers one more connection for this account; the first bumps the hub revision. */
    void opened(AccountHandle account) noexcept;
    /** Releases one connection; the last also stops syncing and bumps the hub revision. */
    void closed(AccountHandle account) noexcept;
    /** Retires an offline cached identity before its account handle is reused. */
    [[nodiscard]] bool forget(AccountHandle account) noexcept;
    /** Publishes a valid, unique identity; changed content advances revision, refusal changes
     * nothing. */
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
    /** True only for this account's registered, nonzero connection/serial pair. */
    [[nodiscard]] bool
    delivers(AccountHandle account, std::uint32_t connection, std::uint64_t serial) const noexcept;
    /** Registered connection count, or zero for an invalid account handle. */
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
        /** Mailbox, cursor and epoch revision; the full Stamp also includes roster, routes and
         * chat. */
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

/**
 * Platform-owned client queues. Network receipt is distinct from explicit invitation
 * acceptance.
 */
class Client {
public:
    /** Discards any prior registration and starts a fresh one under a new epoch. */
    void initialize(std::uint64_t primarySoid, std::uint64_t epoch) noexcept;
    /** Clears roster/incoming invites; retains outgoing invites and reoffers them on reconnect. */
    void disconnected() noexcept;
    /** Queues an invite to a current peer and assigns its sequence; refusal spends no sequence. */
    [[nodiscard]] bool post(const Invite& invite) noexcept;
    /** Also stages how far the outgoing queue is being offered; only a feed commits that mark. */
    void snapshot(feed::Sync& output) noexcept;
    /**
     * Applies an accepted feed: replaces the roster, admits validated invites, and drops
     * outgoing entries the host has now accepted.
     * @return False when the feed's epoch, rows, or invites fail validation; state is
     * unchanged in that case.
     */
    [[nodiscard]] bool receive(const feed::Feed& value) noexcept;
    /** @return True and dequeues the oldest incoming invite; false when none are queued. */
    [[nodiscard]] bool take(Invite& output) noexcept;
    /** @return Roster rows copied into `output`, capped by its size. */
    [[nodiscard]] std::size_t peers(std::span<RosterEntry> output) const noexcept;
    /** Change token for received roster content, also advanced when a nonempty roster is cleared.
     */
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }
    /**
     * Work the host has not been shown yet: an invitation past the offered mark, or a receipt the
     * host has not confirmed, or newly available room for an incoming invite held by the host.
     * A host-refused offer waits for a publication notice when mailbox capacity changes.
     */
    [[nodiscard]] bool pending_local_work() const noexcept {
        return (outgoingCount_ != 0 && outgoing_[outgoingCount_ - 1].sequence > offeredThrough_)
               || receivedThrough_ != receiptSent_
               || (incomingDeferred_ && incomingCount_ < incoming_.size());
    }
    /** Highest feed/notice delivery token seen within the current host connection. */
    [[nodiscard]] std::uint64_t known_publication() const noexcept {
        return knownPublication_;
    }
    /** Raises the observed publication high-water mark without applying a feed or lowering it. */
    void note_publication(std::uint64_t publication) noexcept;
    /** A new host connection may restart its publication clock; preserve the queued work. */
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
    /** The last valid feed contained an invitation that did not fit the local inbox. */
    bool incomingDeferred_{};
};

/** The server directory is serialized by the existing BAP session lock. */
[[nodiscard]] Hub& session_directory() noexcept;
/** Destroys and reconstructs the server directory in place, clearing every account. */
void reset_directory() noexcept;
/** Client-facing functions use their own lock and never read native client hook state. */
[[nodiscard]] bool initialize_client(std::uint64_t primarySoid) noexcept;
/** Clears local roster/incoming invites while preserving outgoing work for reconnect. */
void client_disconnected() noexcept;
/** Stages the local client's outgoing sync request, including the lobby chat's own. */
void snapshot_sync(feed::Sync& output) noexcept;
/** Applies an accepted feed to the local client and its lobby chat. */
[[nodiscard]] bool apply_feed(const feed::Feed& value) noexcept;
/** Queues an invite for the local client to offer in its next sync. */
[[nodiscard]] bool post_invite(const Invite& value) noexcept;
/** @return True and dequeues the invite when `targetSoid` names the local client. */
[[nodiscard]] bool take_invite(std::uint64_t targetSoid, Invite& output) noexcept;
/** @return Roster rows copied for the local viewer; zero for any other account. */
[[nodiscard]] std::size_t snapshot_peers(AccountHandle viewer,
                                         std::span<RosterEntry> output) noexcept;
/** @return The local client's roster revision. */
[[nodiscard]] std::uint64_t revision() noexcept;
/** True while the local client owes the host something it has not yet been shown. */
[[nodiscard]] bool pending_local_work() noexcept;
/** @return Highest publication the local client has observed, from a feed or a direct notice. */
[[nodiscard]] std::uint64_t known_publication() noexcept;
/** Records a publication the local client learned of outside a feed, without applying one. */
void note_publication(std::uint64_t publication) noexcept;
/** Forgets only the old host's publication clock when the social connection changes. */
void reset_publication() noexcept;
/** @return The local peer roster's Steam id for `soid`, or zero when not present. */
[[nodiscard]] std::uint64_t platform_id_for_soid(std::uint64_t soid) noexcept;
/** @return The local peer roster's primary soid for `platformId`, or zero when not present. */
[[nodiscard]] std::uint64_t soid_for_steam_id(std::uint64_t platformId) noexcept;

} // namespace sunrise::state::social
