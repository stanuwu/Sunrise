#include "studio_world_link.h"

#include <Windows.h>

#include <algorithm>
#include <array>

#include "../../../middleware/studio/studio_mapping.h"
#include "../../hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../../hooks/world_objects/internal.h"
#include "studio_pose_link.h"
#include "studio_protocol.h"
#include "studio_world_protocol.h"
#include "studio_world_reader.h"

namespace sunrise::client::diagnostics::studio {
namespace {

namespace objects = hooks::world_objects;
/** Loaded bubbles and world identity change far more slowly than object poses. */
constexpr std::uint64_t kWorldInterval = 500;
/** A full placement scan starts at most every 50 ms, so the editor follows motion at 20 Hz. */
constexpr std::uint64_t kPlacementInterval = 50;
/** Placement reads yield after half a millisecond of the game frame. */
constexpr std::int64_t kFrameBudgetDivisor = 2000;
static_assert(kNativeSliceCapacity <= wire::kSliceCapacity);

HANDLE g_mapping{};
wire::SharedWorld* g_view{};
std::uint64_t g_nextConnect{}, g_nextWorld{}, g_nextPlacements{}, g_scanStarted{}, g_epoch{};
std::int64_t g_frameBudgetTicks{};
bool g_scanning{}, g_incompleteRegistry{};
bool g_worldKnown{}, g_worldPresent{}, g_clientInWorld{};
std::uint32_t g_publishedMap{}, g_publishedMapRoot{}, g_publishedScenario{};
std::size_t g_scanPosition{}, g_authoredCount{}, g_rowCount{}, g_publishedCount{};
std::uint32_t g_excluded{}, g_scanPlayer{objects::kNone};
std::uint32_t g_player{objects::kNone};
objects::SnapshotResult g_registry{};
std::array<objects::Instance, wire::kPlacementCapacity> g_candidates{};
std::array<objects::DynamicInstance, wire::kPlacementCapacity> g_dynamicCandidates{};
std::array<wire::Placement, wire::kPlacementCapacity> g_rows{};
/** Parent handles stay private; only the object link answers them, from the last publication. */
std::array<std::uint32_t, wire::kPlacementCapacity> g_parents{}, g_publishedParents{};

/** Discard unfinished work when its editor mapping disappears. */
void disconnect_world() noexcept {
    middleware::studio::close_editor_view(g_mapping, g_view);
    g_scanning = false;
    g_scanPosition = 0;
    g_publishedCount = 0;
    g_nextWorld = 0;
    g_nextPlacements = 0;
    g_worldKnown = false;
    g_worldPresent = false;
    g_clientInWorld = false;
}

/** The editor creates the mapping; the client only opens a compatible existing one. */
[[nodiscard]] bool connect_world() noexcept {
    g_view =
        middleware::studio::open_editor_view<wire::SharedWorld>(wire::kMappingFormat, g_mapping);
    if (g_view == nullptr) {
        return false;
    }
    // Studio reads clientVersion to explain a version mismatch.
    if (g_view->control.magic == wire::kMagic) {
        g_view->control.clientVersion = wire::kVersion;
    }
    if (!middleware::studio::editor_header_ok(
            g_view->control, wire::kMagic, wire::kVersion, wire::kMappingBytes)) {
        disconnect_world();
        return false;
    }
    return true;
}

/** A world change cancels old work without treating its empty result as an object deletion. */
void invalidate_placements(std::uint64_t now) noexcept {
    g_scanning = false;
    g_scanPosition = 0;
    g_nextPlacements = 0;
    g_publishedCount = 0;
    auto& target = g_view->placements;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&target.sequence));
    target.flags = 0;
    target.time = now;
    target.started = now;
    target.epoch = ++g_epoch;
    target.count = 0;
    target.total = 0;
    target.overflow = 0;
    target.excluded = 0;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&target.sequence));
}

