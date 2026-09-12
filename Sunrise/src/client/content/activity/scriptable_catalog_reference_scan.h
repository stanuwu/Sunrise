#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "../../../middleware/content/packages/tables/slot_descriptor_reader.h"

namespace sunrise::client::content::activity::scriptables::internal {

/** Authored slot indices fit a signed 16-bit value. */
inline constexpr std::uint16_t kUnresolvedReferenceSlot =
    (std::numeric_limits<std::uint16_t>::max)();

/** One package ClientRef and its exact source slot when the descriptor owns it. */
struct RawReference final {
    std::uint32_t configTag{};
    std::uint32_t offset{};
    std::uint32_t targetKey{};
    std::uint16_t targetType{};
    std::uint16_t targetIndex{};
    std::uint16_t sourceIndex{kUnresolvedReferenceSlot};
};

/** Reads only the authored Type 2 descriptor's squad reference. */
[[nodiscard]] bool
read_type2_squad_reference(std::span<const std::byte> blob,
                           const middleware::content::packages::tables::SlotDescriptor& descriptor,
                           RawReference& output) noexcept;

/** Retains aligned ClientRef records from one reached config blob. */
void collect_typed_references(std::span<const std::byte> blob,
                              std::uint32_t configTag,
                              std::vector<RawReference>& output);

} // namespace sunrise::client::content::activity::scriptables::internal
