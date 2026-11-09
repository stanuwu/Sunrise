#pragma once
#include <algorithm>
#include <array>
#include <limits>
#include <span>

#include "../member_selection.h"

namespace sunrise::state::activity::membership {
/** A committed member and the join lifetime that invalidates old participation reports. */
struct MissionMember final {
    Identity identity{};
    std::uint64_t joinedRevision{};
};
using MissionParty = std::array<MissionMember, entity_slots::kMemberLeaseRowCount>;

/** All-one bits is not a spawn set hash. */
inline constexpr std::uint32_t kInvalidSpawnSetHash = (std::numeric_limits<std::uint32_t>::max)();

/**
 * Lists the committed members of one session record with their join lifetimes.
 * @param record Session record to read.
 * @param output Receives the members in member-row order, primary first. Unused entries are
 * cleared.
 * @return The number of members written.
 */
inline std::size_t read_mission_party(const SessionRecord& record, MissionParty& output) noexcept {
    output = {};
    std::size_t count{};
    for (std::size_t row = 0; row < entity_slots::kMemberLeaseRowCount; ++row) {
        if (const auto* identity = member_identity(record, row)) {
            output[count++] = {*identity,
                               row == 0 ? record.joinedRevision
                                        : record.coMembers[row - 1].joinedRevision};
        }
    }
    return count;
}

/**
 * Tests whether a session record still holds exactly the expected party.
 * @param record Session record to read.
 * @param expected Members and join lifetimes read earlier, in any order.
 * @return True when the counts match and each current member appears exactly once in expected.
 */
inline bool same_mission_party(const SessionRecord& record,
                               std::span<const MissionMember> expected) noexcept {
    MissionParty current{};
    const auto count = read_mission_party(record, current);
    if (count != expected.size()) {
        return false;
    }
    for (const auto& member : std::span(current).first(count)) {
        if (std::count_if(expected.begin(),
                          expected.end(),
                          [&](const auto& row) {
                              return row.identity == member.identity
                                     && row.joinedRevision == member.joinedRevision;
                          })
            != 1) {
            return false;
        }
    }
    return true;
}

/** Preflights the entire party before arming any native member's wipe block. */
inline bool arm_mission_party(SessionRecord& record,
                              std::uint64_t request,
                              std::int32_t region,
                              std::uint32_t spawn) noexcept {
    if (!request || region < 0 || !spawn || spawn == kInvalidSpawnSetHash) {
        return false;
    }
    std::size_t count{};
    for (std::size_t row = 0; row < entity_slots::kMemberLeaseRowCount; ++row) {
        const auto* member = member_state(record, row);
        if (!member) {
            continue;
        }
        ++count;
        const auto& wipe = member->hardWipe;
        if (wipe.requestKey == request) {
            if (wipe.region != region || wipe.spawnSetHash != spawn) {
                return false;
            }
        } else if (wipe.active || member->hasHostTeleport) {
            return false;
        }
    }
    if (!count) {
        return false;
    }
    for (std::size_t row = 0; row < entity_slots::kMemberLeaseRowCount; ++row) {
        auto* member = member_state(record, row);
        if (!member || member->hardWipe.requestKey == request) {
            continue;
        }
        auto& wipe = member->hardWipe;
        wipe = {};
        wipe.requestKey = request;
        wipe.region = region;
        wipe.spawnSetHash = spawn;
        wipe.host.state = kHardWipeStartState;
        wipe.host.opaqueByte = static_cast<std::uint8_t>(member->spawn.opaqueByte + 1U);
        wipe.active = true;
    }
    return true;
}

/** Releases surviving participants in this request; later joins are not reset. */
inline bool release_mission_party(SessionRecord& record, std::uint64_t request) noexcept {
    if (!request) {
        return false;
    }
    std::size_t count{};
    for (std::size_t row = 0; row < entity_slots::kMemberLeaseRowCount; ++row) {
        const auto* member = member_state(record, row);
        if (!member) {
            continue;
        }
        if (member->hardWipe.requestKey == request) {
            ++count;
        } else if (member->hardWipe.active) {
            return false;
        }
    }
    if (!count) {
        return false;
    }
    for (std::size_t row = 0; row < entity_slots::kMemberLeaseRowCount; ++row) {
        auto* member = member_state(record, row);
        if (member && member->hardWipe.requestKey == request) {
            member->hardWipe.release();
        }
    }
    return true;
}

/** Exact-binding snapshots and atomic mutations independent of caller account scope. */
[[nodiscard]] bool snapshot_mission_party(const SessionBinding& binding,
                                          MissionParty& output,
                                          std::size_t& count) noexcept;
[[nodiscard]] bool arm_party_hard_wipe(const SessionBinding& binding,
                                       std::uint64_t request,
                                       std::int32_t region,
                                       std::uint32_t spawn,
                                       std::span<const MissionMember> expected) noexcept;
[[nodiscard]] bool release_party_hard_wipe(const SessionBinding& binding,
                                           std::uint64_t request) noexcept;
} // namespace sunrise::state::activity::membership
