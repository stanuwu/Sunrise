#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "../../../state/build_data/scenarios/definition.h"

namespace sunrise::server::ui::activity_override {

/** One row label and its null. The widest is a spawn row: a hash, a 48-byte name, 2 markers. */
inline constexpr std::size_t kLabelCapacity = 96;
/** Slice-set rows for one bubble, which is what its region index has room for. */
inline constexpr std::size_t kSliceCapacity = 8;
/** Spawn-set rows for one map stem. The widest installed stem declares 294. */
inline constexpr std::size_t kSpawnCapacity = 1'024;
/** Index of no row, which is what every list starts on. */
inline constexpr std::size_t kNoRow = static_cast<std::size_t>(-1);

/** One null-terminated row label. */
using Label = std::array<char, kLabelCapacity>;

/** Rows the four pickers draw, rebuilt from the catalogues rather than stored in State. */
struct Lists {
    std::array<Label, state::build_data::scenarios::kDefinitionCapacity> activities{};
    std::size_t activityCount{};
    /** Published destination count the activity rows were built from. */
    std::size_t activityRevision{};

    std::array<Label, state::build_data::scenarios::kBubbleCapacity> bubbles{};
    std::array<std::uint8_t, state::build_data::scenarios::kBubbleCapacity> bubbleOrdinals{};
    std::size_t bubbleCount{};

    /** Layout of the selected destination, kept so a bubble pick needs no second lookup. */
    state::build_data::scenarios::Definition selected{};
    std::array<Label, kSliceCapacity> slices{};
    std::array<std::uint16_t, kSliceCapacity> sliceValues{};
    std::size_t sliceCount{};

    std::array<Label, kSpawnCapacity> spawns{};
    std::array<std::uint32_t, kSpawnCapacity> spawnHashes{};
    std::size_t spawnCount{};
    /** Set when the destination has a map stem but its spawn sets could not be listed. */
    bool spawnUnavailable{};
    /** Set when the rows are narrowed to the selected bubble rather than the whole map. */
    bool spawnNarrowed{};
    /** Rows the map declares that the selected bubble does not offer. */
    std::size_t spawnHidden{};
    /** Rows whose package this destination does not load, which are shown and marked. */
    std::size_t spawnForeign{};
};

/** @return Process-lifetime picker rows, which no other module reads. */
[[nodiscard]] Lists& lists() noexcept;

/** Rebuilds the destination rows when the published layout count has changed. */
void refresh_activities(Lists& rows) noexcept;

/**
 * Rebuilds the bubble and spawn-set rows for one destination, and clears the slice-set rows.
 * @param name Destination package name, or empty to clear every dependent list.
 */
void refresh_destination(Lists& rows, std::string_view name) noexcept;

/**
 * Rebuilds the slice-set and spawn-set rows for one bubble of the selected destination.
 * A bubble owns a run of slice sets from its region index, so only that run is offered. The spawn
 * rows narrow to the sets whose bubble mask names this bubble, plus the unbound candidates.
 */
void refresh_bubble(Lists& rows, std::uint8_t bubble) noexcept;

} // namespace sunrise::server::ui::activity_override
