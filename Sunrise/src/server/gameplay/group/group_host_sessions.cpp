#include "group_host_sessions.h"

#include <Windows.h>

#include <array>
#include <limits>
#include <memory>
#include <new>

#include "../../../middleware/bap/activity_message/replicate_membership.h"
#include "../../../state/activity/destination/activity_destination_public.h"
#include "../../../state/activity/runtime.h"
#include "../endpoint/gameplay_endpoint.h"
#include "../gameplay_log.h"

namespace sunrise::server::gameplay::group {

namespace {
constexpr std::size_t kSourceCapacity = core::network_capacity::kPlayers * 2;

/** One source-bound activity-host row owned by the fixed table. */
struct HostSession {
    HostSessionBinding binding{};
    /** Borrowed private generations; their live owners retain them and join rechecks State. */
    std::array<state::activity::SessionBinding, kSourceCapacity> sources{};
    HostSessionState state{HostSessionState::absent};
    std::uint64_t lastUse{};
    std::uint32_t references{};
    bool occupied{};
    bool publicRegion{};
};

// The table must outsize the directory a body carries, because the previous body's rows are still
// retained while the next one is built. A table sized to the directory refuses every new region.
static_assert(kHostSessionCapacity
              > middleware::bap::activity_message::replicate_membership::kCitizenCapacity);
// Every ready row holds one activity session, and the private session and the destination sources
// need slots of their own. A session table sized to the host table evicts the private record.
static_assert(state::activity::kSessionCapacity > kHostSessionCapacity);
/** Guards host rows and their deferred-retirement queue. Never held across a State call. */
SRWLOCK g_hostSessionLock{SRWLOCK_INIT};
std::array<HostSession, kHostSessionCapacity> g_hostSessions{};
std::array<HostSessionBinding, kHostSessionCapacity> g_retired{};
std::size_t g_retiredCount = 0;
std::uint64_t g_useStamp = 0;
std::uint64_t g_generation = 0;

/** Public content compatibility excludes the launch's nonce and travel provenance. */
[[nodiscard]] bool
compatible_destination(const state::activity::destination::DestinationSelection& a,
                       const state::activity::destination::DestinationSelection& b) noexcept {
    return a.packageName == b.packageName && a.packageNameLength == b.packageNameLength
           && a.activityIndex == b.activityIndex && a.elementIndex == b.elementIndex
           && a.hasElementIndex == b.hasElementIndex && a.arrivalBubbleHash == b.arrivalBubbleHash
           && a.hasArrivalBubbleHash == b.hasArrivalBubbleHash && a.spawnSetHash == b.spawnSetHash
           && a.hasSpawnSetHash == b.hasSpawnSetHash
           && a.arrivalBubbleOverride == b.arrivalBubbleOverride
           && a.hasArrivalBubbleOverride == b.hasArrivalBubbleOverride
           && a.sliceSetOverride == b.sliceSetOverride
           && a.hasSliceSetOverride == b.hasSliceSetOverride
           && a.spawnSetOverride == b.spawnSetOverride
           && a.hasSpawnSetOverride == b.hasSpawnSetOverride;
}

/** Removes expired borrowed generations without holding the host lock across a State call. */
void prune_sources() noexcept {
    for (std::size_t index = 0; index < g_hostSessions.size(); ++index) {
        std::array<state::activity::SessionBinding, kSourceCapacity> sources{};
        std::uint64_t generation{};
        AcquireSRWLockShared(&g_hostSessionLock);
        if (!g_hostSessions[index].occupied || !g_hostSessions[index].publicRegion) {
            ReleaseSRWLockShared(&g_hostSessionLock);
            continue;
        }
        sources = g_hostSessions[index].sources;
        generation = g_hostSessions[index].binding.generation;
        ReleaseSRWLockShared(&g_hostSessionLock);
        for (const auto& source : sources) {
            if (source.sessionId == 0 || state::activity::binding_matches(source)) {
                continue;
            }
            AcquireSRWLockExclusive(&g_hostSessionLock);
            auto& row = g_hostSessions[index];
            if (row.binding.generation == generation) {
                for (auto& candidate : row.sources) {
                    if (same_binding(candidate, source)) {
                        candidate = {};
                    }
                }
            }
            ReleaseSRWLockExclusive(&g_hostSessionLock);
        }
    }
}

/** Records one exact source once. The fixed State table bounds simultaneously valid sources. */
[[nodiscard]] bool remember_source(HostSession& row,
                                   const state::activity::SessionBinding& source) noexcept {
    for (const auto& candidate : row.sources) {
        if (same_binding(candidate, source)) {
            return true;
        }
    }
    for (auto& candidate : row.sources) {
        if (candidate.sessionId == 0) {
            candidate = source;
            return true;
        }
    }
    return false;
}

/** Moves one unreferenced occupied row to deferred retirement. The caller holds the lock. */
[[nodiscard]] bool retire_locked(HostSession& row) noexcept {
    if (!row.occupied) {
        return true;
    }
    if (row.references != 0 || g_retiredCount == g_retired.size()) {
        return false;
    }
    g_retired[g_retiredCount] = row.binding;
    ++g_retiredCount;
    row = {};
    return true;
}

/** Releases both State retains and the allocated target of one retired row. */
void release_retired(const HostSessionBinding& binding) noexcept {
    if (binding.target.sessionId != state::activity::kAbsentSessionId) {
        state::activity::release_binding(binding.target);
        const bool released = state::activity::release_session(binding.target.sessionId);
        report(core::log::Level::info,
               "ev=gameplay stage=activityhost result=retired session=0x%llX generation=%llu "
               "state=%s",
               static_cast<unsigned long long>(binding.target.sessionId),
               static_cast<unsigned long long>(binding.generation),
               released ? "freed" : "retained");
    }
    state::activity::release_binding(binding.source);
}

/** Releases every deferred row. Callers hold no lock. */
void free_retired_host_sessions() noexcept {
    AcquireSRWLockShared(&g_hostSessionLock);
    const bool pending = g_retiredCount != 0;
    ReleaseSRWLockShared(&g_hostSessionLock);
    if (!pending) {
        return;
    }
    using Retired = std::array<HostSessionBinding, kHostSessionCapacity>;
    auto retired = std::unique_ptr<Retired>(new (std::nothrow) Retired{});
    if (!retired) {
        return; // Keep the owned queue intact for the next service slice.
    }
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_hostSessionLock);
    *retired = g_retired;
    count = g_retiredCount;
    for (auto& row : g_retired) {
        row = {};
    }
    g_retiredCount = 0;
    ReleaseSRWLockExclusive(&g_hostSessionLock);
    for (std::size_t index = 0; index < count; ++index) {
        release_retired((*retired)[index]);
    }
}

/** Copies one ready row selected by a predicate, then validates both retained State bindings. */
template <typename Predicate>
[[nodiscard]] bool find_ready(Predicate predicate, HostSessionBinding& output) noexcept {
    output = {};
    AcquireSRWLockShared(&g_hostSessionLock);
    for (const HostSession& row : g_hostSessions) {
        if (row.occupied && row.state == HostSessionState::ready && predicate(row.binding)) {
            output = row.binding;
            break;
        }
    }
    ReleaseSRWLockShared(&g_hostSessionLock);
    if (output.generation == 0 || !state::activity::binding_matches(output.source)
        || !state::activity::binding_matches(output.target)) {
        output = {};
        return false;
    }
    return true;
}

} // namespace

