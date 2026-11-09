#include "studio_pose_link.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <intrin.h>
#include <span>
#include <string_view>

#include "../../../middleware/studio/studio_mapping.h"
#include "../../executable/image.h"
#include "../../hooking/detour.h"
#include "../../hooks/world_objects/internal.h"
#include "../../memory/current_process_memory.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/registry.h"
#include "../../patterns/signature_text.h"
#include "studio_pose_capture.h"
#include "studio_protocol.h"

namespace sunrise::client::diagnostics::studio {
namespace {

namespace wire = pose_wire;
/** The producer and primary caller patterns each match one function in the supported build. */
constexpr std::string_view kProducerText =
    "4C 8B DC 55 41 54 41 56 49 8D AB C8 FC FF FF 48 81 EC 20 04 00 00 "
    "48 8B 05 ? ? ? ? 48 33 C4 48 89 85 B0 02 00 00 0F 10 41 18";
constexpr std::string_view kCallerText =
    "4C 8B C7 48 8B CE E8 ? ? ? ? 84 DB 74 ? 48 8B 5F 70 48 85 DB 74 ?";
/** The producer's first handle decode, ending in its load of the handle-table slot. */
constexpr std::string_view kTablesLoadText =
    "8B 54 24 68 33 F6 83 FA FF 0F 84 ? ? ? ? 48 8B 5C 24 70 8B C2 C1 F8 0D 81 E2 FF 1F 00 00 "
    "8B C8 48 81 C9 00 00 FC 0F 0F B7 C0 48 C1 E9 12 48 23 C8 48 8B 05 ? ? ? ?";
constexpr auto kProducer =
    patterns::signature<patterns::signature_length(kProducerText)>(kProducerText);
constexpr auto kCaller = patterns::signature<patterns::signature_length(kCallerText)>(kCallerText);
constexpr auto kTablesLoad =
    patterns::signature<patterns::signature_length(kTablesLoadText)>(kTablesLoadText);
constexpr std::array kPatterns{patterns::Pattern{"studio_pose_producer", kProducer},
                               patterns::Pattern{"studio_pose_primary", kCaller},
                               patterns::Pattern{"studio_pose_tables_load", kTablesLoad}};
/** The primary CALL occupies bytes 6..10; the slot load's operand occupies bytes 53..56. */
constexpr std::size_t kPrimaryCall = 6, kPrimaryReturn = 11;
constexpr std::size_t kTablesOperand = 53, kTablesNext = 57;
/** A capture starts at most every 50 ms, so the editor follows animation at 20 Hz. */
constexpr std::uint64_t kSampleInterval = 50;
using Producer = char(__fastcall*)(std::uintptr_t, void*, std::uintptr_t, void*, void*, int);

/** Draws of one capture window; observers fill it under the shared stage lock. */
struct Stage final {
    std::atomic_uint32_t draws{}, vectors{}, dropped{};
    /** Studio's owner filter, copied when the window opens so observers never read the mapping. */
    std::array<std::uint32_t, wire::kOwnerWords> owners{};
    std::array<wire::Draw, wire::kDrawCapacity> rows{};
    std::array<wire::Vector, wire::kVectorCapacity> palette{};
};

SRWLOCK g_stageLock{SRWLOCK_INIT};
hooking::detour::Handle g_hook{};
volatile LONG g_active{};
std::atomic_bool g_enabled{false}, g_capturing{false};
std::uintptr_t g_primaryReturn{}, g_tablesSlot{};
HANDLE g_mapping{};
wire::SharedPose* g_view{};
Stage g_stage{};
std::uint64_t g_nextConnect{}, g_nextSample{};
bool g_worldPresent{}, g_pending{}, g_published{};

#if defined(SUNRISE_POSE_OBSERVER_TEST)
volatile LONG g_pauseIngress{}, g_ingressEntered{}, g_releaseIngress{1};
volatile LONG g_pauseFinally{}, g_finallyEntered{}, g_releaseFinally{1};
/** A failed fixture cannot keep an owned callback spinning forever. */
constexpr unsigned kPauseAttempts = 1'000'000'000;
#endif

/** The sequence covers only Studio-owned bytes, never a retained frame allocation. */
void clear_snapshot() noexcept {
    g_published = false;
    if (g_view == nullptr) {
        return;
    }
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_view->snapshot.sequence));
    g_view->snapshot.valid = 0;
    g_view->snapshot.time = 0;
    g_view->snapshot.drawCount = 0;
    g_view->snapshot.vectorCount = 0;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_view->snapshot.sequence));
}

/** Observers never read the mapping, so it can close while a draw is staged. */
void disconnect() noexcept {
    g_capturing.store(false, std::memory_order_release);
    g_pending = false;
    g_published = false;
    middleware::studio::close_editor_view(g_mapping, g_view);
}

