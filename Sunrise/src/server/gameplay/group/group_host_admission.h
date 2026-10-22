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
/** Fixed-capacity admitted-peer table; every function below indexes or scans this array. */
extern std::array<Admitted, kAdmittedCapacity> g_admitted;
/** Both accounts must have native player rows in the same admitted group. */
[[nodiscard]] bool accounts_coresident(std::uint64_t first, std::uint64_t second) noexcept;
/** Resolves a native player-add owner; a participant may use its own sibling in one exact group. */
[[nodiscard]] bool view_owner(const state::gameplay::Endpoint& peer,
                              wire::PlayerBlockSoids& output,
                              std::uint64_t participantGroup = 0) noexcept;
// --- Record operations; callers hold the admitted lock -----------------------------------------
/**
 * @return The sole admitted row for `sessionId`, or null when absent or shared by multiple
 * endpoints. Shared sessions normally have several rows; use `find` with their endpoint.
 */
[[nodiscard]] Admitted* find_admitted(std::uint64_t sessionId) noexcept;
/** @return The record admitted for `peer` under `sessionId`, or null. */
[[nodiscard]] Admitted* find(const state::gameplay::Endpoint& peer,
                             std::uint64_t sessionId) noexcept;
/**
 * Finds or claims the record for `peer` under `sessionId`, and binds it to that endpoint.
 * Different endpoints may join the same native session without replacing each other's rows.
 * @param sessionId Zero claims nothing.
 * @return The record, or null when the table is full.
 */
[[nodiscard]] Admitted* claim(const state::gameplay::Endpoint& peer,
                              std::uint64_t sessionId) noexcept;
/**
 * Finds the record for `sessionId` and proves `peer` owns it. Every later message names only its
 * own session, so without this a peer could move the state of a session another endpoint was
 * admitted for.
 * @return The record, or null when absent or owned by a different endpoint.
 */
[[nodiscard]] Admitted* find_owned(const state::gameplay::Endpoint& peer,
                                   std::uint64_t sessionId) noexcept;
/** Marks every record in `sessionId` for republication. */
void mark_stale(std::uint64_t sessionId) noexcept;
/** A rebuilt channel lost its queue; its own snapshots must be sent again. Caller holds lock. */
void refresh_endpoint(const state::gameplay::Endpoint& peer) noexcept;
/** @return A bitmask sized by how many rows share `sessionId`, regardless of visibility. */
[[nodiscard]] std::uint32_t member_mask(std::uint64_t sessionId) noexcept;
/** @return True when native residency lets `recipient` see `peer`'s membership row. */
[[nodiscard]] bool visible_member(const Admitted& recipient, const Admitted& peer) noexcept;
/** @return True when `peer` also has a player `recipient` may see, per `visible_member`. */
[[nodiscard]] bool visible_player(const Admitted& recipient, const Admitted& peer) noexcept;
/** @return A bitmask sized by how many admitted rows `visible_member` grants `recipient`. */
[[nodiscard]] std::uint32_t member_mask(const Admitted& recipient) noexcept;
/** @return Players `visible_player` grants `recipient`. */
[[nodiscard]] std::uint8_t player_count(const Admitted& recipient) noexcept;
/**
 * Adds or updates `record`'s player from `request`, assigning a free slot on first add.
 * @return False for a session mismatch, an invalid request, or a player id or account another
 * record in the session already holds.
 */
[[nodiscard]] bool set_player(Admitted& record, const wire::PlayerAddRequest& request) noexcept;
/**
 * Merges a sparse property update into `record`'s player. Cannot move it to another account or
 * character; that requires `clear_player` and then a fresh `set_player`.
 * @return False for a session or player mismatch, an invalid request, or an account another
 * record in the session already holds.
 */
[[nodiscard]] bool update_player(Admitted& record,
                                 const wire::PlayerPropertiesRequest& request) noexcept;
/** Removes `record`'s player and its presence sample. @return False when it had none. */
[[nodiscard]] bool clear_player(Admitted& record) noexcept;
/** Frees the record for `peer` under `sessionId`. @return False when none was found. */
[[nodiscard]] bool depart(const state::gameplay::Endpoint& peer, std::uint64_t sessionId) noexcept;
/** Retires at most one excess session belonging to a single endpoint, copying its old identity. */
[[nodiscard]] bool retire_excess(Admitted& retired) noexcept;
// --- Queries; these acquire the admitted lock themselves ---------------------------------------
/**
 * @return True when only other endpoints are admitted for `sessionId`. An existing row for
 * `peer` permits sharing; a session with no admitted rows is not a conflict either.
 */
[[nodiscard]] bool owned_elsewhere(const state::gameplay::Endpoint& peer,
                                   std::uint64_t sessionId) noexcept;
/** @return Native player rows admitted in `sessionId`. */
[[nodiscard]] std::uint8_t session_player_count(std::uint64_t sessionId) noexcept;
} // namespace sunrise::server::gameplay::group::admission
