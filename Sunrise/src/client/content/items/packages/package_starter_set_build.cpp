#include <array>
#include <cstddef>
#include <cstring>

#include "../../../../core/logging/log.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808074.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808075.h"
#include "../../../../middleware/content/packages/schema/generated/tag_classes_808087.h"
#include "internal.h"

namespace sunrise::client::content::items::packages {
namespace {

namespace domain = state::build_data::starter_sets;

using StarterSetRow = generated::StarterSetRow;
using ItemIndexRow = generated::ItemIndexU16Row;
/** One class row opens with its class byte. */
constexpr std::size_t kCharacterClassByte = offsetof(StarterSetRow, characterClass);

/**
 * Appends one item-index array of a class row to that row's definition.
 * @param blob Whole table blob.
 * @param descriptorOffset Offset of the array's count and self-relative offset.
 * @param output Row whose item list grows.
 * @return False when the array does not read or overflows the row's storage.
 */
[[nodiscard]] bool append_items(std::span<const std::byte> blob,
                                std::size_t descriptorOffset,
                                domain::Definition& output) noexcept {
    tables::Array items{};
    if (!tables::find_optional_array_at(blob, descriptorOffset, items)) {
        return false;
    }
    if (items.count == 0) {
        return true;
    }
    const std::size_t size = static_cast<std::size_t>(items.count) * sizeof(ItemIndexRow);
    if (items.elementClass != ItemIndexRow::kClass
        || items.count > domain::kItemCapacity - output.itemCount || items.dataOffset > blob.size()
        || size > blob.size() - items.dataOffset) {
        return false;
    }
    for (std::size_t index = 0; index < items.count; ++index) {
        std::uint16_t itemIndex = 0;
        std::memcpy(&itemIndex,
                    blob.data() + items.dataOffset + index * sizeof(ItemIndexRow)
                        + offsetof(ItemIndexRow, itemIndex),
                    sizeof itemIndex);
        output.itemIndices[output.itemCount++] = itemIndex;
    }
    return true;
}

/**
 * Reads every class row of the new-character table.
 * @param blob Whole table blob.
 * @param output Receives one definition per row.
 * @param count Receives the row count.
 * @return False when the rows do not match the installed layout.
 */
[[nodiscard]] bool read_rows(std::span<const std::byte> blob,
                             std::array<domain::Definition, domain::kClassCount>& output,
                             std::size_t& count) noexcept {
    count = 0;
    tables::Array rows{};
    if (!tables::find_array(blob, StarterSetRow::kClass, rows) || rows.count > output.size()
        || rows.dataOffset > blob.size()
        || static_cast<std::size_t>(rows.count) * sizeof(StarterSetRow)
               > blob.size() - rows.dataOffset) {
        return false;
    }
    for (std::size_t row = 0; row < rows.count; ++row) {
        const std::size_t base = rows.dataOffset + row * sizeof(StarterSetRow);
        domain::Definition& definition = output[row];
        definition = {};
        definition.characterClass = std::to_integer<std::uint8_t>(blob[base + kCharacterClassByte]);
        if (!append_items(blob, base + offsetof(StarterSetRow, defaultGear), definition)
            || !append_items(blob, base + offsetof(StarterSetRow, h6d26fb8d), definition)) {
            return false;
        }
    }
    count = static_cast<std::size_t>(rows.count);
    return true;
}

} // namespace

/**
 * Reads and publishes the new-character table: per class, the items a new character gets.
 * A table that reads but does not match the layout publishes empty, so the pass settles.
 * @param source Package source.
 * @param scratch Reader scratch.
 * @param root Investment root bytes.
 * @param blob Scratch storage for the table blob.
 * @return False when the tag does not read or the publish fails, so a later slice retries it.
 */
bool build_starter_sets(const reader::Source& source,
                        reader::Scratch& scratch,
                        std::span<const std::byte> root,
                        std::vector<std::byte>& blob) noexcept {
    std::uint32_t tag = 0;
    std::uint32_t tableClass = 0;
    if (!tables::slot_tag(root, tables::kStarterSetTableSlot, tag) || tag == 0
        || !reader::read_tag(source, scratch, tag, blob, tableClass)) {
        return false;
    }
    std::array<domain::Definition, domain::kClassCount> rows{};
    std::size_t count = 0;
    if (tableClass != generated::StarterSetTable::kClass
        || !read_rows(std::span<const std::byte>{blob}, rows, count)) {
        count = 0;
        core::log::writef(core::log::Channel::client,
                          core::log::Level::warn,
                          "ev=starter_sets stage=read result=fail tag=0x%08X class=0x%08X",
                          tag,
                          tableClass);
    }
    return state::build_data::publish_starter_sets(std::span(rows).first(count));
}

} // namespace sunrise::client::content::items::packages
