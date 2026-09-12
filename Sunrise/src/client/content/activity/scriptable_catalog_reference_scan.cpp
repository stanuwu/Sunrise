#include "scriptable_catalog_reference_scan.h"

#include <cstring>

#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../state/activity_sdk/format.h"
#include "../../../state/build_data/scenarios/definition.h"

namespace sunrise::client::content::activity::scriptables::internal {
namespace {

// Client references: the package class id and the fixed row stride.
constexpr std::uint32_t kClientReferenceClass = 0x80809C42U;
constexpr std::size_t kClientReferenceSize = 16;
/** Native ClientRef uses the empty-name hash for an absent key. */
constexpr std::uint32_t kAbsentClientReferenceKey = 0x811C9DC5U;

/** Reads one bounded scalar without imposing alignment on package bytes. */
template <typename T>
[[nodiscard]] bool
read_value(std::span<const std::byte> blob, std::size_t offset, T& value) noexcept {
    value = {};
    if (offset > blob.size() || sizeof value > blob.size() - offset) {
        return false;
    }
    std::memcpy(&value, blob.data() + offset, sizeof value);
    return true;
}

} // namespace

/**
 * Reads the squad ClientRef owned by one exact Type 2 descriptor.
 * @param blob Package config containing the descriptor.
 * @param descriptor Validated slot descriptor.
 * @param output Receives the exact source slot and squad target; cleared on failure.
 * @return True when the descriptor and reference have the native shapes.
 */
bool read_type2_squad_reference(
    std::span<const std::byte> blob,
    const middleware::content::packages::tables::SlotDescriptor& descriptor,
    RawReference& output) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    namespace tables = middleware::content::packages::tables;
    namespace format = state::activity_sdk::format;
    output = {};
    if (descriptor.slotType != auth::kType2SlotType
        || descriptor.componentClass != auth::kType2ComponentClass
        || descriptor.senseSchema != auth::kType2SenseSchema
        || descriptor.authSchema != auth::kType2Schema
        || descriptor.slotIndex > (std::numeric_limits<std::int16_t>::max)()) {
        return false;
    }
    const std::size_t offset =
        static_cast<std::size_t>(descriptor.descriptorOffset) + tables::kType2SquadReferenceOffset;
    RawReference row{};
    if (offset > (std::numeric_limits<std::uint32_t>::max)()
        || !read_value(blob, offset, row.targetKey)
        || !read_value(blob, offset + sizeof(row.targetKey), row.targetType)
        || !read_value(
            blob, offset + sizeof(row.targetKey) + sizeof(row.targetType), row.targetIndex)
        || row.targetKey == 0 || row.targetKey == format::kAbsentIndex
        || row.targetKey == kAbsentClientReferenceKey || row.targetType != format::kSquadSlotType
        || row.targetIndex > (std::numeric_limits<std::int16_t>::max)()) {
        return false;
    }
    row.configTag = descriptor.configTag;
    row.offset = static_cast<std::uint32_t>(offset);
    row.sourceIndex = descriptor.slotIndex;
    output = row;
    return true;
}

/** Retains aligned ClientRef records from one reached config blob. */
void collect_typed_references(std::span<const std::byte> blob,
                              std::uint32_t configTag,
                              std::vector<RawReference>& output) {
    for (std::size_t offset = 0; offset + kClientReferenceSize <= blob.size(); offset += 4) {
        std::uint32_t classId = 0;
        std::uint32_t reserved = 0;
        RawReference row{};
        if (!read_value(blob, offset, classId) || classId != kClientReferenceClass
            || !read_value(blob, offset + 4, reserved) || reserved != 0
            || !read_value(blob, offset + 8, row.targetKey)
            || !read_value(blob, offset + 12, row.targetType)
            || !read_value(blob, offset + 14, row.targetIndex) || row.targetType == 0
            || row.targetType > state::build_data::scenarios::kMaximumSlotType) {
            continue;
        }
        row.configTag = configTag;
        row.offset = static_cast<std::uint32_t>(offset);
        output.push_back(row);
    }
}

} // namespace sunrise::client::content::activity::scriptables::internal