/** @return True only when an editor created the matching pose protocol. */
[[nodiscard]] bool connect() noexcept {
    g_view =
        middleware::studio::open_editor_view<wire::SharedPose>(wire::kMappingFormat, g_mapping);
    if (g_view == nullptr) {
        return false;
    }
    // Studio reads clientVersion to explain a version mismatch.
    if (g_view->control.magic == wire::kMagic) {
        g_view->control.clientVersion = wire::kVersion;
    }
    if (!middleware::studio::editor_header_ok(
            g_view->control, wire::kMagic, wire::kVersion, sizeof(wire::SharedPose))) {
        disconnect();
        return false;
    }
    return true;
}

/** Copies one completed draw into the open window; the caller holds the shared stage lock. */
void stage_draw(std::uintptr_t draw,
                std::uintptr_t pose,
                PoseObjectPool pool,
                const PoseIdentity& before) noexcept {
    const content::handles::Source source{g_tablesSlot, nullptr, &memory::copy_current_process};
    wire::Draw row{};
    PoseIdentity current{};
    std::uintptr_t palette = 0;
    if (!describe_draw(source, draw, pose, row, palette)
        || !describe_pose_identity(source, pool, draw, current) || current != before) {
        return;
    }
    row.ownerGeneration = before.ownerGeneration;
    row.componentGeneration = before.componentGeneration;
    const std::uint32_t owner = row.owner & wire::kSlotMask;
    if ((g_stage.owners[owner / wire::kOwnerWordBits] & (1U << (owner % wire::kOwnerWordBits)))
        == 0) {
        return;
    }
    if (g_stage.vectors.load(std::memory_order_relaxed) >= wire::kVectorCapacity) {
        g_stage.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const std::uint32_t first = g_stage.vectors.fetch_add(row.count, std::memory_order_relaxed);
    const std::uint32_t slot = g_stage.draws.fetch_add(1, std::memory_order_relaxed);
    if (slot >= wire::kDrawCapacity) {
        g_stage.dropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    row.first = first;
    // A taken slot is always written, so it never shows a draw from an earlier window.
    if (first > wire::kVectorCapacity - row.count
        || !memory::copy_current_process(
            nullptr,
            palette,
            std::as_writable_bytes(std::span(g_stage.palette.data() + first, row.count)))
        || !describe_pose_identity(source, pool, draw, current) || current != before) {
        g_stage.dropped.fetch_add(1, std::memory_order_relaxed);
        row = {};
    }
    g_stage.rows[slot] = row;
}

/** Registry shutdown revokes new observations without holding its lock during native work. */
[[nodiscard]] bool object_pool(PoseObjectPool& output) noexcept {
    namespace objects = hooks::world_objects;
    if (!TryAcquireSRWLockShared(&objects::g_lock)) {
        return false;
    }
    const bool accepting = objects::g_accepting.load(std::memory_order_acquire);
    output = {reinterpret_cast<std::uintptr_t>(objects::g_datumBaseStorage),
              reinterpret_cast<std::uintptr_t>(objects::g_datumStrideStorage)};
    ReleaseSRWLockShared(&objects::g_lock);
    return accepting && output.baseSlot != 0 && output.strideSlot != 0;
}

/** Identity preflight cannot skip the producer; caller is the original return address. */
__declspec(noinline) char __fastcall observe_body(std::uintptr_t caller,
                                                  std::uintptr_t draw,
                                                  void* flags,
                                                  std::uintptr_t pose,
                                                  void* allocator,
                                                  void* corrections,
                                                  int pass) noexcept {
    PoseObjectPool pool{};
    PoseIdentity identity{};
    bool identified = false;
    const bool primary = caller == g_primaryReturn;
    if (primary && g_capturing.load(std::memory_order_acquire)
        && TryAcquireSRWLockShared(&g_stageLock)) {
        const content::handles::Source source{g_tablesSlot, nullptr, &memory::copy_current_process};
        std::uint32_t owner = wire::kSlotMask;
        if (g_capturing.load(std::memory_order_acquire)
            && memory::copy_current_process(nullptr,
                                            draw + sizeof(std::uint32_t),
                                            std::as_writable_bytes(std::span(&owner, 1)))) {
            const auto slot = owner & wire::kSlotMask;
            if ((g_stage.owners[slot / wire::kOwnerWordBits]
                 & (1U << (slot % wire::kOwnerWordBits)))
                    != 0
                && object_pool(pool)) {
                identified = describe_pose_identity(source, pool, draw, identity);
            }
        }
        ReleaseSRWLockShared(&g_stageLock);
    }
    const auto original = reinterpret_cast<Producer>(g_hook.original);
    const char result = original(draw, flags, pose, allocator, corrections, pass);
    if (result != 0 && identified && g_capturing.load(std::memory_order_acquire)
        && TryAcquireSRWLockShared(&g_stageLock)) {
        if (g_capturing.load(std::memory_order_acquire)) {
            stage_draw(draw, pose, pool, identity);
        }
        ReleaseSRWLockShared(&g_stageLock);
    }
    return result;
}

#pragma code_seg(push, pose_guard_body, ".srpose")
/** Every borrowed native or helper call finishes before this protected decrement. */
__declspec(noinline) void egress() noexcept {
    (void)InterlockedDecrement(&g_active);
}

/** The original callsite and native faults cross this boundary without changing their meaning. */
__declspec(noinline) char __fastcall observe(std::uintptr_t draw,
                                             void* flags,
                                             std::uintptr_t pose,
                                             void* allocator,
                                             void* corrections,
                                             int pass) noexcept {
#if defined(SUNRISE_POSE_OBSERVER_TEST)
    if (InterlockedCompareExchange(&g_pauseIngress, 0, 1) == 1) {
        (void)InterlockedExchange(&g_ingressEntered, 1);
        for (unsigned attempt = 0; attempt < kPauseAttempts && g_releaseIngress == 0; ++attempt) {
            YieldProcessor();
        }
    }
#endif
    (void)InterlockedIncrement(&g_active);
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    char result{};
    __try {
        result = observe_body(caller, draw, flags, pose, allocator, corrections, pass);
    } __finally {
        egress();
#if defined(SUNRISE_POSE_OBSERVER_TEST)
        if (InterlockedCompareExchange(&g_pauseFinally, 0, 1) == 1) {
            (void)InterlockedExchange(&g_finallyEntered, 1);
            for (unsigned attempt = 0; attempt < kPauseAttempts && g_releaseFinally == 0;
                 ++attempt) {
                YieldProcessor();
            }
        }
#endif
    }
    return result;
}
#pragma code_seg(pop, pose_guard_body)

/** Detours calls this while peer threads are suspended, so it must not acquire a lock. */
[[nodiscard]] bool idle() noexcept {
    return InterlockedCompareExchange(&g_active, 0, 0) == 0;
}

/** Copies the closed window to the editor; the caller holds the exclusive stage lock. */
void publish(std::uint64_t now) noexcept {
    const std::uint32_t draws =
        std::min<std::uint32_t>(g_stage.draws.load(std::memory_order_relaxed), wire::kDrawCapacity);
    const std::uint32_t vectors = std::min<std::uint32_t>(
        g_stage.vectors.load(std::memory_order_relaxed), wire::kVectorCapacity);
    PoseObjectPool pool{};
    PoseOwner player{};
    const content::handles::Source source{g_tablesSlot, nullptr, &memory::copy_current_process};
    const auto requested = static_cast<std::uint32_t>(InterlockedCompareExchange(
        reinterpret_cast<volatile LONG*>(&g_view->control.player), 0, 0));
    const bool playerValid =
        object_pool(pool) && describe_pose_owner(source, pool, requested, player);
    auto& output = g_view->snapshot;
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&output.sequence));
    output.valid = 1;
    output.time = now;
    output.drawCount = draws;
    output.vectorCount = vectors;
    output.dropped = g_stage.dropped.load(std::memory_order_relaxed);
    output.playerValid = playerValid ? 1U : 0U;
    output.player = playerValid ? player.handle : wire::kNoOwner;
    output.playerGeneration = playerValid ? player.generation : wire::kNoOwner;
    std::copy_n(g_stage.rows.begin(), draws, output.draws.begin());
    std::copy_n(g_stage.palette.begin(), vectors, output.palette.begin());
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&output.sequence));
    g_published = true;
}

