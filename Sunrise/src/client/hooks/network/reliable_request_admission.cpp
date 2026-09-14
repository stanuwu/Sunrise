#include "reliable_request_admission.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "../../../core/logging/log.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"

namespace sunrise::client::hooks::reliable_requests {
namespace {
using Enqueue = void(__fastcall*)(void*, int, int, const void*, int*, char);
using Reset = void(__fastcall*)(void*);
constexpr int kRequestId = 39;
constexpr int kRequestSize = 0xAC18;
// Native header, one 0x934-byte response and its eight-byte packing alignment.
constexpr std::size_t kResponseRequestSize = 0x950;
using Request = std::array<std::byte, kResponseRequestSize>;
std::array<hooking::detour::Handle, 2> g_hooks{};
std::atomic<Enqueue> g_enqueue{};
std::atomic<Reset> g_reset{};
std::atomic_uint32_t g_calls{}, g_resets{};
std::atomic_uint64_t g_epoch{1};
std::uintptr_t g_queueVtable{};
SRWLOCK g_cacheLock = SRWLOCK_INIT;

template <typename T> T field(const std::byte* data, std::size_t offset) noexcept {
    T value;
    std::memcpy(&value, data + offset, sizeof value);
    return value;
}

// Queue retirement can free a node. An unreadable or changing snapshot always forwards.
bool copy(void* destination, const void* source, std::size_t size) noexcept {
    __try {
        std::memcpy(destination, source, size);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct Stamp {
    std::uintptr_t head{}, tail{};
    std::uint32_t produced{}, retired{}, serial{};
    std::int64_t created{};
    std::uint16_t sequence{}, bits{}, size{};
    std::uint8_t id{}, tag{}, flags{};
    bool pending{};
};

bool read_queue(void* queue, Stamp& stamp) noexcept {
    std::array<std::byte, 0xA0> first{}, second{};
    if (!copy(first.data(), queue, first.size())
        || field<std::uintptr_t>(first.data(), 0) != g_queueVtable || first[8] == std::byte{}
        || first[9] != std::byte{}) {
        return false;
    }
    stamp.head = field<std::uintptr_t>(first.data(), 0x50);
    stamp.tail = field<std::uintptr_t>(first.data(), 0x58);
    stamp.produced = field<std::uint32_t>(first.data(), 0x38);
    stamp.retired = field<std::uint32_t>(first.data(), 0x3C);
    stamp.serial = field<std::uint32_t>(first.data(), 0x9C);
    stamp.pending = stamp.head && stamp.tail && stamp.produced != stamp.retired;
    if (stamp.pending) {
        std::array<std::byte, 0x2B> node{}, again{};
        // NOLINTNEXTLINE(performance-no-int-to-ptr)
        const auto* tail = reinterpret_cast<const void*>(stamp.tail);
        if (!copy(node.data(), tail, node.size()) || !copy(again.data(), tail, again.size())
            || node != again) {
            return false;
        }
        stamp.created = field<std::int64_t>(node.data(), 0);
        stamp.sequence = field<std::uint16_t>(node.data(), 8);
        stamp.size = field<std::uint16_t>(node.data(), 0xA);
        stamp.bits = field<std::uint16_t>(node.data(), 0xC);
        stamp.id = field<std::uint8_t>(node.data(), 0x10);
        stamp.flags = field<std::uint8_t>(node.data(), 0x11);
        stamp.tag = field<std::uint8_t>(node.data(), 0x29);
    }
    if (!copy(second.data(), queue, second.size())) {
        return false;
    }
    // A link is published before its header. Do not mistake an in-progress append for success.
    return std::memcmp(first.data(), second.data(), first.size()) == 0;
}

bool completed_response(const Stamp& stamp, char tag) noexcept {
    return stamp.pending && stamp.id == kRequestId && stamp.size == kRequestSize && stamp.bits != 0
           && (stamp.flags & 0xC) == 0xC && stamp.tag == static_cast<std::uint8_t>(tag);
}

struct Entry {
    void* queue{};
    std::uint64_t epoch{}, ticket{};
    Stamp stamp{};
    Request request{};
    bool valid{};
};
// Bounded and allocation-free. Eviction only loses an optimization.
std::array<Entry, 128> g_cache{};
std::size_t g_victim{};
std::uint64_t g_ticket{};
struct Reservation {
    std::size_t index{};
    std::uint64_t ticket{}, epoch{};
    bool valid{};
};

bool repeat(void* queue,
            const Request& request,
            const Stamp& now,
            char tag,
            Reservation& reservation) noexcept {
    if (!TryAcquireSRWLockExclusive(&g_cacheLock)) {
        g_epoch.fetch_add(1, std::memory_order_acq_rel);
        return false;
    }
    std::size_t index = g_cache.size();
    for (std::size_t i = 0; i < g_cache.size(); ++i) {
        if (g_cache[i].queue == queue) {
            index = i;
            break;
        }
    }
    if (index == g_cache.size()) {
        index = g_victim++ % g_cache.size();
    }
    auto& entry = g_cache[index];
    const auto epoch = g_epoch.load(std::memory_order_acquire);
    const bool same = g_resets.load(std::memory_order_acquire) == 0 && entry.valid
                      && entry.queue == queue && entry.epoch == epoch
                      && completed_response(now, tag) && entry.request == request
                      && entry.stamp.serial == now.serial && entry.stamp.tail == now.tail
                      && entry.stamp.sequence == now.sequence && entry.stamp.created == now.created
                      && entry.stamp.bits == now.bits && entry.stamp.tag == now.tag;
    if (!same) {
        entry.queue = queue;
        entry.valid = false;
        entry.ticket = ++g_ticket;
        reservation = {index, entry.ticket, epoch, true};
    }
    ReleaseSRWLockExclusive(&g_cacheLock);
    return same;
}

void remember(const Reservation& reservation,
              const Request& request,
              const Stamp& before,
              const Stamp& after,
              char tag) noexcept {
    if (!reservation.valid || after.serial != before.serial + 1U || after.tail == before.tail
        || !completed_response(after, tag)
        || after.sequence != static_cast<std::uint16_t>(before.serial)
        || !TryAcquireSRWLockExclusive(&g_cacheLock)) {
        return;
    }
    auto& entry = g_cache[reservation.index];
    if (entry.ticket == reservation.ticket && g_resets.load(std::memory_order_acquire) == 0
        && g_epoch.load(std::memory_order_acquire) == reservation.epoch) {
        entry.epoch = reservation.epoch;
        entry.stamp = after;
        entry.request = request;
        entry.valid = true;
    }
    ReleaseSRWLockExclusive(&g_cacheLock);
}

template <typename T> T original(std::atomic<T>& source) noexcept {
    auto value = source.load(std::memory_order_acquire);
    while (value == nullptr) {
        source.wait(nullptr, std::memory_order_acquire);
        value = source.load(std::memory_order_acquire);
    }
    return value;
}

__declspec(noinline) void __fastcall
enqueue(void* queue, int id, int size, const void* data, int* bits, char tag) noexcept {
    g_calls.fetch_add(1, std::memory_order_acq_rel);
    const auto native = original(g_enqueue);
    Request request;
    Stamp before{}, after{};
    Reservation reservation{};
    const bool eligible = id == kRequestId && size == kRequestSize
                          && copy(request.data(), data, request.size()) && request[8] == std::byte{}
                          && field<std::uint64_t>(request.data(), 0x10) == 0x40
                          && read_queue(queue, before);
    if (eligible && repeat(queue, request, before, tag, reservation)) {
        // No additional wire bits: the earlier native reliable copy still owns delivery.
        if (bits) {
            *bits = 0;
        }
    } else {
        native(queue, id, size, data, bits, tag);
        if (eligible && read_queue(queue, after)) {
            remember(reservation, request, before, after, tag);
        }
    }
    g_calls.fetch_sub(1, std::memory_order_acq_rel);
}

__declspec(noinline) void __fastcall reset(void* queue) noexcept {
    g_calls.fetch_add(1, std::memory_order_acq_rel);
    const auto native = original(g_reset);
    // Native reset zeros the queue serial and frees nodes. Invalidate before and after
    // it runs so a reused queue/node address cannot inherit an older admission record.
    g_resets.fetch_add(1, std::memory_order_acq_rel);
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
    native(queue);
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
    g_resets.fetch_sub(1, std::memory_order_acq_rel);
    g_calls.fetch_sub(1, std::memory_order_acq_rel);
}

bool idle() noexcept {
    return g_calls.load(std::memory_order_acquire) == 0;
}
} // namespace

bool install() noexcept {
    if (g_hooks[0].attached && g_hooks[1].attached) {
        return true;
    }
    using namespace patterns;
    constexpr std::string_view enqueueText =
        "40 53 55 56 57 41 54 41 55 41 56 41 57 B8 38 01 02 00 E8 ? ? ? ? 48 2B E0";
    constexpr std::string_view resetText =
        "48 89 5C 24 20 57 48 83 EC 20 48 89 6C 24 30 48 8B D9 33 ED 40 38 69 08 0F 84 ? ? ? ?";
    constexpr auto enqueuePattern = signature<signature_length(enqueueText)>(enqueueText);
    constexpr auto resetPattern = signature<signature_length(resetText)>(resetText);
    auto* enqueueTarget = scan_main_image_unique(enqueuePattern, "reliable_request_enqueue");
    auto* resetTarget = scan_main_image_unique(resetPattern, "reliable_queue_reset");
    // The queue and node layouts read above are only known for the supported executable, so
    // both scans must land on its verified addresses and the queue vtable is fixed with them.
    const auto base = reinterpret_cast<std::uintptr_t>(GetModuleHandleW(nullptr));
    if (reinterpret_cast<std::uintptr_t>(enqueueTarget) != base + 0x16E3A20U
        || reinterpret_cast<std::uintptr_t>(resetTarget) != base + 0x16E2EC0U) {
        return false;
    }
    g_queueVtable = base + 0x1C9E908U;
    const std::array specs{hooking::detour::Spec{enqueueTarget, reinterpret_cast<void*>(&enqueue)},
                           hooking::detour::Spec{resetTarget, reinterpret_cast<void*>(&reset)}};
    if (!hooking::detour::install(specs, g_hooks)) {
        return false;
    }
    g_enqueue.store(reinterpret_cast<Enqueue>(g_hooks[0].original), std::memory_order_release);
    g_reset.store(reinterpret_cast<Reset>(g_hooks[1].original), std::memory_order_release);
    g_enqueue.notify_all();
    g_reset.notify_all();
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=reliable_requests stage=install result=ok policy=pending_repeat");
    return true;
}

bool uninstall() noexcept {
    if (!g_hooks[0].attached && !g_hooks[1].attached) {
        return true;
    }
    const std::array entries{hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&enqueue)},
                             hooking::detour::ProtectedCodeEntry{reinterpret_cast<void*>(&reset)}};
    if (hooking::detour::uninstall(g_hooks, entries, &idle)
        != hooking::detour::UninstallResult::removed) {
        return false;
    }
    g_enqueue.store(nullptr, std::memory_order_release);
    g_reset.store(nullptr, std::memory_order_release);
    g_epoch.fetch_add(1, std::memory_order_acq_rel);
    return true;
}
} // namespace sunrise::client::hooks::reliable_requests
