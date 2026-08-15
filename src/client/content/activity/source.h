#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/scenario_walk.h"

namespace sunrise::client::content::activity {

namespace packages = sunrise::middleware::content::packages;

/** One buffer per read slot. */
inline constexpr std::size_t kSlotCount =
    static_cast<std::size_t>(packages::tables::ReadSlot::count);

/** Lock-owned storage for one scenario walk. */
struct ScenarioSource {
    const packages::reader::Source* source{};
    packages::reader::Scratch scratch{};
    std::array<std::vector<std::byte>, kSlotCount> slots{};
};

} // namespace sunrise::client::content::activity
