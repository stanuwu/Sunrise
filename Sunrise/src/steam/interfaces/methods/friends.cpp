#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

#include "../../../client/hooks/account_registration/account_registration.h"
#include "../../../core/settings/settings.h"
#include "../../../state/account/account_context.h"
#include "../../../state/social/steam_roster.h"
#include "../internal.h"
#include "../invitations.h"

namespace sunrise::steam::interfaces::methods {
namespace {
namespace social = state::social;
constexpr int kImmediate = 4;
constexpr int kPersonaCallback = 304;
constexpr int kJoinCallback = 337;
constexpr int kArrived = 0x04 | 0x02 | 0x10;
constexpr int kDeparted = 0x08 | 0x02 | 0x10;
SRWLOCK mutex = SRWLOCK_INIT;

template <class Action> auto locked(Action&& action) noexcept -> decltype(action()) {
    AcquireSRWLockExclusive(&mutex);
    __try {
        return action();
    } __finally {
        ReleaseSRWLockExclusive(&mutex);
    }
}
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
std::uint64_t nextInvitationId{};

bool ready() noexcept {
    return !core::settings::multiplayer()
           || client::hooks::account_registration::own_entry_registered();
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
    return queue_callback(kPersonaCallback, 0, &change, sizeof change);
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
    return find(steamId, row) ? 3 : 0;
}
int friend_persona_state([[maybe_unused]] void* self, std::uint64_t steamId) noexcept {
    social::RosterEntry row{};
    return find(steamId, row) ? 1 : 0;
}
const char* friend_persona_name([[maybe_unused]] void* self, std::uint64_t steamId) noexcept {
    thread_local std::array<std::array<char, social::feed::kNameCapacity>, 4> names{};
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
    info->gameId = static_cast<std::uint64_t>(app_id()) & 0xFFFFFFULL;
    return true;
}

void service_friends() noexcept {
    if (!ready()) {
        return;
    }
    locked([] {
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

void service_invites() noexcept {
    const auto local = state::account_primary_soid(state::kLocalAccount);
    locked([local] {
        if (invitationAccount != local) {
            invitations = {};
            invitationAccount = local;
        }
        if (local == 0 || !ready()) {
            return;
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
                if (queue_callback(kJoinCallback, 0, &event, sizeof event)) {
                    invitation = {};
                }
            }
        }
        for (auto& invitation : invitations) {
            if (invitation.presentation.id != 0) {
                continue;
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
            std::snprintf(invitation.presentation.inviterName.data(),
                          invitation.presentation.inviterName.size(),
                          "%s",
                          peer->personaName.data());
        }
    });
}
bool pending_invitation(PendingInvitation& output) noexcept {
    output = {};
    if (!TryAcquireSRWLockExclusive(&mutex)) {
        return false;
    }
    __try {
        for (const auto& invitation : invitations) {
            if (invitation.presentation.id != 0 && !invitation.accepted
                && (output.id == 0 || invitation.presentation.id < output.id)) {
                output = invitation.presentation;
            }
        }
        return output.id != 0;
    } __finally {
        ReleaseSRWLockExclusive(&mutex);
    }
}
bool decide_invitation(std::uint64_t id, bool accept) noexcept {
    return locked([id, accept] {
        for (auto& invitation : invitations) {
            if (id != 0 && invitation.presentation.id == id && !invitation.accepted
                && invitationAccount == state::account_primary_soid(state::kLocalAccount)) {
                if (accept) {
                    invitation.accepted = true;
                } else {
                    invitation = {};
                }
                return true;
            }
        }
        return false;
    });
}
void reset_friends() noexcept {
    locked([] {
        announced = {};
        announcedCount = 0;
        announcedRevision = 0;
        invitations = {};
        invitationAccount = 0;
    });
}

} // namespace sunrise::steam::interfaces::methods