/** Claims or finds one source-bound activity-host row. */
HostSessionState request_host_session(std::uint64_t groupSessionId,
                                      const state::activity::SessionBinding& source,
                                      std::int32_t regionIndex,
                                      HostSessionBinding& output,
                                      bool publicRegion) noexcept {
    output = {};
    if (groupSessionId == 0 || regionIndex < 0 || !state::activity::retain_binding(source)) {
        return HostSessionState::absent;
    }
    if (publicRegion) {
        prune_sources();
    }

    HostSessionState result = HostSessionState::full;
    bool sourceTransferred = false;
    AcquireSRWLockExclusive(&g_hostSessionLock);

    HostSession* matching = nullptr;
    for (HostSession& row : g_hostSessions) {
        if (row.occupied
            && (row.binding.groupSessionId == groupSessionId
                || (publicRegion && row.publicRegion && row.binding.regionIndex == regionIndex
                    && compatible_destination(row.binding.source.destination,
                                              source.destination)))) {
            matching = &row;
            break;
        }
    }
    if (matching != nullptr && matching->publicRegion == publicRegion
        && (same_binding(matching->binding.source, source)
            || (publicRegion
                && compatible_destination(matching->binding.source.destination,
                                          source.destination)))
        && matching->binding.regionIndex == regionIndex) {
        if (!publicRegion || remember_source(*matching, source)) {
            matching->lastUse = ++g_useStamp;
            output = matching->binding;
            result = matching->state;
        }
    } else if (matching != nullptr && matching->references != 0) {
        result = HostSessionState::conflict;
    } else {
        HostSession* target = matching;
        if (target == nullptr) {
            for (HostSession& row : g_hostSessions) {
                if (!row.occupied) {
                    target = &row;
                    break;
                }
            }
        }
        if (target == nullptr) {
            for (HostSession& row : g_hostSessions) {
                if (row.references == 0 && (target == nullptr || row.lastUse < target->lastUse)) {
                    target = &row;
                }
            }
        }

        // The peer's reason-4 and reason-5 checks read the descriptor a new claim replaces, so
        // the same group's old target is kept as `previous-activity`. Another group's row is not.
        const state::activity::SessionBinding previous =
            matching != nullptr ? matching->binding.target : state::activity::SessionBinding{};
        if (target != nullptr && retire_locked(*target)) {
            target->binding.previous = previous;
            target->binding.source = source;
            target->publicRegion = publicRegion;
            if (publicRegion) {
                target->sources[0] = source;
            }
            target->binding.groupSessionId = groupSessionId;
            // Rows are found by a nonzero generation, so the counter starts at one.
            target->binding.generation = ++g_generation;
            target->binding.regionIndex = regionIndex;
            // The row index picks the port, so a row keeps one port for its whole life and no two
            // live rows share one. That is what keeps their client channels apart.
            target->binding.port =
                endpoint::host_port(static_cast<std::size_t>(target - g_hostSessions.data()));
            target->state = HostSessionState::pending;
            target->lastUse = ++g_useStamp;
            target->occupied = true;
            output = target->binding;
            sourceTransferred = true;
            result = HostSessionState::pending;
        }
    }
    ReleaseSRWLockExclusive(&g_hostSessionLock);

    if (!sourceTransferred) {
        state::activity::release_binding(source);
    }
    if (result == HostSessionState::full) {
        report(core::log::Level::warn, "ev=gameplay stage=activityhost result=full");
    }
    return result;
}

