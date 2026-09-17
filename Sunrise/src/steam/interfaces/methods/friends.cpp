#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <limits>

#include "../../../client/hooks/account_registration/account_registration.h"
#include "../../../core/settings/settings.h"
#include "../../../state/account/account_context.h"
#include "../../../state/social/steam_roster.h"
#include "../internal.h"
#include "../invitations.h"

namespace sunrise::steam::interfaces::methods {
namespace {
namespace social = state::social;
// Steamworks enumerators, spelled out because the SDK headers are not linked here.
// EFriendFlags: the caller asked for actual friends rather than any other list kind.
constexpr int kImmediate = 4;
// Callback ids of PersonaStateChange_t and GameRichPresenceJoinRequested_t.
constexpr int kPersonaCallback = 304;
constexpr int kJoinCallback = 337;
// EPersonaChange bits: status and game-played, with come-online or gone-offline beside them.
constexpr int kArrived = 0x04 | 0x02 | 0x10;
constexpr int kDeparted = 0x08 | 0x02 | 0x10;
// EFriendRelationship: a roster peer is a friend, anyone else has no relationship at all.
constexpr int kRelationshipFriend = 3;
constexpr int kRelationshipNone = 0;
// EPersonaState: a roster peer is online, anyone else is offline.
constexpr int kPersonaOnline = 1;
constexpr int kPersonaOffline = 0;
// CGameID carries the app id in its low 24 bits.
constexpr std::uint64_t kGameIdAppMask = 0xFFFFFFULL;
// Results one caller may hold at once, since each call returns a pointer into this store.
constexpr std::size_t kPersonaNameSlots = 4;
// A competing or reentrant pump skips this producer pass; no caller waits for ownership.
std::atomic_flag producer = ATOMIC_FLAG_INIT;
std::atomic<std::uint64_t> generation{1};
// Keep the last choice across consumption/reset so delayed UI writers cannot replace a newer ID.
std::atomic<std::uint64_t> decision{};
std::uint64_t ownedGeneration{};
// The mailbox reserves its low bit for Accept; zero is empty and IDs never repeat after reset.
constexpr auto kMaximumInvitationId = (std::numeric_limits<std::uint64_t>::max)() >> 1U;
static_assert(std::atomic<std::uint64_t>::is_always_lock_free
              && std::atomic<char>::is_always_lock_free);
std::array<social::RosterEntry, social::kRosterCapacity> announced{};
std::size_t announcedCount{};
std::uint64_t announcedRevision{};
struct Invitation {
    PendingInvitation presentation{};
    social::Invite delivery{};
    bool accepted{};
};
std::array<Invitation, social::kMailboxCapacity> invitations{};
std::uint64_t invitationAccount{};
bool invitationEnabled{};
std::uint64_t nextInvitationId{};

bool ready() noexcept {
    return core::settings::multiplayer()
           && client::hooks::account_registration::own_entry_registered();
}
std::size_t peers(std::array<social::RosterEntry, social::kRosterCapacity>& rows) noexcept {
    return ready() ? social::snapshot_peers(state::kLocalAccount, rows) : 0;
}
bool find(std::uint64_t steamId, social::RosterEntry& output) noexcept {
    std::array<social::RosterEntry, social::kRosterCapacity> rows{};
    const auto count = peers(rows);
    for (std::size_t i = 0; i < count; ++i) {
        if (rows[i].steamId == steamId) {
            output = rows[i];
            return true;
        }
    }
    return false;
}
bool raise(std::uint64_t steamId, int flags) noexcept {
    // The native handler for callback 304 (PersonaStateChange_t) writes through the
    // friends-manager singleton, so the change is queued only once that singleton exists.
    if (!client::hooks::account_registration::friends_manager_ready()) {
        return false;
    }
    const PersonaStateChange change{steamId, flags};
    const CallbackDelivery event{
        kPersonaCallback, 0, &change, sizeof change, {&generation, ownedGeneration}};
    return queue_callbacks(std::span(&event, 1));
}
struct PublishedInvitation {
    PendingInvitation value{};
    std::uint64_t generation{};
};
struct InvitationPublication {
    std::atomic<std::uint64_t> sequence{};
    std::atomic<std::uint64_t> id{};
    std::atomic<std::uint64_t> steamId{};
    std::atomic<std::uint64_t> generation{};
    std::array<std::atomic<char>, social::feed::kNameCapacity> name{};
};
InvitationPublication published;
PublishedInvitation lastPublished;

/** Publishes only when the oldest visible invitation or its lifetime changes. */
void publish_invitation() noexcept {
    PublishedInvitation next{};
    next.generation = ownedGeneration;
    if (ready()) {
        for (const auto& invitation : invitations) {
            if (invitation.presentation.id != 0 && !invitation.accepted
                && (next.value.id == 0 || invitation.presentation.id < next.value.id)) {
                next.value = invitation.presentation;
            }
        }
    }
    if (next.generation == lastPublished.generation && next.value.id == lastPublished.value.id
        && next.value.inviterSteamId == lastPublished.value.inviterSteamId
        && next.value.inviterName == lastPublished.value.inviterName) {
        return;
    }
    // Sequentially consistent fields make the odd/even sequence test a coherent snapshot.
    // All fields are atomic: a UI read overlapping publication is still free of data races.
    const auto sequence = published.sequence.load();
    published.sequence.store(sequence + 1);
    published.id.store(next.value.id);
    published.steamId.store(next.value.inviterSteamId);
    published.generation.store(next.generation);
    for (std::size_t index = 0; index < next.value.inviterName.size(); ++index) {
        published.name[index].store(next.value.inviterName[index]);
    }
    published.sequence.store(sequence + 2);
    lastPublished = next;
}

/** Makes one bounded read; callers retain their last coherent image during publication. */
bool read_invitation(PublishedInvitation& output) noexcept {
    const auto sequence = published.sequence.load();
    if ((sequence & 1U) != 0) {
        return false;
    }
    PublishedInvitation candidate{};
    candidate.value.id = published.id.load();
    candidate.value.inviterSteamId = published.steamId.load();
    candidate.generation = published.generation.load();
    for (std::size_t index = 0; index < candidate.value.inviterName.size(); ++index) {
        candidate.value.inviterName[index] = published.name[index].load();
    }
    if (published.sequence.load() != sequence) {
        return false;
    }
    output = candidate;
    return true;
}

/** Establishes exclusive producer ownership without waiting; reset is observed before mutation. */
template <class Action> void service(std::uint64_t expectedGeneration, Action&& action) noexcept {
    if (expectedGeneration == 0) {
        expectedGeneration = generation.load();
    }
    if (producer.test_and_set(std::memory_order_acquire)) {
        return;
    }
    __try {
        if (expectedGeneration != generation.load()) {
            return;
        }
        const auto local = state::account_primary_soid(state::kLocalAccount);
        const bool enabled = core::settings::multiplayer();
        auto current = generation.load();
        if (current != expectedGeneration) {
            return;
        }
        if (current != ownedGeneration || local != invitationAccount
            || enabled != invitationEnabled) {
            if (current == ownedGeneration) {
                const auto next = current + 1;
                if (!generation.compare_exchange_strong(current, next)) {
                    return;
                }
                current = next;
            }
            announced = {};
            announcedCount = 0;
            announcedRevision = 0;
            invitations = {};
            invitationAccount = local;
            invitationEnabled = enabled;
            ownedGeneration = current;
        }
        action();
        publish_invitation();
    } __finally {
        producer.clear(std::memory_order_release);
    }
}
} // namespace

/** @return Persona name from settings. It lasts for the whole process. */
const char* persona_name([[maybe_unused]] void* self) noexcept {
    return core::settings::get().steam.user.personaName.data();
}

int friend_count([[maybe_unused]] void* self, int flags) noexcept {
    if ((flags & kImmediate) == 0) {
        return 0;
    }
    std::array<social::RosterEntry, social::kRosterCapacity> rows{};
    return static_cast<int>(peers(rows));
}
SteamId*
friend_by_index([[maybe_unused]] void* self, SteamId* result, int index, int flags) noexcept {
    if (!result) {
        return nullptr;
    }
    result->value = 0;
    std::array<social::RosterEntry, social::kRosterCapacity> rows{};
    const auto count = peers(rows);
    if ((flags & kImmediate) != 0 && index >= 0 && static_cast<std::size_t>(index) < count) {
        result->value = rows[static_cast<std::size_t>(index)].steamId;
    }
    return result;
}
int friend_relationship([[maybe_unused]] void* self, std::uint64_t steamId) noexcept {
    social::RosterEntry row{};
    return find(steamId, row) ? kRelationshipFriend : kRelationshipNone;
}
int friend_persona_state([[maybe_unused]] void* self, std::uint64_t steamId) noexcept {
    social::RosterEntry row{};
    return find(steamId, row) ? kPersonaOnline : kPersonaOffline;
}
const char* friend_persona_name([[maybe_unused]] void* self, std::uint64_t steamId) noexcept {
    thread_local std::array<std::array<char, social::feed::kNameCapacity>, kPersonaNameSlots>
        names{};
    thread_local std::size_t slot{};
    auto& output = names[slot];
    slot = (slot + 1) % names.size();
    social::RosterEntry row{};
    output = find(steamId, row) ? row.personaName : decltype(row.personaName){};
    return output.data();
}
bool friend_game_played([[maybe_unused]] void* self,
                        std::uint64_t steamId,
                        FriendGameInfo* info) noexcept {
    if (!info) {
        return false;
    }
    *info = {};
    social::RosterEntry row{};
    if (!find(steamId, row)) {
        return false;
    }
    info->gameId = static_cast<std::uint64_t>(app_id()) & kGameIdAppMask;
    return true;
}

std::uint64_t friends_generation() noexcept {
    return generation.load();
}

void service_friends(std::uint64_t expectedGeneration) noexcept {
    service(expectedGeneration, [] {
        if (!ready()) {
            return;
        }
        const auto revision = social::revision();
        if (revision == announcedRevision) {
            return;
        }
        std::array<social::RosterEntry, social::kRosterCapacity> rows{};
        const auto count = peers(rows);
        bool complete = true;
        for (std::size_t i = 0; i < announcedCount;) {
            const auto row = std::find_if(
                rows.begin(),
                rows.begin() + static_cast<std::ptrdiff_t>(count),
                [&](const auto& value) { return value.steamId == announced[i].steamId; });
            if (row == rows.begin() + static_cast<std::ptrdiff_t>(count)) {
                if (!raise(announced[i].steamId, kDeparted)) {
                    complete = false;
                    ++i;
                    continue;
                }
                for (auto j = i + 1; j < announcedCount; ++j) {
                    announced[j - 1] = announced[j];
                }
                announced[--announcedCount] = {};
            } else {
                if (row->personaName != announced[i].personaName) {
                    if (raise(row->steamId, 1)) {
                        announced[i] = *row;
                    } else {
                        complete = false;
                    }
                }
                ++i;
            }
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (std::any_of(announced.begin(),
                            announced.begin() + static_cast<std::ptrdiff_t>(announcedCount),
                            [&](const auto& value) { return value.steamId == rows[i].steamId; })) {
                continue;
            }
            if (announcedCount == announced.size() || !raise(rows[i].steamId, kArrived)) {
                complete = false;
                continue;
            }
            announced[announcedCount++] = rows[i];
        }
        if (complete) {
            announcedRevision = revision;
        }
    });
}

bool invite_user_to_game([[maybe_unused]] void* self,
                         std::uint64_t steamId,
                         const char* connect) noexcept {
    social::RosterEntry peer{};
    if (!connect || !find(steamId, peer)) {
        return false;
    }
    social::Invite invite{};
    invite.targetSoid = peer.primarySoid;
    invite.inviterSoid = state::account_primary_soid(state::kLocalAccount);
    const auto length = strnlen_s(connect, invite.connect.size());
    if (length == invite.connect.size()) {
        return false;
    }
    std::copy_n(connect, length, invite.connect.begin());
    return social::post_invite(invite);
}

void service_invites(std::uint64_t expectedGeneration) noexcept {
    service(expectedGeneration, [] {
        const auto local = invitationAccount;
        if (local == 0 || !ready()) {
            return;
        }
        const auto choice = decision.load();
        for (auto& invitation : invitations) {
            if (invitation.presentation.id != 0 && invitation.presentation.id == (choice >> 1U)
                && !invitation.accepted) {
                if ((choice & 1U) != 0) {
                    invitation.accepted = true;
                } else {
                    invitation = {};
                }
                break;
            }
        }
        std::array<social::RosterEntry, social::kRosterCapacity> rows{};
        const auto count = peers(rows);
        const auto peer_for = [&](std::uint64_t primary) -> const social::RosterEntry* {
            for (std::size_t i = 0; i < count; ++i) {
                if (rows[i].primarySoid == primary) {
                    return &rows[i];
                }
            }
            return nullptr;
        };
        for (auto& invitation : invitations) {
            if (invitation.presentation.id == 0) {
                continue;
            }
            const auto* peer = peer_for(invitation.delivery.inviterSoid);
            if (!peer || peer->steamId != invitation.presentation.inviterSteamId) {
                invitation = {};
                continue;
            }
            if (invitation.accepted) {
                GameRichPresenceJoinRequested event{};
                event.friendSteamId = peer->steamId;
                std::copy(invitation.delivery.connect.begin(),
                          invitation.delivery.connect.end(),
                          event.connect);
                const CallbackDelivery delivery{
                    kJoinCallback, 0, &event, sizeof event, {&generation, ownedGeneration}};
                if (queue_callbacks(std::span(&delivery, 1))) {
                    invitation = {};
                }
            }
        }
        for (auto& invitation : invitations) {
            if (invitation.presentation.id != 0) {
                continue;
            }
            if (nextInvitationId == kMaximumInvitationId) {
                break;
            }
            social::Invite delivery{};
            if (!social::take_invite(local, delivery)) {
                break;
            }
            const auto* peer = peer_for(delivery.inviterSoid);
            if (!peer) {
                continue;
            }
            invitation.delivery = delivery;
            invitation.presentation.id = ++nextInvitationId;
            invitation.presentation.inviterSteamId = peer->steamId;
            invitation.presentation.inviterName = peer->personaName;
        }
    });
}

bool pending_invitation(PendingInvitation& output) noexcept {
    thread_local PublishedInvitation snapshot;
    (void)read_invitation(snapshot);
    output = {};
    if (snapshot.generation != generation.load() || snapshot.value.id == 0
        || (decision.load() >> 1U) >= snapshot.value.id) {
        return false;
    }
    output = snapshot.value;
    return true;
}

bool decide_invitation(std::uint64_t id, bool accept) noexcept {
    PendingInvitation current{};
    if (id == 0 || !pending_invitation(current) || current.id != id) {
        return false;
    }
    auto previous = decision.load();
    if ((previous >> 1U) >= id) {
        return false;
    }
    return decision.compare_exchange_strong(previous, (id << 1U) | (accept ? 1U : 0U));
}

void reset_friends() noexcept {
    (void)generation.fetch_add(1);
}

} // namespace sunrise::steam::interfaces::methods
