#include "mod_load_patch.h"

#include <Windows.h>

#include <atomic>
#include <cstring>
#include <limits>
#include <unordered_map>
#include <utility>

#include "../../../core/logging/log.h"

namespace sunrise::state::mods::load_patch {
namespace {

// IEEE CRC-32, reflected, with all-ones start and end; Studio's guard uses the same.
constexpr std::uint32_t kCrcPolynomial = 0xEDB88320U, kCrcAllOnes = 0xFFFFFFFFU;

/** Every kept update of one tag, oldest first. */
struct Kept final {
    std::uint32_t classId{};
    std::uint32_t size{};
    std::vector<std::vector<Range>> updates;
};

SRWLOCK g_lock{SRWLOCK_INIT};
std::unordered_map<std::uint32_t, Kept> g_kept;
/** Lets every load skip the lock while nothing is kept. */
std::atomic<std::size_t> g_count{};

/** @return True when every range of the update lies inside the tag and matches its guard. */
[[nodiscard]] bool guards_hold(std::span<const Range> update,
                               std::span<const std::byte> blob) noexcept {
    for (const Range& range : update) {
        if (range.offset > blob.size() || range.bytes.size() > blob.size() - range.offset
            || crc32(blob.subspan(range.offset, range.bytes.size())) != range.oldCrc32) {
            return false;
        }
    }
    return true;
}

} // namespace

std::uint32_t crc32(std::span<const std::byte> bytes) noexcept {
    std::uint32_t crc = kCrcAllOnes;
    for (const std::byte value : bytes) {
        crc ^= std::to_integer<std::uint32_t>(value);
        for (int bit = 0; bit < std::numeric_limits<unsigned char>::digits; ++bit) {
            crc = (crc >> 1U) ^ (kCrcPolynomial & (0U - (crc & 1U)));
        }
    }
    return crc ^ kCrcAllOnes;
}

/** A tag kept with another class or size refuses the update; its earlier updates stay. */
bool retain(std::uint32_t tag,
            std::uint32_t classId,
            std::uint32_t size,
            std::vector<Range> ranges) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    bool kept = false;
    try {
        Kept& entry = g_kept[tag];
        if (entry.updates.empty()) {
            entry.classId = classId;
            entry.size = size;
        }
        if (entry.classId == classId && entry.size == size) {
            entry.updates.push_back(std::move(ranges));
            kept = true;
        }
        g_count.store(g_kept.size(), std::memory_order_release);
    } catch (...) {
        kept = false;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return kept;
}

/** The client's loader asks for every tag it loads, so an empty table answers without the lock. */
std::uint32_t kept_size(std::uint32_t tag) noexcept {
    if (g_count.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    AcquireSRWLockShared(&g_lock);
    const auto found = g_kept.find(tag);
    const std::uint32_t size = found == g_kept.end() ? 0 : found->second.size;
    ReleaseSRWLockShared(&g_lock);
    return size;
}

/** Runs on the client's loader threads, so it holds the lock only to read. */
std::size_t apply(std::uint32_t tag, std::uint32_t classId, std::span<std::byte> blob) noexcept {
    if (g_count.load(std::memory_order_acquire) == 0) {
        return 0;
    }
    AcquireSRWLockShared(&g_lock);
    std::size_t written = 0;
    std::size_t total = 0;
    const auto found = g_kept.find(tag);
    const bool known = found != g_kept.end();
    // A slot may own more bytes than its entry; the updates cover the entry's bytes only.
    if (known && found->second.classId == classId && found->second.size <= blob.size()) {
        total = found->second.updates.size();
        blob = blob.first(found->second.size);
        for (const std::vector<Range>& update : found->second.updates) {
            if (!guards_hold(update, blob)) {
                break;
            }
            for (const Range& range : update) {
                std::memcpy(blob.data() + range.offset, range.bytes.data(), range.bytes.size());
            }
            ++written;
        }
    }
    ReleaseSRWLockShared(&g_lock);
    if (known) {
        core::log::writef(core::log::Channel::state,
                          written == total ? core::log::Level::info : core::log::Level::warn,
                          "ev=load_patch tag=%08X written=%zu kept=%zu",
                          tag,
                          written,
                          total);
    }
    return written;
}

} // namespace sunrise::state::mods::load_patch