void host_session_sources(std::uint64_t generation,
                          std::span<state::activity::SessionBinding> output,
                          std::size_t& count) noexcept {
    count = 0;
    std::array<state::activity::SessionBinding, kSourceCapacity> sources{};
    AcquireSRWLockShared(&g_hostSessionLock);
    for (const auto& row : g_hostSessions) {
        if (row.occupied && row.state == HostSessionState::ready
            && row.binding.generation == generation) {
            sources = row.sources;
            if (!row.publicRegion) {
                sources[0] = row.binding.source;
            }
            break;
        }
    }
    ReleaseSRWLockShared(&g_hostSessionLock);
    for (const auto& source : sources) {
        if (count < output.size() && state::activity::binding_matches(source)) {
            output[count++] = source;
        }
    }
}

/** Copies a ready row by its exact group-session key. */
bool host_session_for_group(std::uint64_t groupSessionId, HostSessionBinding& output) noexcept {
    output = {};
    return groupSessionId != 0
           && find_ready(
               [groupSessionId](const HostSessionBinding& binding) {
                   return binding.groupSessionId == groupSessionId;
               },
               output);
}

/** Copies a ready row by its allocated target activity-session id. */
bool host_session_for_activity(std::uint64_t hostSessionId, HostSessionBinding& output) noexcept {
    output = {};
    return hostSessionId != state::activity::kAbsentSessionId
           && find_ready(
               [hostSessionId](const HostSessionBinding& binding) {
                   return binding.target.sessionId == hostSessionId;
               },
               output);
}

