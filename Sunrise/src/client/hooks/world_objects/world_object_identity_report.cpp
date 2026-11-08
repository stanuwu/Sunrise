// Placement identity and allocation reporting for the world-object hooks.
// Every helper here takes g_lock exclusively itself, so no caller may already hold it.

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <intrin.h>
#include <span>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808099.h"
#include "../../memory/current_process_memory.h"
#include "dynamic_object_registry.h"
#include "internal.h"
#include "world_object_registry.h"

namespace sunrise::client::hooks::world_objects {
namespace {

/** Stack frames reported above one off-ledger allocation. */
constexpr std::size_t kFrameCount = 4;

/** Set while this thread is inside the instantiate detour, whose allocation is already reported. */
thread_local bool t_inInstantiate{};

/**
 * Reads the placed entry's authored world position.
 * Two entries at one position under different object lists are the same authored prop placed
 * twice, which an identity-keyed count cannot see because each source carries its own identity.
 */
[[nodiscard]] bool read_entry_position(const void* entry, std::array<float, 3>& output) noexcept {
    if (entry == nullptr) {
        return false;
    }
    return memory::read_current_process(
        nullptr,
        reinterpret_cast<std::uintptr_t>(entry)
            + offsetof(middleware::content::packages::schema::generated::AuthoredPlacementEntry,
                       position),
        std::as_writable_bytes(std::span(output)));
}

/**
 * Reports one object built by a caller that named no placed entry.
 * Those callers pass {-1,-1}, so the registry cannot retain them and nothing else here sees them.
 * A second copy of authored content that the placed path builds once must come from here.
 */
void report_dynamic(std::uint32_t handle, const void* entry, std::uintptr_t caller) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    ++g_dynamicCount;
    const std::uint64_t ordinal = g_dynamicCount;
    const bool report = g_dynamicReportBudget > 0;
    if (report) {
        --g_dynamicReportBudget;
    }
    const std::size_t track = static_cast<std::size_t>(ordinal - 1) % kDynamicHandleCapacity;
    g_dynamicHandles[track] = handle;
    g_dynamicOrdinals[track] = ordinal;
    ReleaseSRWLockExclusive(&g_lock);
    if (!report) {
        return;
    }
    std::array<float, 3> position{};
    const bool positionRead = read_entry_position(entry, position);
    std::array<char, 224> line{};
    const int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=world_object stage=placement result=dynamic "
        "handle=0x%08X ordinal=%llu pos=%.3f,%.3f,%.3f "
        "caller=+0x%llX",
        handle,
        static_cast<unsigned long long>(ordinal),
        positionRead ? static_cast<double>(position[0]) : 0.0,
        positionRead ? static_cast<double>(position[1]) : 0.0,
        positionRead ? static_cast<double>(position[2]) : 0.0,
        static_cast<unsigned long long>(caller >= g_moduleBase ? caller - g_moduleBase : caller));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Captures only placed-content calls; dynamic callers pass {-1,-1}. */
void observe_instance(std::uint32_t handle,
                      const void* entry,
                      std::int32_t objectListTag,
                      std::int32_t entryIndex,
                      std::uintptr_t caller) noexcept {
    if (handle == kNone || objectListTag == -1 || entryIndex == -1 || g_resolvePair == nullptr
        || !g_accepting.load(std::memory_order_acquire)) {
        return;
    }
    HandlePair pair{};
    __try {
        g_resolvePair(&pair, handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return;
    }
    if (pair.handle != handle || pair.generation == kNone) {
        return;
    }
    // Native init has already copied placed entry +0x70 into the datum at +0x90, so the identity
    // that survives a mirrored object list is readable here.
    DatumIdentity datum{};
    const bool datumRead = read_datum_identity(handle, datum) && datum.selfHandle == handle
                           && datum.objectListTag == static_cast<std::uint32_t>(objectListTag)
                           && datum.entryIndex == static_cast<std::uint32_t>(entryIndex);
    const Instance instance{datumRead ? datum.placementIdentity : 0,
                            static_cast<std::uint32_t>(objectListTag),
                            static_cast<std::uint32_t>(entryIndex),
                            handle,
                            pair.generation};
    AcquireSRWLockExclusive(&g_lock);
    g_instantiateCaller = caller;
    retain(instance);
    const bool report = has_placement_identity(instance) && g_identityReportBudget > 0;
    if (report) {
        --g_identityReportBudget;
    }
    const std::size_t live = g_liveCount;
    ReleaseSRWLockExclusive(&g_lock);
    if (!report) {
        return;
    }
    std::array<float, 3> position{};
    const bool positionRead = read_entry_position(entry, position);
    std::array<char, 256> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=world_object stage=placement result=observed "
                                      "list=0x%08X entry=%u identity=0x%016llX handle=0x%08X "
                                      "pos=%.3f,%.3f,%.3f live=%zu",
                                      instance.objectListTag,
                                      instance.entryIndex,
                                      static_cast<unsigned long long>(instance.placementIdentity),
                                      instance.handle,
                                      positionRead ? static_cast<double>(position[0]) : 0.0,
                                      positionRead ? static_cast<double>(position[1]) : 0.0,
                                      positionRead ? static_cast<double>(position[2]) : 0.0,
                                      live);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** @return The datum flag word at `+4`, or zero when it cannot be read. */
[[nodiscard]] std::uint32_t datum_flags(std::uint32_t handle) noexcept {
    if (g_datumBaseStorage == nullptr || g_datumStrideStorage == nullptr) {
        return 0;
    }
    const std::uintptr_t datum =
        *g_datumBaseStorage
        + static_cast<std::uintptr_t>(*g_datumStrideStorage) * (handle & 0x1FFFU);
    std::uint32_t flags = 0;
    if (!memory::read_current_process(
            nullptr, datum + 4U, std::span(reinterpret_cast<std::byte*>(&flags), sizeof flags))) {
        return 0;
    }
    return flags;
}

/**
 * Associates an allocation with its native creator and any active entity identity.
 * @param handle Native handle the allocator returned.
 * @param caller Return address, reported as an RVA so it names the creating function.
 */
void report_allocation(std::uint32_t handle, std::uintptr_t caller) noexcept {
    // The allocation site alone does not say which subsystem asked. Unwind names the frames.
    std::array<void*, kFrameCount> frames{};
    const USHORT captured =
        RtlCaptureStackBackTrace(2, static_cast<DWORD>(frames.size()), frames.data(), nullptr);
    AcquireSRWLockExclusive(&g_lock);
    const bool report = g_allocateReportBudget > 0;
    if (report) {
        --g_allocateReportBudget;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!report) {
        return;
    }
    if (t_entityGlue != kNone) {
        std::array<char, 192> identity{};
        const int length = std::snprintf(
            identity.data(),
            identity.size(),
            "ev=world_object stage=entity_create handle=0x%08X glue=0x%08X network=0x%08X slot=%u",
            handle,
            t_entityGlue,
            t_entityNetwork,
            t_entityNetwork & kEntityIndexMask);
        if (length > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::debug,
                             {identity.data(), static_cast<std::size_t>(length)});
        }
    }
    std::array<char, 192> line{};
    int written = std::snprintf(
        line.data(),
        line.size(),
        "ev=world_object stage=placement result=alloc handle=0x%08X flags=0x%08X caller=+0x%llX",
        handle,
        datum_flags(handle),
        static_cast<unsigned long long>(caller >= g_moduleBase ? caller - g_moduleBase : caller));
    for (USHORT index = 0; written > 0 && index < captured; ++index) {
        const auto frame = reinterpret_cast<std::uintptr_t>(frames[index]);
        const int appended = std::snprintf(
            line.data() + written,
            line.size() - static_cast<std::size_t>(written),
            " f%u=+0x%llX",
            static_cast<unsigned>(index),
            static_cast<unsigned long long>(frame >= g_moduleBase ? frame - g_moduleBase : frame));
        if (appended <= 0) {
            break;
        }
        written += appended;
    }
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports the teardown of one object the dynamic path built, and forgets it. */
void report_dynamic_destroy(std::uint32_t handle) noexcept {
    std::uint64_t ordinal = 0;
    AcquireSRWLockExclusive(&g_lock);
    for (std::size_t index = 0; index < kDynamicHandleCapacity; ++index) {
        if (g_dynamicHandles[index] == handle && g_dynamicOrdinals[index] != 0) {
            ordinal = g_dynamicOrdinals[index];
            g_dynamicHandles[index] = 0;
            g_dynamicOrdinals[index] = 0;
            break;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (ordinal == 0) {
        return;
    }
    std::array<char, 128> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=world_object stage=placement result=dynamic_destroyed "
                                      "handle=0x%08X ordinal=%llu",
                                      handle,
                                      static_cast<unsigned long long>(ordinal));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

std::atomic<Instantiate> g_instantiateOriginal{nullptr};
std::atomic<Destroy> g_destroyOriginal{nullptr};
std::atomic<Allocate> g_allocateOriginal{nullptr};
std::atomic<LogicalDestroy> g_logicalDestroyOriginal{nullptr};

/** Calls native construction first, then retains the successfully initialized identity. */
__declspec(noinline) std::uint32_t* __fastcall instantiate(std::uint32_t* output,
                                                           const void* entry,
                                                           std::int32_t objectListTag,
                                                           std::int32_t entryIndex) noexcept {
    ActiveCall active;
    const Instantiate original = g_instantiateOriginal.load(std::memory_order_acquire);
    t_inInstantiate = true;
    std::uint32_t* const result =
        original != nullptr ? original(output, entry, objectListTag, entryIndex) : output;
    t_inInstantiate = false;
    if (result != nullptr) {
        if (objectListTag == -1 || entryIndex == -1) {
            observe_dynamic(*result, entry);
            report_dynamic(*result, entry, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
        } else {
            observe_instance(*result,
                             entry,
                             objectListTag,
                             entryIndex,
                             reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
        }
    }
    return result;
}

/** Calls native allocation first, then reports it when the instantiate hook did not ask. */
__declspec(noinline) std::uint32_t* __fastcall allocate(std::uint32_t* output,
                                                        const void* entry,
                                                        std::int32_t objectListTag,
                                                        std::int32_t entryIndex) noexcept {
    ActiveCall active;
    const Allocate original = g_allocateOriginal.load(std::memory_order_acquire);
    std::uint32_t* const result =
        original != nullptr ? original(output, entry, objectListTag, entryIndex) : output;
    if (!t_inInstantiate && result != nullptr && *result != kNone
        && g_accepting.load(std::memory_order_acquire)) {
        if (objectListTag == -1) {
            observe_dynamic(*result, entry);
        }
        report_allocation(*result, reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    }
    return result;
}

/** Reports one logical destroy, the only teardown that releases the object's entity. */
__declspec(noinline) std::uintptr_t __fastcall logical_destroy(std::uint32_t handle) noexcept {
    ActiveCall active;
    const LogicalDestroy original = g_logicalDestroyOriginal.load(std::memory_order_acquire);
    AcquireSRWLockExclusive(&g_lock);
    const bool report = g_logicalDestroyReportBudget > 0;
    if (report) {
        --g_logicalDestroyReportBudget;
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (report && g_accepting.load(std::memory_order_acquire)) {
        std::array<char, 128> line{};
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=world_object stage=placement result=released handle=0x%08X "
                          "caller=+0x%llX",
                          handle,
                          static_cast<unsigned long long>(
                              reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - g_moduleBase));
        if (written > 0) {
            core::log::write(core::log::Channel::client,
                             core::log::Level::debug,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    return original != nullptr ? original(handle) : 0;
}

/** Removes the identity while its datum and generation still belong to this handle. */
__declspec(noinline) std::uintptr_t __fastcall destroy(std::uint32_t handle) noexcept {
    ActiveCall active;
    if (g_accepting.load(std::memory_order_acquire)) {
        erase_dynamic(handle);
        AcquireSRWLockExclusive(&g_lock);
        erase(handle);
        ReleaseSRWLockExclusive(&g_lock);
        report_dynamic_destroy(handle);
    }
    const Destroy original = g_destroyOriginal.load(std::memory_order_acquire);
    return original != nullptr ? original(handle) : 0;
}

} // namespace sunrise::client::hooks::world_objects