/**
 * A window collects the draws between two frame polls; the next poll publishes it. A publish
 * waits for observers still inside the window, so it may slip one frame.
 */
void advance_capture(std::uint64_t now, bool wanted) noexcept {
    if (g_capturing.exchange(false, std::memory_order_acq_rel)) {
        g_pending = true;
    }
    if (!wanted) {
        g_pending = false;
        if (g_published) {
            clear_snapshot();
        }
        return;
    }
    if (g_pending) {
        if (TryAcquireSRWLockExclusive(&g_stageLock)) {
            publish(now);
            ReleaseSRWLockExclusive(&g_stageLock);
            g_pending = false;
            g_nextSample = now + kSampleInterval;
        }
        return;
    }
    if (now >= g_nextSample && TryAcquireSRWLockExclusive(&g_stageLock)) {
        g_stage.draws.store(0, std::memory_order_relaxed);
        g_stage.vectors.store(0, std::memory_order_relaxed);
        g_stage.dropped.store(0, std::memory_order_relaxed);
        for (std::size_t i = 0; i < g_stage.owners.size(); ++i) {
            g_stage.owners[i] = static_cast<std::uint32_t>(InterlockedCompareExchange(
                reinterpret_cast<volatile LONG*>(&g_view->control.owners[i]), 0, 0));
        }
        ReleaseSRWLockExclusive(&g_stageLock);
        g_capturing.store(true, std::memory_order_release);
    }
}

} // namespace

