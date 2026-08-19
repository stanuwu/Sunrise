#include "baseplate_composition.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <string_view>
#include <vector>

#include "../../client/hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../../client/hooks/spawn/spawn_runtime.h"
#include "../../client/hooks/teleport/runtime.h"
#include "../../client/movement/movement_settings_store.h"
#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../../middleware/content/packages/reader/reader.h"
#include "carrier_probe.h"
#include "custom_package_builder.h"

namespace sunrise::izanami::runtime::baseplate_composition {
namespace {

namespace bootflow = client::hooks::bootflow;
namespace native_spawn = client::hooks::spawn;
namespace teleport = client::hooks::teleport;
namespace package_reader = middleware::content::packages::reader;

/** Class of a spawnable entity definition in the installed package entry tables. */
constexpr std::uint32_t kEntityClass = 0x80809C0FU;
/** Native object type 1 is the loaded static-mesh path. */
constexpr std::uint8_t kStaticMeshType = 1;
/** Cosmetic-static entities are retained as a fallback when a carrier streams no type-1 entity. */
constexpr std::uint8_t kCosmeticStaticType = 4;
/** Networked-static entities are the final static fallback. */
constexpr std::uint8_t kNetworkedStaticType = 7;
/** Entity definitions tested per camera frame, keeping native resolver work bounded. */
constexpr std::size_t kCandidatesPerFrame = 64;
/**
 * A remote but stable coordinate outside the installed carriers' known spawn extents.
 * Keeping the workspace under 2^16 preserves sub-centimeter float precision while putting the
 * carrier beyond Destiny's ordinary scenery draw distance.
 */
constexpr teleport::Vector kWorkspaceOrigin{32768.0F, -32768.0F, 8192.0F};
/** Consecutive physics reads required before the carrier can no longer overwrite the move. */
constexpr std::uint32_t kRelocationStableFrames = 30;
/** Maximum time spent resisting an authored activity spawn or network correction. */
constexpr std::uint64_t kRelocationTimeoutMs = 8'000;
/** Squared distance accepted as the same physical position. */
constexpr float kRelocationToleranceSquared = 1.0F;
/** The platform is placed below the Guardian and enlarged into a useful authoring surface. */
constexpr float kPlatformLift = -2.0F;
constexpr float kPlatformScale = 24.0F;

SRWLOCK g_lock{SRWLOCK_INIT};
std::vector<std::uint32_t> g_entityTags{};
std::atomic_bool g_armed{false};
bool g_relocated{};
bool g_platformRequested{};
std::uint64_t g_relocationStarted{};
std::uint32_t g_relocationStableFrames{};
bool g_relocationRequested{};
std::size_t g_candidateCursor{};
std::uint32_t g_fallbackTag{};
std::uint8_t g_fallbackType{};

/** Reports one composition stage and its most useful native values. */
void report(const char* stage,
            const char* result,
            std::uint32_t tag = 0,
            std::uint8_t type = 0) noexcept {
    std::array<char, 256> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=izanami_baseplate_composition stage=%s result=%s "
                                      "tag=0x%08X type=%u origin=%.1f,%.1f,%.1f",
                                      stage,
                                      result,
                                      static_cast<unsigned>(tag),
                                      static_cast<unsigned>(type),
                                      static_cast<double>(kWorkspaceOrigin[0]),
                                      static_cast<double>(kWorkspaceOrigin[1]),
                                      static_cast<double>(kWorkspaceOrigin[2]));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         result == std::string_view{"ok"} ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Collects one entity tag from an installed package entry table. */
bool collect_entity(void* context, const package_reader::ClassEntry& entry) noexcept {
    auto& tags = *static_cast<std::vector<std::uint32_t>*>(context);
    tags.push_back(entry.tag);
    return true;
}

/** Builds the immutable candidate bank without resolving or instantiating any game object. */
[[nodiscard]] bool collect_entity_tags() noexcept {
    AcquireSRWLockShared(&g_lock);
    const bool cached = !g_entityTags.empty();
    ReleaseSRWLockShared(&g_lock);
    if (cached) {
        return true;
    }

    core::path::Buffer directory{};
    if (!core::path::module_directory(GetModuleHandleW(nullptr), directory)
        || !core::path::append(directory, L"\\packages")) {
        report("catalog", "package_directory_missing");
        return false;
    }

    std::vector<std::uint32_t> tags{};
    package_reader::ScanResult scan{};
    const bool scanned = package_reader::scan_class_entries(
        directory.chars.data(), kEntityClass, &collect_entity, &tags, scan);
    package_reader::release_caches();
    if (!scanned || tags.empty()) {
        report("catalog", "scan_failed");
        return false;
    }
    std::sort(tags.begin(), tags.end());
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    AcquireSRWLockExclusive(&g_lock);
    g_entityTags = std::move(tags);
    ReleaseSRWLockExclusive(&g_lock);
    report("catalog", "ok");
    return true;
}

/** Enables movement ownership before the Guardian is moved outside the carrier map. */
void enable_isolated_navigation() noexcept {
    client::movement::Settings movement = client::movement::get();
    movement.noclipEnabled = true;
    movement.flyEnabled = true;
    if (movement.flySpeed < 25.0F) {
        movement.flySpeed = 25.0F;
    }
    (void)client::movement::publish(movement);
}

/**
 * Tests a bounded slice of installed entity tags against the carrier's live residency table.
 * @return A static tag once one is available, or zero while the search should continue.
 */
[[nodiscard]] std::uint32_t find_platform_tag(std::uint8_t& selectedType) noexcept {
    selectedType = 0;
    AcquireSRWLockShared(&g_lock);
    const std::size_t end =
        (std::min)(g_candidateCursor + kCandidatesPerFrame, g_entityTags.size());
    for (; g_candidateCursor < end; ++g_candidateCursor) {
        const std::uint32_t tag = g_entityTags[g_candidateCursor];
        std::uint8_t type = 0;
        if (!native_spawn::object_type(tag, type)) {
            continue;
        }
        if (type == kStaticMeshType) {
            ++g_candidateCursor;
            selectedType = type;
            ReleaseSRWLockShared(&g_lock);
            return tag;
        }
        if (g_fallbackTag == 0 && (type == kCosmeticStaticType || type == kNetworkedStaticType)) {
            g_fallbackTag = tag;
            g_fallbackType = type;
        }
    }
    const bool finished = g_candidateCursor >= g_entityTags.size();
    ReleaseSRWLockShared(&g_lock);
    if (finished && g_fallbackTag != 0) {
        selectedType = g_fallbackType;
        return g_fallbackTag;
    }
    return 0;
}

} // namespace

/** Prepares an isolated native baseplate composition for the next world arrival. */
bool arm() noexcept {
    carrier_probe::inspect("vfx_shade_test", "map:pandora:root");
    const bool packageStaged = custom_package_builder::stage_map_root("map:pandora:root");
    g_relocated = false;
    g_platformRequested = false;
    g_relocationStarted = 0;
    g_relocationStableFrames = 0;
    g_relocationRequested = false;
    g_candidateCursor = 0;
    g_fallbackTag = 0;
    g_fallbackType = 0;
    g_armed.store(false, std::memory_order_release);
    report("package_stage", packageStaged ? "ok" : "unavailable");
    return packageStaged;
}

/** Cancels an armed composition. */
void disarm() noexcept {
    g_armed.store(false, std::memory_order_release);
    g_relocated = false;
    g_platformRequested = false;
    g_relocationStarted = 0;
    g_relocationStableFrames = 0;
    g_relocationRequested = false;
    g_candidateCursor = 0;
    g_fallbackTag = 0;
    g_fallbackType = 0;
}

/**
 * Blank Baseplate currently performs no in-world mutation. A stock activity is not a Forge map,
 * and moving the player beyond its world bounds can trigger the activity's kill volume.
 */
void poll() noexcept {
    (void)bootflow::in_world();
}

} // namespace sunrise::izanami::runtime::baseplate_composition
