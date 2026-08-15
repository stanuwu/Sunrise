#include "internal.h"

namespace sunrise::client::content::items::packages {

/** Reads the progression definition table and the object array each definition routes to. */
bool build_progressions(const reader::Source& source,
                        reader::Scratch& scratch,
                        std::span<const std::byte> root,
                        std::vector<std::byte>& blob,
                        std::span<state::build_data::progressions::Definition> output,
                        std::size_t& count) noexcept {
    namespace domain = state::build_data::progressions;
    count = 0;
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
        const std::size_t at = rows.dataOffset
                               + static_cast<std::size_t>(row) * tables::kProgressionRowStride
                               + tables::kProgressionScopeOffset;
        if (at >= table.size()) {
            count = 0;
            return false;
        }
        const auto scope = std::to_integer<std::uint8_t>(table[at]);
        // Any scope beyond the two replicated objects belongs to an object this server does not
        // push, so it claims no slot in either array.
        output[count] = {static_cast<std::uint16_t>(row),
                         scope <= static_cast<std::uint8_t>(domain::Scope::character)
                             ? static_cast<domain::Scope>(scope)
                             : domain::Scope::unreplicated};
        ++count;
    }
    return count != 0;
}

} // namespace sunrise::client::content::items::packages