/** Resolution failure leaves the optional observer detached. */
bool initialize_pose_link() noexcept {
    if (g_hook.attached) {
        g_enabled.store(true, std::memory_order_release);
        return true;
    }
    executable::ExecutableImage image{};
    if (!executable::inspect_main_module(image)) {
        return false;
    }
    std::array<patterns::ImageRange, executable::kPeSectionLimit> ranges{};
    for (std::size_t i = 0; i < image.count; ++i) {
        ranges[i].bytes = image.sections[i];
    }
    std::array<patterns::Match, kPatterns.size()> matches{};
    (void)patterns::resolve_all(std::span(ranges.data(), image.count), kPatterns, matches);
    for (const patterns::Match& match : matches) {
        if (match.status != patterns::MatchStatus::unique) {
            return false;
        }
    }
    const auto producer = reinterpret_cast<std::uintptr_t>(matches[0].address);
    const auto caller = reinterpret_cast<std::uintptr_t>(matches[1].address);
    std::int32_t displacement{};
    if (!memory::copy_current_process(
            nullptr, caller + kPrimaryCall + 1, std::as_writable_bytes(std::span(&displacement, 1)))
        || caller + kPrimaryReturn + displacement != producer) {
        return false;
    }
    g_tablesSlot = reinterpret_cast<std::uintptr_t>(patterns::resolve_relative(
        matches[2].address + kTablesOperand, matches[2].address + kTablesNext));
    g_primaryReturn = caller + kPrimaryReturn;
    const bool installed =
        hooking::detour::install({matches[0].address, reinterpret_cast<void*>(&observe)}, g_hook);
    g_enabled.store(installed, std::memory_order_release);
    return installed;
}

/** Frame polling owns the mapping and opens and closes each capture window. */
void poll_pose_link() noexcept {
    const std::uint64_t now = GetTickCount64();
    if (g_view == nullptr && now >= g_nextConnect) {
        g_nextConnect = now + kConnectInterval;
        (void)connect();
    }
    if (g_view == nullptr) {
        return;
    }
    auto& control = g_view->control;
    control.bridgeReady = g_enabled.load(std::memory_order_acquire) ? 1 : 0;
    if (!middleware::studio::editor_alive(control.heartbeat, now, kHeartbeatTimeout)) {
        disconnect();
        return;
    }
    advance_capture(
        now, control.enabled != 0 && g_worldPresent && g_enabled.load(std::memory_order_acquire));
}

void drop_pose_link() noexcept {
    disconnect();
}

/** A world change drops the open window, so no draw crosses into the next world. */
void set_pose_world(bool present, bool changed) noexcept {
    g_worldPresent = present;
    if (changed || !present) {
        g_capturing.store(false, std::memory_order_release);
        g_pending = false;
        clear_snapshot();
    }
}

/** Only the requested full handle and generation can supply a captured model list. */
std::size_t published_models(std::uint32_t owner,
                             std::uint32_t generation,
                             std::span<std::uint32_t> models,
                             std::uint64_t& time) noexcept {
    time = 0;
    if (g_view == nullptr || !g_published) {
        return 0;
    }
    const auto& snapshot = g_view->snapshot;
    const std::size_t draws = std::min<std::size_t>(snapshot.drawCount, wire::kDrawCapacity);
    return copy_pose_models(
        std::span(snapshot.draws).first(draws), {owner, generation}, snapshot.time, models, time);
}

/** An active observer keeps its trampoline attached but inert until a later teardown attempt. */
bool close_pose_link() noexcept {
    g_enabled.store(false, std::memory_order_release);
    clear_snapshot();
    if (g_view != nullptr) {
        g_view->control.bridgeReady = 0;
    }
    disconnect();
    g_worldPresent = false;
    if (g_hook.attached) {
        const std::array entries{
            hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&observe)}};
        // .srpose holds the observer and its compiler continuations.
        const std::array spans{hooking::detour::section_span(".srpose")};
        return hooking::detour::uninstall(g_hook, entries, spans, &idle)
               == hooking::detour::UninstallResult::removed;
    }
    return true;
}

} // namespace sunrise::client::diagnostics::studio