/** World identity and the bootflow gate invalidate placement data together. */
void publish_world(std::uint64_t now, bool inWorld) noexcept {
    auto snapshot = read_world();
    if (!inWorld) {
        const bool available = snapshot.available;
        snapshot = {};
        snapshot.available = available;
    }
    const bool changed = !g_worldKnown || g_worldPresent != snapshot.present
                         || g_publishedMap != snapshot.mapDefinitionTag
                         || g_publishedMapRoot != snapshot.mapRootTag
                         || g_publishedScenario != snapshot.scenarioTag;
    set_pose_world(snapshot.present && inWorld, changed);
    if (changed) {
        invalidate_placements(now);
    }
    g_worldKnown = true;
    g_worldPresent = snapshot.present;
    g_clientInWorld = inWorld;
    g_publishedMap = snapshot.mapDefinitionTag;
    g_publishedMapRoot = snapshot.mapRootTag;
    g_publishedScenario = snapshot.scenarioTag;
    auto& target = g_view->world;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&target.sequence));
    target.flags =
        (snapshot.available ? wire::available : 0U) | (snapshot.present ? wire::present : 0U);
    target.time = now;
    target.mapDefinitionTag = snapshot.mapDefinitionTag;
    target.mapRootTag = snapshot.mapRootTag;
    target.mapNameHash = snapshot.mapNameHash;
    target.scenarioTag = snapshot.scenarioTag;
    target.currentSlice = snapshot.currentSlice;
    target.activity = snapshot.activityName;
    target.count = snapshot.loadedCount;
    for (std::size_t i = 0; i < snapshot.loadedCount; ++i) {
        const auto& source = snapshot.loaded[i];
        target.slices[i] = {source.bubbleHash, source.sliceIndex, source.residency, 0};
    }
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&target.sequence));
}

/** Only a complete, untruncated ledger snapshot authorizes missing-row removal in the editor. */
void publish_placements(std::uint64_t now) noexcept {
    auto& target = g_view->placements;
    const bool available = g_registry.status == objects::SnapshotStatus::ready;
    const bool truncated = g_registry.written < g_registry.total || g_registry.overflow != 0;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&target.sequence));
    target.flags = (available ? wire::registryAvailable : 0U)
                   | (available && !truncated && !g_incompleteRegistry ? wire::complete : 0U)
                   | (truncated ? wire::truncated : 0U);
    target.time = now;
    target.started = g_scanStarted;
    target.epoch = ++g_epoch;
    target.count = static_cast<std::uint32_t>(g_rowCount);
    target.total = static_cast<std::uint32_t>(g_registry.total);
    target.overflow = g_registry.overflow;
    target.excluded = g_excluded;
    std::copy_n(g_rows.begin(), g_rowCount, target.rows.begin());
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&target.sequence));
    std::copy_n(g_parents.begin(), g_rowCount, g_publishedParents.begin());
    g_publishedCount = g_rowCount;
}

/** Fills one row; a failed pose read keeps the row with transformValid clear. */
void write_row(wire::Placement& row,
               const objects::Instance& identity,
               const objects::DynamicInstance* dynamicIdentity,
               const PlacementSnapshot& sample,
               bool posed) noexcept {
    row = {};
    row.identity = identity.placementIdentity;
    row.objectListTag = identity.objectListTag;
    row.entryIndex = identity.entryIndex;
    row.handle = identity.handle;
    row.generation = identity.generation;
    if (dynamicIdentity != nullptr) {
        row.classTag = dynamicIdentity->classTag;
        row.flags = wire::dynamic;
    }
    if (sample.stateKnown) {
        row.flags |= wire::stateKnown | (sample.placedNotHidden ? wire::placedNotHidden : 0U);
    }
    if (sample.flagsValid) {
        row.datumFlags = sample.datumFlags;
        const bool attached = g_scanPlayer != objects::kNone
                              && sample.parentHandle != objects::kNone
                              && (sample.parentHandle & objects::kEntityIndexMask)
                                     == (g_scanPlayer & objects::kEntityIndexMask);
        row.flags |= attached ? wire::playerAttached : 0U;
    }
    if (posed) {
        row.position = sample.position;
        row.orientation = sample.orientation;
        row.scale = sample.scale;
        row.flags |= wire::transformValid | wire::scaleValid;
        row.time = GetTickCount64();
    }
}

