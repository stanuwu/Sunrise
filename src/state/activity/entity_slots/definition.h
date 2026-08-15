#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::state::activity::entity_slots {

/** One activity session owns exactly 8,192 simulation entity-slot lease bits. */
inline constexpr std::size_t kSlotCount = 8'192;
/** 8 consecutive logical slots share one byte in the fixed lease mask. */
inline constexpr std::size_t kSlotsPerByte = 8;
/** The fixed per-session lease mask is exactly 1,024 bytes. */
inline constexpr std::size_t kMaskSize = kSlotCount / kSlotsPerByte;

/** Slots granted by the host to one client and not yet returned. */
using LeaseMask = std::array<std::byte, kMaskSize>;

/** Operation represented by one prepared entity-slot mutation. */
enum class MutationKind : std::uint8_t {
    none,
    join,
    grant,
    release,
};

} // namespace sunrise::state::activity::entity_slots
