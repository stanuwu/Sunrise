#include "public_event_schedule_evaluate.h"

#include <cmath>
#include <limits>

namespace sunrise::server::activity::mission::public_event_schedule {
namespace {
using state::build_data::public_event_schedule::kAbsentHash;

/** Seconds in the client's 365-day rotation grid. */
constexpr auto kYearSeconds = state::build_data::public_event_schedule::kRotationSeconds;
/** The byte hash's seeded first step and its prime. */
constexpr std::uint32_t kHashSeed = 0x050C5D1FU, kHashPrime = 0x01000193U;
/** The congruential draw the client takes from a hash. */
constexpr std::uint32_t kDrawMultiplier = 0x19660DU, kDrawIncrement = 0x3C6EF35FU;
/** The pick seed multiplies the salted slot by this. */
constexpr std::uint64_t kPickSaltMultiplier = 0xEE159U;
/** The client's float32 scale of a 16-bit draw. */
constexpr float kPickScale = 1.5259021893143654e-05f;
/** The native draw keeps its low word and then takes the high word of the product. */
constexpr std::uint32_t kMask16 = 0xFFFFU;
constexpr int kDrawShift = 16;

/** Upper 16 bits of the congruential draw. */
[[nodiscard]] std::uint32_t draw16(std::uint32_t hash) noexcept {
    return (hash * kDrawMultiplier + kDrawIncrement) >> kDrawShift;
}

/** Sign-extends the low 16 bits, as the client's movsx does. */
[[nodiscard]] std::int64_t signed16(std::uint64_t value) noexcept {
    return static_cast<std::int16_t>(value & kMask16);
}

/** Start jitter of one slot in seconds, 0..span, derived from the slot start alone. */
[[nodiscard]] std::int64_t jitter(std::uint64_t slotStart, std::int64_t span) noexcept {
    const std::uint32_t product =
        draw16(hash8(slotStart)) * static_cast<std::uint32_t>(signed16(span) + 1);
    return signed16(product >> kDrawShift);
}

/** One slot's start and window. */
[[nodiscard]] Occurrence
occurrence(const Row& row, std::uint64_t slot, std::uint64_t cycle) noexcept {
    Occurrence out{};
    out.slotStart =
        row.offsetSeconds
        + static_cast<std::int64_t>(slot * static_cast<std::uint64_t>(row.periodSeconds));
    out.start = out.slotStart + jitter(static_cast<std::uint64_t>(out.slotStart), row.spanSeconds);
    out.finish = out.start + row.windowSeconds;
    out.slot = slot % cycle;
    return out;
}
} // namespace

std::uint32_t hash8(std::uint64_t value) noexcept {
    std::uint32_t hash = (static_cast<std::uint32_t>(value & 0xFFU) ^ kHashSeed) * kHashPrime;
    for (int shift = 8; shift <= 48; shift += 8) {
        hash = (hash ^ static_cast<std::uint32_t>((value >> shift) & 0xFFU)) * kHashPrime;
    }
    return hash ^ static_cast<std::uint32_t>(value >> 56);
}

bool evaluate(const Row& row, std::int64_t now, Occurrence& out) noexcept {
    // One extra period, jitter and window must fit after the requested Unix second.
    if (row.periodSeconds <= 0 || row.periodSeconds > kYearSeconds || row.entryCount == 0
        || row.offsetSeconds < 0 || row.offsetSeconds > kYearSeconds || row.spanSeconds < 0
        || row.spanSeconds > kYearSeconds || row.windowSeconds < 0
        || row.windowSeconds > kYearSeconds || now < 0
        || now > (std::numeric_limits<std::int64_t>::max)() - 3 * kYearSeconds) {
        return false;
    }
    const auto period = static_cast<std::uint64_t>(row.periodSeconds);
    const std::uint64_t cycle = kYearSeconds / period;
    const std::uint64_t slot =
        now > row.offsetSeconds ? static_cast<std::uint64_t>(now - row.offsetSeconds) / period : 0;
    out = occurrence(row, slot, cycle);
    if (out.start > now) {
        out.untilStart = out.start - now;
        return true;
    }
    if (out.finish > now) {
        out.active = true;
        return true;
    }
    out = occurrence(row, slot + 1, cycle);
    out.untilStart = out.start - now;
    return true;
}

std::size_t pick(const Table& table, const Row& row, std::uint64_t slot) noexcept {
    const auto entries = table.entries_of(row);
    if (entries.empty()) {
        return kNoEntry;
    }
    const std::uint32_t seed = hash8((row.salt + slot) * kPickSaltMultiplier);
    float total = 0.0f;
    for (const auto& entry : entries) {
        // A gated entry weighs nothing here: the client tests an unlock expression no shipped row
        // uses.
        total += entry.gate == kAbsentHash ? entry.weight : 0.0f;
    }
    if (!std::isfinite(total)) {
        return kNoEntry;
    }
    const float threshold = static_cast<float>(draw16(seed)) * kPickScale * total;
    if (threshold <= 0.0f) {
        return kNoEntry;
    }
    std::size_t last = entries.size() - 1;
    while (last > 0 && (entries[last].gate != kAbsentHash || entries[last].weight == 0.0f)) {
        --last;
    }
    float sum = 0.0f;
    for (std::size_t index = 0; index < last; ++index) {
        const float weight = entries[index].gate == kAbsentHash ? entries[index].weight : 0.0f;
        if (weight != 0.0f) {
            sum += weight;
            if (sum >= threshold) {
                return row.firstEntry + index;
            }
        }
    }
    return row.firstEntry + last;
}

bool for_site(const Table& table, std::uint32_t site, std::int64_t now, SiteResult& out) noexcept {
    for (std::size_t index = 0; index < table.rowCount; ++index) {
        const Row& row = table.rows[index];
        bool listed = false;
        for (const auto& entry : table.entries_of(row)) {
            listed = listed || entry.site == site;
        }
        if (!listed) {
            continue;
        }
        if (!evaluate(row, now, out.occurrence)) {
            return false;
        }
        out.row = index;
        out.picked = pick(table, row, out.occurrence.slot);
        out.selected = out.picked != kNoEntry && table.entries[out.picked].site == site;
        return true;
    }
    return false;
}
} // namespace sunrise::server::activity::mission::public_event_schedule
