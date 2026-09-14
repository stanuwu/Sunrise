#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../state/activity/definition.h"

namespace sunrise::server::gameplay::group {

/** Region sentinel that cannot claim or replace a host-session row. */
inline constexpr std::int32_t kUnknownRegion = -1;

/**
 * Regions that may hold an activity-host session at once.
 * Twice the host directory one membership body can carry, so replacing the whole advertised set
 * always finds free rows while the previous set is still retained.
 */
inline constexpr std::size_t kHostSessionCapacity = core::network_capacity::kHostPorts;

/** Immutable identity of one activity-host generation and its original destination source. */
struct HostSessionBinding {
    state::activity::SessionBinding source{};
    state::activity::SessionBinding target{};
    /** Target this row's group held before its current claim. Absent on the first claim. */
    state::activity::SessionBinding previous{};
    std::uint64_t groupSessionId{};
    std::uint64_t generation{};
    std::int32_t regionIndex{kUnknownRegion};
    /** Port this row advertises. One per row, so no two live hosts share a client channel. */
    std::uint16_t port{};
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
    /** Port this row advertises. Two rows sharing one would share a client channel. */
    std::uint16_t port{};
};

/**
 * Claims or finds one region host. Private rows use the exact source activity generation;
 * authored public rows reuse compatible destination content across private launches.
 * An unknown region never claims a row. A conflicting referenced row is never replaced.
 * @param groupSessionId Group session carried by the matching join descriptor.
 * @param source Exact source activity. A public region's target runs its free-roam activity.
 * @param regionIndex Concrete advertised region.
 * @param output Cleared, then receives the pending or ready row generation.
 * @param publicRegion True only when the installed destination authors this region as public.
 * @return Current state of the requested row.
 */
[[nodiscard]] HostSessionState request_host_session(std::uint64_t groupSessionId,
                                                    const state::activity::SessionBinding& source,
                                                    std::int32_t regionIndex,
                                                    HostSessionBinding& output,
                                                    bool publicRegion = false) noexcept;

/** Copies live sources that advertised this public host; each join still validates its identity. */
void host_session_sources(std::uint64_t generation,
                          std::span<state::activity::SessionBinding> output,
                          std::size_t& count) noexcept;

/** Copies a ready row by its exact group-session key. */
[[nodiscard]] bool host_session_for_group(std::uint64_t groupSessionId,
                                          HostSessionBinding& output) noexcept;

/** Copies a ready row by its allocated target activity-session id. */
[[nodiscard]] bool host_session_for_activity(std::uint64_t hostSessionId,
                                             HostSessionBinding& output) noexcept;

/** Copies a ready row for its original source or a live source sharing the public region. */
[[nodiscard]] bool host_session_for_source_region(const state::activity::SessionBinding& source,
                                                  std::int32_t regionIndex,
                                                  HostSessionBinding& output) noexcept;

/** Copies a ready row by its exact source generation and group-session key. */
[[nodiscard]] bool host_session_for_source_group(const state::activity::SessionBinding& source,
                                                 std::uint64_t groupSessionId,
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