/** Rows of destroyed or reused objects are left out and counted as excluded. */
void advance_placements(std::uint64_t now) noexcept {
    if (!g_scanning) {
        if (now < g_nextPlacements) {
            return;
        }
        const auto authored = objects::try_snapshot(g_candidates);
        const auto dynamic = objects::try_dynamic_snapshot(g_dynamicCandidates);
        if (authored.status == objects::SnapshotStatus::busy
            || dynamic.status == objects::SnapshotStatus::busy) {
            return;
        }
        g_authoredCount = authored.written;
        g_registry = authored;
        g_registry.total += dynamic.total;
        g_registry.written += std::min(dynamic.written, wire::kPlacementCapacity - g_authoredCount);
        g_registry.overflow += dynamic.overflow;
        g_incompleteRegistry = authored.status != objects::SnapshotStatus::ready
                               || dynamic.status != objects::SnapshotStatus::ready;
        if (dynamic.status == objects::SnapshotStatus::ready) {
            g_registry.status = objects::SnapshotStatus::ready;
        }
        g_scanStarted = now;
        g_scanPosition = 0;
        g_rowCount = 0;
        g_excluded = 0;
        g_scanPlayer = g_player;
        g_scanning = true;
    }
    LARGE_INTEGER begin{};
    QueryPerformanceCounter(&begin);
    while (g_scanPosition < g_registry.written) {
        const bool dynamic = g_scanPosition >= g_authoredCount;
        const auto* dynamicIdentity =
            dynamic ? &g_dynamicCandidates[g_scanPosition - g_authoredCount] : nullptr;
        const auto& identity =
            dynamicIdentity != nullptr ? dynamicIdentity->identity : g_candidates[g_scanPosition];
        ++g_scanPosition;
        PlacementSnapshot sample{};
        const bool posed = dynamicIdentity != nullptr ? read_dynamic(*dynamicIdentity, sample)
                                                      : read_placement(identity, sample);
        if (sample.gone) {
            ++g_excluded;
        } else {
            g_parents[g_rowCount] = sample.flagsValid ? sample.parentHandle : objects::kNone;
            write_row(g_rows[g_rowCount++], identity, dynamicIdentity, sample, posed);
        }
        LARGE_INTEGER current{};
        QueryPerformanceCounter(&current);
        if (current.QuadPart - begin.QuadPart >= g_frameBudgetTicks) {
            return;
        }
    }
    publish_placements(GetTickCount64());
    g_scanning = false;
    g_nextPlacements = now + kPlacementInterval;
}

} // namespace

/** One reader setup serves every later editor connection in this hook lifetime. */
bool initialize_world_link() noexcept {
    const bool result = initialize_world_reader();
    LARGE_INTEGER frequency{};
    QueryPerformanceFrequency(&frequency);
    g_frameBudgetTicks = std::max<std::int64_t>(1, frequency.QuadPart / kFrameBudgetDivisor);
    return result;
}

/** Placement reads stop at the frame budget and resume on the next poll. */
void poll_world_link() noexcept {
    const std::uint64_t now = GetTickCount64();
    if (g_view == nullptr && now >= g_nextConnect) {
        g_nextConnect = now + kConnectInterval;
        (void)connect_world();
    }
    if (g_view == nullptr) {
        return;
    }
    if (!middleware::studio::editor_alive(g_view->control.heartbeat, now, kHeartbeatTimeout)) {
        disconnect_world();
        return;
    }
    const bool inWorld = hooks::bootflow::in_world();
    if (now >= g_nextWorld || inWorld != g_clientInWorld) {
        publish_world(now, inWorld);
        g_nextWorld = now + kWorldInterval;
    }
    if (g_worldPresent && inWorld) {
        advance_placements(now);
    }
}

void drop_world_link() noexcept {
    disconnect_world();
}

/** Closing the diagnostic channel releases mappings and all resolved reader targets. */
void close_world_link() noexcept {
    disconnect_world();
    clear_world_reader();
    g_nextConnect = 0;
}

/** The next scan start takes this value. */
void set_world_player(std::uint32_t handle) noexcept {
    g_player = handle;
}

/** Reads only the last completed publication, never a scan in progress. */
Lookup find_published_object(std::uint32_t handle,
                             std::uint32_t generation,
                             PublishedObject& output,
                             std::span<std::array<std::uint32_t, 2>> children,
                             std::size_t& childCount) noexcept {
    childCount = 0;
    if (g_view == nullptr || g_publishedCount == 0) {
        return Lookup::unavailable;
    }
    const auto& rows = g_view->placements.rows;
    Lookup result = Lookup::absent;
    for (std::size_t i = 0; i < g_publishedCount; ++i) {
        const auto& row = rows[i];
        if (row.handle == handle && row.generation == generation) {
            output.identity = {
                row.identity, row.objectListTag, row.entryIndex, row.handle, row.generation};
            output.classTag = row.classTag;
            output.dynamic = (row.flags & wire::dynamic) != 0;
            result = Lookup::found;
        }
        if (g_publishedParents[i] == handle) {
            if (childCount < children.size()) {
                children[childCount] = {row.handle, row.generation};
            }
            ++childCount;
        }
    }
    return result;
}

} // namespace sunrise::client::diagnostics::studio
