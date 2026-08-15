/**
 * Guard on the widget bitmap-reference setter. A reference that is not a tag passes the setter's
 * own `!= 0xFFFFFFFF` gate, decodes to a table slot that was never made, and faults the reader.
 * The guard puts the none sentinel in its place, so a bad reference costs one bitmap, not the
 * process.
 */

#include "bitmap_ref_guard.h"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"

namespace sunrise::client::hooks::bitmap {
namespace {

using SetBitmapRef = void(__fastcall*)(void*, void*);

hooking::detour::Handle g_handle{};
std::atomic<SetBitmapRef> g_original{nullptr};
std::atomic<unsigned> g_reported{0};
std::atomic<unsigned> g_seenCount{0};
std::array<std::atomic<std::uint32_t>, kMaxSeenReports> g_seen{};

/**
 * Reports each new reference once, so a boot shows whether a bitmap ever reaches a widget. The
 * table is small and append-only, so a linear scan beats an index.
 */
void observe(std::uint32_t reference) noexcept {
    const unsigned count = g_seenCount.load(std::memory_order_acquire);
    for (unsigned index = 0; index < count && index < g_seen.size(); ++index) {
        if (g_seen[index].load(std::memory_order_relaxed) == reference) {
            return;
        }
    }
    if (count >= g_seen.size()) {
        return;
    }
    g_seen[count].store(reference, std::memory_order_relaxed);
    g_seenCount.store(count + 1, std::memory_order_release);
    std::array<char, kReportCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bitmap stage=seen handle=0x%08X result=ok",
                                      static_cast<unsigned>(reference));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** @return True when the reference is in tag space. */
bool looks_like_tag(std::uint32_t reference) noexcept {
    const std::uint32_t top = reference >> kTopByteShift;
    return top == kTagTopByteLow || top == kTagTopByteHigh;
}

/** @param triple Rejected triple, with the reference word already replaced. */
void report(const std::uint32_t (&triple)[BitmapRefWords::count],
            std::uint32_t reference) noexcept {
    const unsigned emitted = g_reported.load(std::memory_order_relaxed);
    if (emitted >= kMaxReports) {
        return;
    }
    g_reported.store(emitted + 1, std::memory_order_relaxed);
    std::array<char, kReportCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bitmap stage=ref_guard handle=0x%08X tag=0x%08X"
                                      " state=%u result=sentinel",
                                      static_cast<unsigned>(reference),
                                      static_cast<unsigned>(triple[BitmapRefWords::tag]),
                                      static_cast<unsigned>(triple[BitmapRefWords::state]));
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Puts the none sentinel in place of a reference that is not a tag. The caller owns its triple, so
 * the sentinel goes in a copy and the original bytes are never written.
 * @param self Borrowed widget the setter writes into.
 * @param ref Borrowed reference triple.
 */
__declspec(noinline) void __fastcall setter(void* self, void* ref) noexcept {
    const SetBitmapRef original = g_original.load(std::memory_order_acquire);
    if (original == nullptr) {
        return;
    }
    if (ref == nullptr) {
        original(self, ref);
        return;
    }
    std::uint32_t triple[BitmapRefWords::count]{};
    std::memcpy(triple, ref, sizeof triple);
    const std::uint32_t reference = triple[BitmapRefWords::reference];
    observe(reference);
    if (reference == kNoneReference || looks_like_tag(reference)) {
        original(self, ref);
        return;
    }
    triple[BitmapRefWords::reference] = kNoneReference;
    original(self, triple);
    report(triple, reference);
}

/** @param reason Key naming the step that failed. @return False, for a direct return. */
bool fail_install(const char* reason) noexcept {
    std::array<char, kReportCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=bitmap stage=ref_guard result=fail reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    return false;
}

} // namespace

/** Finds the setter and attaches the guard. */
bool install_bitmap_ref_guard() noexcept {
    if (g_handle.attached) {
        return true;
    }
    std::byte* const target = scan_main_image_unique(kSetterSignature, "bitmap_set_ref");
    if (target == nullptr) {
        return fail_install("target");
    }
    const hooking::detour::Spec spec{target, reinterpret_cast<void*>(&setter)};
    if (!hooking::detour::install(spec, g_handle)) {
        return fail_install("attach");
    }
    g_original.store(reinterpret_cast<SetBitmapRef>(g_handle.original), std::memory_order_release);
    core::log::write(
        core::log::Channel::client, core::log::Level::info, "ev=bitmap stage=ref_guard result=ok");
    return true;
}

/** Detaches the bitmap-reference guard and drops its trampoline. */
void uninstall_bitmap_ref_guard() noexcept {
    if (g_handle.attached) {
        (void)hooking::detour::uninstall(g_handle);
    }
    g_original.store(nullptr, std::memory_order_release);
    g_reported.store(0, std::memory_order_release);
    g_seenCount.store(0, std::memory_order_release);
    for (std::atomic<std::uint32_t>& entry : g_seen) {
        entry.store(0, std::memory_order_relaxed);
    }
}

} // namespace sunrise::client::hooks::bitmap
