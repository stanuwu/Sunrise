#pragma once

#include "definition.h"

namespace sunrise::state::activity {
/** Native member keys and the current recipient's membership receipt, from one locked snapshot. */
struct JoinedMemberSet final {
    std::array<std::uint64_t, entity_slots::kMemberLeaseRowCount> keys{};
    bool acknowledged{};
};
[[nodiscard]] bool joined_member_set(const SessionBinding& binding,
                                     std::uint64_t memberKey,
                                     JoinedMemberSet& output) noexcept;
/** Exact recipient and retired slots captured before a type-25 publication. */
struct MemberPurge final {
    SessionBinding binding{};
    entity_slots::LeaseMask slots{};
    std::uint64_t accountSoid{};
    std::uint64_t memberKey{};
};
[[nodiscard]] bool pending_member_purge(const SessionBinding& binding,
                                        std::uint64_t memberKey,
                                        MemberPurge& output) noexcept;
/** Commits only after the complete frame fits; advances the receipt barrier for slot reuse. */
[[nodiscard]] bool commit_member_purge(const MemberPurge& pending) noexcept;
/** Consumes the exact native member's connection departure without disbanding its fireteam. */
[[nodiscard]] bool depart_member(const SessionBinding& binding,
                                 std::uint64_t accountSoid,
                                 std::uint64_t memberKey) noexcept;
} // namespace sunrise::state::activity
