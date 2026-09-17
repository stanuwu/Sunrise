#pragma once

#include <Windows.h>

#include <array>

#include "../../../middleware/gameplay/group/member_messages.h"
#include "../../../middleware/gameplay/group/native_player_profile.h"
#include "../../../state/gameplay/definition.h"
#include "group_publication_stamp.h"
#include "group_residency.h"

namespace sunrise::server::gameplay::group::admission {
namespace wire = middleware::gameplay::group;
/** Every player may hold overlapping current and target public sessions. */
inline constexpr std::size_t kAdmittedCapacity = core::network_capacity::kPlayers * 2;
/** The same overlap seen from the session side: one current and one target. */
inline constexpr std::size_t kPublicSessionCapacity = 2;
/** Player slots the native session table holds, tracked as a 32-bit occupancy mask. */
inline constexpr std::uint32_t kPlayerSlotCount = 32;
/** The native player-add sequence field is twenty bits wide. */
inline constexpr std::uint32_t kPlayerAddSequenceMask = 0xFFFFFU;
/** Largest player kind the native player-add carries. */
inline constexpr std::uint32_t kMaximumPlayerKind = 3;
/** Seed the native baseline checksum hashes the decoded B image with. */
inline constexpr std::uint32_t kBaselineChecksumSeed = 0xDEADBFD6U;
/** One admitted peer and the player it asked this host to add. */
struct Admitted {
    state::gameplay::Endpoint endpoint{};
    std::uint64_t joinId{};
    /** Machine id the peer's join request carried for itself. Zero when its row was not found. */
    std::uint64_t machineId{};
    std::uint64_t playerId{};
    /** Player kind and soid pair the peer's own player-add carried, republished verbatim. */
    std::uint8_t playerKind{};
    std::uint32_t playerProfileValue{};
    wire::PlayerBlockSoids playerSoids{};
    wire::NativePlayerProfile playerProfile{};
    std::uint32_t playerSlot{};
    std::uint32_t playerAddSequence{};
    /** Group-session id the peer named in its join request, which its parameters must echo. */
    std::uint64_t sessionId{};
    bool occupied{};
    bool hasPlayer{};
    /** The peer has reported its join finished, so its member state is `established`. */
    bool joinComplete{};
    /** The reliable queue refused the `activity-host` parameter. Nothing asks for it again. */
    bool parameterOwed{};
    bool rosterStale{};
    PublicationStamp publication{};
    residency::Report presence{};
    /** A new player identity needs one complete presence sample before any group publication. */
    bool presencePending{};
    /** Order in which the peer last named this session. The lowest is the least recently used. */
    std::uint64_t lastUse{};
};

/** Guards the admitted table against the worker and the callback pump. */
extern SRWLOCK g_admittedLock;
extern std::array<Admitted, kAdmittedCapacity> g_admitted;
/** Both accounts must have native player rows in the same admitted group. */
[[nodiscard]] bool accounts_coresident(std::uint64_t first, std::uint64_t second) noexcept;
/** Resolves a native player-add owner; a participant may use its own sibling in one exact group. */
[[nodiscard]] bool view_owner(const state::gameplay::Endpoint& peer,
                              wire::PlayerBlockSoids& output,
                              std::uint64_t participantGroup = 0) noexcept;
/** Callers hold the admitted lock for record operations. Session-only lookup rejects ambiguity. */
[[nodiscard]] Admitted* find_admitted(std::uint64_t sessionId) noexcept;
[[nodiscard]] Admitted* find(const state::gameplay::Endpoint& peer,
                             std::uint64_t sessionId) noexcept;
[[nodiscard]] Admitted* claim(const state::gameplay::Endpoint& peer,
                              std::uint64_t sessionId) noexcept;
[[nodiscard]] Admitted* find_owned(const state::gameplay::Endpoint& peer,
                                   std::uint64_t sessionId) noexcept;
void mark_stale(std::uint64_t sessionId) noexcept;
/** A rebuilt channel lost its queue; its own snapshots must be sent again. Caller holds lock. */
void refresh_endpoint(const state::gameplay::Endpoint& peer) noexcept;
[[nodiscard]] std::uint32_t member_mask(std::uint64_t sessionId) noexcept;
[[nodiscard]] bool visible_member(const Admitted& recipient, const Admitted& peer) noexcept;
[[nodiscard]] bool visible_player(const Admitted& recipient, const Admitted& peer) noexcept;
[[nodiscard]] std::uint32_t member_mask(const Admitted& recipient) noexcept;
[[nodiscard]] std::uint8_t player_count(const Admitted& recipient) noexcept;
[[nodiscard]] bool set_player(Admitted& record, const wire::PlayerAddRequest& request) noexcept;
[[nodiscard]] bool update_player(Admitted& record,
                                 const wire::PlayerPropertiesRequest& request) noexcept;
[[nodiscard]] bool clear_player(Admitted& record) noexcept;
[[nodiscard]] bool depart(const state::gameplay::Endpoint& peer, std::uint64_t sessionId) noexcept;
/** Retires at most one excess session belonging to a single endpoint, copying its old identity. */
[[nodiscard]] bool retire_excess(Admitted& retired) noexcept;
/** Queries below acquire the admitted lock themselves. */
[[nodiscard]] bool owned_elsewhere(const state::gameplay::Endpoint& peer,
                                   std::uint64_t sessionId) noexcept;
[[nodiscard]] std::uint8_t session_player_count(std::uint64_t sessionId) noexcept;
} // namespace sunrise::server::gameplay::group::admission
