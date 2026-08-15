#include "spawn_gate_record_dump.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../../../../state/account/account_state.h"
#include "../../../../state/runtime/runtime.h"

namespace sunrise::client::hooks::bootflow::spawn {
namespace {

/**
 * Head of the participation record to dump once, in bytes.
 * The record heads a 760-byte type-13 snapshot, so this stays well inside it. 96 bytes covers
 * every small field plus the player key at `+72`, the known value that locates the run.
 */
constexpr std::size_t kDumpBytes = 96;
/** Bytes of the component head to dump, which covers the object identity and its registry key. */
constexpr std::size_t kHeadBytes = 32;
/** Two hex digits per byte for both windows, plus the fixed prefix. */
constexpr std::size_t kDumpCapacity = (kDumpBytes + kHeadBytes) * 3 + 96;

/**
 * Bytes swept each side of the record for the player key.
 * Record-relative, because the record's own offset is the question: local authority binds `+1256`,
 * the proxy arm `+496`. A key at `+72` means the body is the record, `-688` that it is not.
 */
constexpr std::int32_t kSweepBack = 768;
constexpr std::int32_t kSweepForward = 768;
/** Head window dumped ahead of the record, which covers whichever base is in play. */
constexpr std::size_t kRecordComponentOffset = 496;
/** Reported when the sweep finds no copy of the key. */
constexpr std::int32_t kKeyAbsent = 0x7FFFFFFF;

std::atomic_bool g_dumped{false};

/**
 * Finds the player key the roster publishes, near the record.
 * The key is the one value in the type-13 body with no other producer, so where it lands says
 * whether that body reached this object and which arm bound the record.
 * @return Record-relative offset of the key, or `kKeyAbsent` when it is not near it.
 */
[[nodiscard]] std::int32_t find_player_key(const std::uint8_t* record) noexcept {
    const state::AccountState account = state::account_snapshot();
    // A backward sweep can leave the allocation on the proxy arm, so a fault ends the sweep, not
    // the process.
    __try {
        for (std::int32_t offset = -kSweepBack; offset <= kSweepForward; ++offset) {
            std::uint64_t candidate = 0;
            for (std::size_t byte = 0; byte < sizeof candidate; ++byte) {
                candidate |= static_cast<std::uint64_t>(record[offset + static_cast<int>(byte)])
                             << (8U * byte);
            }
            // The roster publishes the character the join named, which is not always the selected
            // one, so any authored character means the body landed.
            for (std::size_t index = 0; index < account.characterCount; ++index) {
                if (candidate != 0 && candidate == account.characters[index].soid) {
                    return offset;
                }
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kKeyAbsent;
    }
    return kKeyAbsent;
}

} // namespace

/** Dumps the head of the participation record once per run, with the player key's own offset. */
void dump_record(const std::uint8_t* record) noexcept {
    if (g_dumped.exchange(true, std::memory_order_relaxed)) {
        return;
    }
    // The component head carries the object's own identity, including the registry key at its
    // `+12`. That key says whether this object is in the group message 5 publishes.
    const std::uint8_t* const component = record - kRecordComponentOffset;
    std::array<char, kDumpCapacity> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=bootflow stage=spawn_record key_at=%d head=",
                                find_player_key(record));
    for (std::size_t index = 0; index < kHeadBytes && written > 0; ++index) {
        const int step = std::snprintf(line.data() + written,
                                       line.size() - static_cast<std::size_t>(written),
                                       "%02X",
                                       component[index]);
        written = step > 0 ? written + step : 0;
    }
    if (written > 0) {
        const int step = std::snprintf(
            line.data() + written, line.size() - static_cast<std::size_t>(written), " bytes=");
        written = step > 0 ? written + step : 0;
    }
    for (std::size_t index = 0; index < kDumpBytes && written > 0; ++index) {
        const int step = std::snprintf(line.data() + written,
                                       line.size() - static_cast<std::size_t>(written),
                                       "%02X",
                                       record[index]);
        written = step > 0 ? written + step : 0;
    }
    if (written > 0) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace sunrise::client::hooks::bootflow::spawn
