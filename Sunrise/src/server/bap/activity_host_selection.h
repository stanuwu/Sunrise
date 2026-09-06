#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "../../state/activity/definition.h"

namespace sunrise::server::bap::host_selection {

/** A connection has no activity binding before its first generation is assigned. */
inline constexpr std::uint64_t kUnboundGeneration = 0;

/** Binding identities and connection generation copied while the session lock is held. */
struct Candidate {
    std::uint64_t session{};
    std::uint64_t revision{};
    std::uint64_t sourceSession{};
    std::uint64_t sourceRevision{};
    std::uint64_t generation{};
    std::int32_t region{state::activity::membership::kAbsentRegionIndex};
    bool publicTarget{};
};

/** No candidate row was selected. */
inline constexpr std::size_t absent = (std::numeric_limits<std::size_t>::max)();

/** @return Index of the newest private binding, or absent when missing or tied. */
inline std::size_t current_private(std::span<const Candidate> rows) noexcept {
    std::size_t selected = absent;
    bool ambiguous = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (row.publicTarget || row.session == state::activity::kAbsentSessionId
            || row.generation == kUnboundGeneration) {
            continue;
        }
        if (selected == absent || row.generation > rows[selected].generation) {
            selected = i;
            ambiguous = false;
        } else if (row.generation == rows[selected].generation) {
            ambiguous = true;
        }
    }
    return ambiguous ? absent : selected;
}

/**
 * Selects the private source or its public host for one region.
 * Public hosts must match both the source session and its revision.
 * @param privateRegion Authored publicity; an empty value rejects selection.
 * @return Index of the unique region owner, or absent when no exact owner is available.
 */
inline std::size_t region_host(std::span<const Candidate> rows,
                               std::size_t source,
                               std::int32_t region,
                               std::optional<bool> privateRegion) noexcept {
    if (!privateRegion.has_value() || source >= rows.size() || region < 0
        || rows[source].publicTarget) {
        return absent;
    }
    if (*privateRegion) {
        return rows[source].region == region ? source : absent;
    }
    std::size_t selected = absent;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (!row.publicTarget || row.generation == kUnboundGeneration
            || row.session == state::activity::kAbsentSessionId
            || row.region != region || row.sourceSession != rows[source].session
            || row.sourceRevision != rows[source].revision) {
            continue;
        }
        if (selected != absent) {
            return absent;
        }
        selected = i;
    }
    return selected;
}

} // namespace sunrise::server::bap::host_selection
