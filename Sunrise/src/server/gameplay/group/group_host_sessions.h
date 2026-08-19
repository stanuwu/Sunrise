#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/activity/definition.h"

namespace sunrise::server::gameplay::group {

/** Region sentinel that cannot claim or replace a host-session row. */
inline constexpr std::int32_t kUnknownRegion = -1;

/** Immutable identity of one source-bound activity-host row generation. */
struct HostSessionBinding {
    state::activity::SessionBinding source{};
    state::activity::SessionBinding target{};
    std::uint64_t groupSessionId{};
    std::uint64_t generation{};
    std::int32_t regionIndex{kUnknownRegion};
};

/** Result of requesting one source-bound activity-host row. */
enum class HostSessionState : std::uint8_t {
    absent,
    pending,
    ready,
    conflict,
    full,
};

/** One advertised group session and its currently allocated activity-host session. */
struct HostSessionRow {
    std::uint64_t groupSessionId{};
    std::uint64_t hostSessionId{};
    std::int32_t regionIndex{};
    /** Rises on every claim, so it separates one host binding from the next on the same region. */
    std::uint64_t generation{};
};

/**
 * Claims or finds one host row bound to an exact source activity generation and region.
 * An unknown region never claims a row. A conflicting referenced row is never replaced.
 * @param groupSessionId Group session carried by the matching join descriptor.
 * @param source Exact source activity whose destination the target must copy.
 * @param regionIndex Concrete advertised region.
 * @param output Cleared, then receives the pending or ready row generation.
 * @return Current state of the requested row.
 */
[[nodiscard]] HostSessionState request_host_session(std::uint64_t groupSessionId,
                                                    const state::activity::SessionBinding& source,
                                                    std::int32_t regionIndex,
                                                    HostSessionBinding& output) noexcept;

/** Copies a ready row by its exact group-session key. */
[[nodiscard]] bool host_session_for_group(std::uint64_t groupSessionId,
                                          HostSessionBinding& output) noexcept;

/** Copies a ready row by its allocated target activity-session id. */
[[nodiscard]] bool host_session_for_activity(std::uint64_t hostSessionId,
                                             HostSessionBinding& output) noexcept;

/** Retains one ready host-row generation against replacement or eviction. */
[[nodiscard]] bool retain_host_session(std::uint64_t generation) noexcept;

/** Releases one external retain on the exact host-row generation. */
void release_host_session(std::uint64_t generation) noexcept;

/** Copies every occupied host-session row. @param count Receives the copied row count. */
void snapshot_host_sessions(std::span<HostSessionRow> output, std::size_t& count) noexcept;

/**
 * Allocates pending source-bound targets and releases retired rows.
 * State calls run outside the host lock and advance State revisions, so this must not run inside a
 * staged push.
 */
void allocate_claimed_host_sessions() noexcept;

/** Returns every retained binding and allocated target to State, then clears the table. */
void reset_host_sessions() noexcept;

} // namespace sunrise::server::gameplay::group
