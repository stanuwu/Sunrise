#include "scriptable_catalog_reference_scan.h"

#include <cstring>

#include "../../../middleware/bap/activity_message/scriptable_auth_body.h"
#include "../../../middleware/content/packages/reader/container_framing.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_808095.h"
#include "../../../middleware/content/packages/schema/generated/tag_classes_80809c.h"
#include "../../../state/activity_sdk/format.h"
#include "../../../state/build_data/scenarios/definition.h"

namespace sunrise::client::content::activity::scriptables::internal {
namespace {

namespace generated = middleware::content::packages::schema::generated;
using ClientRef = generated::ClientRef;
// An inline client reference: class word, a zero word, then the ClientRef body.
constexpr std::uint32_t kClientReferenceClass = ClientRef::kClass;
constexpr std::size_t kClientReferenceReserved =
    middleware::content::packages::reader::layout::kClassWordSize;
constexpr std::size_t kClientReferenceBodyOffset =
    middleware::content::packages::reader::layout::kInlineRecordHeaderSize;
constexpr std::size_t kClientReferenceSize = kClientReferenceBodyOffset + sizeof(ClientRef);
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
        || !read_value(blob, offset + offsetof(ClientRef, targetKey), row.targetKey)
        || !read_value(blob, offset + offsetof(ClientRef, targetSlotType), row.targetType)
        || !read_value(blob, offset + offsetof(ClientRef, targetSlotIndex), row.targetIndex)
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

/** Reads only a type-30 player monitor's measured type-60 volume. */
bool read_type30_volume_reference(
    std::span<const std::byte> blob,
    const middleware::content::packages::tables::SlotDescriptor& descriptor,
    RawReference& output) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    namespace tables = middleware::content::packages::tables;
    namespace format = state::activity_sdk::format;
    output = {};
    if (descriptor.slotType != format::kOccupancySlotType
        || descriptor.componentClass != generated::MonitorSlotHandleType30::kClass
        || descriptor.senseSchema != generated::C80809531::kClass
        || descriptor.authSchema != generated::C80809532::kClass
        || descriptor.slotIndex > (std::numeric_limits<std::int16_t>::max)()) {
        return false;
    }
    const std::size_t offset = static_cast<std::size_t>(descriptor.descriptorOffset)
                               + tables::kType30MeasuredReferenceOffset;
    RawReference row{};
    if (offset > (std::numeric_limits<std::uint32_t>::max)()
        || !read_value(blob, offset + offsetof(ClientRef, targetKey), row.targetKey)
        || !read_value(blob, offset + offsetof(ClientRef, targetSlotType), row.targetType)
        || !read_value(blob, offset + offsetof(ClientRef, targetSlotIndex), row.targetIndex)
        || row.targetKey == 0 || row.targetKey == format::kAbsentIndex
        || row.targetKey == kAbsentClientReferenceKey || row.targetType != auth::kType60SlotType
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
        const std::size_t body = offset + kClientReferenceBodyOffset;
        if (!read_value(blob, offset, classId) || classId != kClientReferenceClass
            || !read_value(blob, offset + kClientReferenceReserved, reserved) || reserved != 0
            || !read_value(blob, body + offsetof(ClientRef, targetKey), row.targetKey)
            || !read_value(blob, body + offsetof(ClientRef, targetSlotType), row.targetType)
            || !read_value(blob, body + offsetof(ClientRef, targetSlotIndex), row.targetIndex)
            || row.targetType == 0
            || row.targetType > state::build_data::scenarios::kMaximumSlotType) {
            continue;
        }
        row.configTag = configTag;
        row.offset = static_cast<std::uint32_t>(offset);
        output.push_back(row);
    }
}

} // namespace sunrise::client::content::activity::scriptables::internal