/** Copies a ready row by its exact source generation and active region. */
bool host_session_for_source_region(const state::activity::SessionBinding& source,
                                    std::int32_t regionIndex,
                                    HostSessionBinding& output) noexcept {
    output = {};
    if (regionIndex < 0 || !state::activity::binding_matches(source)) {
        return false;
    }
    AcquireSRWLockShared(&g_hostSessionLock);
    for (const auto& row : g_hostSessions) {
        if (!row.occupied || row.state != HostSessionState::ready
            || row.binding.regionIndex != regionIndex) {
            continue;
        }
        bool matches = same_binding(row.binding.source, source);
        if (row.publicRegion) {
            for (const auto& candidate : row.sources) {
                matches |= same_binding(candidate, source);
            }
        }
        if (matches) {
            output = row.binding;
            break;
        }
    }
    ReleaseSRWLockShared(&g_hostSessionLock);
    if (output.generation == 0 || !state::activity::binding_matches(output.source)
        || !state::activity::binding_matches(output.target)) {
        output = {};
        return false;
    }
    return true;
}

/** Copies a ready row by its exact source generation and group-session key. */
bool host_session_for_source_group(const state::activity::SessionBinding& source,
                                   std::uint64_t groupSessionId,
                                   HostSessionBinding& output) noexcept {
    output = {};
    return groupSessionId != 0 && state::activity::binding_matches(source)
           && find_ready(
               [&source, groupSessionId](const HostSessionBinding& binding) {
                   return same_binding(binding.source, source)
                          && binding.groupSessionId == groupSessionId;
               },
               output);
}

