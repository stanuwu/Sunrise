#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <optional>

namespace sunrise::server::bap::host_selection {
struct Candidate {
    std::uint64_t session{};
    std::uint64_t revision{};
    std::uint64_t sourceSession{};
    std::uint64_t sourceRevision{};
    std::uint64_t generation{};
    std::int32_t region{-1};
    bool publicTarget{};
};

inline constexpr std::size_t absent = static_cast<std::size_t>(-1);

/** Establish the current player session before considering its region hosts. */
inline std::size_t current_private(std::span<const Candidate> rows) noexcept {
    std::size_t selected = absent;
    bool ambiguous = false;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (row.publicTarget || row.session == 0 || row.generation == 0) continue;
        if (selected == absent || row.generation > rows[selected].generation) {
            selected = i;
            ambiguous = false;
        } else if (row.generation == rows[selected].generation) {
            ambiguous = true;
        }
    }
    return ambiguous ? absent : selected;
}

/** Public regions belong to the joined public host, never the private directory link. */
inline std::size_t region_host(std::span<const Candidate> rows, std::size_t source,
                              std::int32_t region, std::optional<bool> privateRegion) noexcept {
    if (!privateRegion.has_value() || source >= rows.size() || region < 0
        || rows[source].publicTarget) return absent;
    if (*privateRegion) return rows[source].region == region ? source : absent;
    std::size_t selected = absent;
    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto& row = rows[i];
        if (!row.publicTarget || row.generation == 0 || row.session == 0
            || row.region != region || row.sourceSession != rows[source].session
            || row.sourceRevision != rows[source].revision) continue;
        if (selected != absent) return absent;
        selected = i;
    }
    return selected;
}
} // namespace sunrise::server::bap::host_selection
