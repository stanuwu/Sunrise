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

/** Shared-session state. Only authenticated connection owners may call sync/publish. */
class Hub {
public:
    void opened(AccountHandle account) noexcept;
    void closed(AccountHandle account) noexcept;
    /** Retires an offline cached identity before its account handle is reused. */
    [[nodiscard]] bool forget(AccountHandle account) noexcept;
    [[nodiscard]] bool publish(AccountHandle account, const RosterEntry& row) noexcept;
    [[nodiscard]] bool
    sync(AccountHandle account, const feed::Sync& request, feed::Feed& output) noexcept;
    [[nodiscard]] std::size_t link_count(AccountHandle account) const noexcept;

private:
    struct Account {
        RosterEntry row{};
        std::size_t links{};
        bool syncing{};
        std::uint64_t epoch{};
        std::uint64_t acceptedThrough{};
        std::uint64_t nextDelivery{1};
        std::array<Invite, kMailboxCapacity> inbox{};
        std::size_t inboxCount{};
    };
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
    void snapshot(feed::Sync& output) const noexcept;
    [[nodiscard]] bool receive(const feed::Feed& value) noexcept;
    [[nodiscard]] bool take(Invite& output) noexcept;
    [[nodiscard]] std::size_t peers(std::span<RosterEntry> output) const noexcept;
    [[nodiscard]] std::uint64_t revision() const noexcept {
        return revision_;
    }
    [[nodiscard]] bool dirty() const noexcept {
        return outgoingCount_ != 0;
    }

private:
    std::uint64_t primarySoid_{};
    std::uint64_t epoch_{};
    std::uint64_t nextSequence_{1};
    std::uint64_t acceptedThrough_{};
    std::uint64_t receivedThrough_{};
    std::uint64_t revision_{};
    std::array<RosterEntry, kRosterCapacity> rows_{};
    std::size_t rowCount_{};
    std::array<Invite, kMailboxCapacity> outgoing_{};
    std::size_t outgoingCount_{};
    std::array<Invite, kMailboxCapacity> incoming_{};
    std::size_t incomingCount_{};
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
[[nodiscard]] bool dirty() noexcept;
[[nodiscard]] std::uint64_t platform_id_for_soid(std::uint64_t soid) noexcept;
[[nodiscard]] std::uint64_t soid_for_steam_id(std::uint64_t platformId) noexcept;

} // namespace sunrise::state::social