/** Retains one ready host-row generation against replacement or eviction. */
bool retain_host_session(std::uint64_t generation) noexcept {
    if (generation == 0) {
        return false;
    }
    bool retained = false;
    AcquireSRWLockExclusive(&g_hostSessionLock);
    for (HostSession& row : g_hostSessions) {
        if (row.occupied && row.state == HostSessionState::ready
            && row.binding.generation == generation
            && row.references != (std::numeric_limits<decltype(row.references)>::max)()) {
            ++row.references;
            row.lastUse = ++g_useStamp;
            retained = true;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_hostSessionLock);
    return retained;
}

/** Releases one external retain on the exact host-row generation. */
void release_host_session(std::uint64_t generation) noexcept {
    if (generation == 0) {
        return;
    }
    AcquireSRWLockExclusive(&g_hostSessionLock);
    for (HostSession& row : g_hostSessions) {
        if (row.occupied && row.binding.generation == generation && row.references != 0) {
            --row.references;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_hostSessionLock);
}

/** Copies every occupied host-session row. */
void snapshot_host_sessions(std::span<HostSessionRow> output, std::size_t& count) noexcept {
    count = 0;
    AcquireSRWLockShared(&g_hostSessionLock);
    for (const HostSession& row : g_hostSessions) {
        if (!row.occupied || count >= output.size()) {
            continue;
        }
        output[count] = {row.binding.groupSessionId,
                         row.binding.target.sessionId,
                         row.binding.regionIndex,
                         row.binding.generation,
                         row.binding.port};
        ++count;
    }
    ReleaseSRWLockShared(&g_hostSessionLock);
}

/** Fills every pending host-session row with a source-bound target. */
void allocate_claimed_host_sessions() noexcept {
    free_retired_host_sessions();
    for (;;) {
        HostSessionBinding pending{};
        AcquireSRWLockShared(&g_hostSessionLock);
        for (const HostSession& row : g_hostSessions) {
            if (row.occupied && row.state == HostSessionState::pending) {
                pending = row.binding;
                break;
            }
        }
        ReleaseSRWLockShared(&g_hostSessionLock);
        if (pending.generation == 0) {
            return;
        }

        // A public region runs its destination's free-roam activity. The private Bubble Host row,
        // keyed by this machine, runs the source activity.
        state::activity::destination::DestinationSelection destination = pending.source.destination;
        if (pending.groupSessionId != endpoint::identity().machineId
            && !state::activity::destination::public_destination(pending.source.destination,
                                                                 destination)) {
            report(core::log::Level::info,
                   "ev=group stage=public_activity result=no_free_roam source=%d",
                   static_cast<int>(pending.source.destination.activityIndex));
        }

        std::uint64_t sessionId = state::activity::kAbsentSessionId;
        state::activity::PendingAllocation allocation{};
        // The commit compares one process-wide State revision, so a frame landing between the
        // prepare and the commit refuses this allocation. The next tick retries it.
        if (!state::activity::prepare_session(destination, sessionId, allocation)
            || !state::activity::commit(allocation)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=group stage=allocate result=retry");
            return;
        }

        state::activity::SessionBinding target{};
        if (!state::activity::snapshot_binding(sessionId, target)
            || !state::activity::retain_binding(target)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=group stage=retain result=fail");
            return;
        }

        bool stored = false;
        std::size_t occupied = 0;
        AcquireSRWLockExclusive(&g_hostSessionLock);
        for (HostSession& row : g_hostSessions) {
            if (!row.occupied) {
                continue;
            }
            ++occupied;
            if (row.state == HostSessionState::pending
                && row.binding.generation == pending.generation) {
                row.binding.target = target;
                row.state = HostSessionState::ready;
                row.lastUse = ++g_useStamp;
                stored = true;
                break;
            }
        }
        ReleaseSRWLockExclusive(&g_hostSessionLock);

        if (!stored) {
            state::activity::release_binding(target);
            static_cast<void>(state::activity::release_session(target.sessionId));
            return;
        }
        report(core::log::Level::info,
               "ev=gameplay stage=activityhost result=allocated session=0x%llX group=0x%016llX "
               "generation=%llu port=%u held=%zu activity=%d",
               static_cast<unsigned long long>(target.sessionId),
               static_cast<unsigned long long>(pending.groupSessionId),
               static_cast<unsigned long long>(pending.generation),
               static_cast<unsigned>(pending.port),
               occupied,
               static_cast<int>(destination.activityIndex));
    }
}

/** Returns every retained binding and allocated target to State, then clears the table. */
void reset_host_sessions() noexcept {
    using Released = std::array<HostSessionBinding, kHostSessionCapacity * 2>;
    auto released = std::unique_ptr<Released>(new (std::nothrow) Released{});
    if (!released) {
        report(core::log::Level::error,
               "ev=gameplay stage=activityhost result=reset_allocation_failed");
        return;
    }
    std::size_t count = 0;
    AcquireSRWLockExclusive(&g_hostSessionLock);
    for (const HostSession& row : g_hostSessions) {
        if (row.occupied) {
            (*released)[count] = row.binding;
            ++count;
        }
    }
    for (std::size_t index = 0; index < g_retiredCount; ++index) {
        (*released)[count] = g_retired[index];
        ++count;
    }
    // Clearing the whole scaled table creates a large temporary on the game's stack.
    for (auto& row : g_hostSessions) {
        row = {};
    }
    for (auto& row : g_retired) {
        row = {};
    }
    g_retiredCount = 0;
    ReleaseSRWLockExclusive(&g_hostSessionLock);

    for (std::size_t index = 0; index < count; ++index) {
        release_retired((*released)[index]);
    }
}

} // namespace sunrise::server::gameplay::group
