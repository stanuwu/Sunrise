#include <cstring>

#include "internal.h"

namespace sunrise::client::content::items::packages {

/** Reads the progression definition table, its rank steps, and the object array each routes to. */
bool build_progressions(const reader::Source& source,
                        reader::Scratch& scratch,
                        std::span<const std::byte> root,
                        std::vector<std::byte>& blob,
                        std::span<state::build_data::progressions::Definition> output,
                        std::size_t& count,
                        std::span<state::build_data::progressions::Step> steps,
                        std::size_t& stepCount) noexcept {
    namespace domain = state::build_data::progressions;
    count = 0;
    stepCount = 0;
    std::uint32_t tableTag = 0;
    tables::Array rows{};
    if (!tables::slot_tag(root, tables::kProgressionTableSlot, tableTag) || tableTag == 0
        || !reader::read_tag(source, scratch, tableTag, blob)
        || !tables::find_array_at(
            std::span<const std::byte>{blob}, tables::kTableArrayDescriptor, rows)
        || rows.elementClass != tables::kProgressionTableClass || rows.count > output.size()) {
        return false;
    }
    const std::span<const std::byte> table{blob};
    for (std::uint64_t row = 0; row < rows.count; ++row) {
        const std::size_t at =
            rows.dataOffset + static_cast<std::size_t>(row) * tables::kProgressionRowStride;
        if (at + tables::kProgressionRowStride > table.size()) {
            count = 0;
            stepCount = 0;
            return false;
        }
        const auto scope =
            std::to_integer<std::uint8_t>(table[at + tables::kProgressionScopeOffset]);
        const auto repeatLastStep =
            std::to_integer<std::uint8_t>(table[at + tables::kProgressionRepeatLastStepOffset]);
        if (repeatLastStep > 1) {
            count = 0;
            stepCount = 0;
            return false;
        }
        domain::Definition& definition = output[count];
        // Any scope beyond the two replicated objects belongs to an object this server does not
        // push, so it claims no slot in either array.
        const auto resolvedScope = scope <= static_cast<std::uint8_t>(domain::Scope::character)
                                       ? static_cast<domain::Scope>(scope)
                                       : domain::Scope::unreplicated;
        definition = {static_cast<std::uint16_t>(row),
                      static_cast<std::uint16_t>(stepCount),
                      0,
                      resolvedScope,
                      repeatLastStep != 0};
        tables::Array ladder{};
        if (!tables::find_optional_array_at(table, at + tables::kProgressionStepField, ladder)) {
            count = 0;
            stepCount = 0;
            return false;
        }
        if (ladder.count != 0
            && (ladder.elementClass != tables::kProgressionStepRowClass
                || ladder.count > domain::kStepPerDefinitionCapacity
                || ladder.count > steps.size() - stepCount
                || ladder.dataOffset
                           + static_cast<std::size_t>(ladder.count) * tables::kProgressionStepStride
                       > table.size())) {
            count = 0;
            stepCount = 0;
            return false;
        }
        for (std::uint64_t step = 0; step < ladder.count; ++step) {
            std::memcpy(&steps[stepCount].cost,
                        table.data() + ladder.dataOffset
                            + static_cast<std::size_t>(step) * tables::kProgressionStepStride,
                        sizeof steps[stepCount].cost);
            ++stepCount;
        }
        definition.stepCount = static_cast<std::uint8_t>(ladder.count);
        ++count;
    }
    return count != 0;
}

} // namespace sunrise::client::content::items::packages
