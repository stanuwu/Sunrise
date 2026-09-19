#include "../../encoding/bit_writer.h"
#include "auth_schema_catalog.h"
#include "scoreboard_record.h"

namespace sunrise::middleware::bap::activity_message::scoreboard_record {
namespace {

namespace bits = encoding::bits;
constexpr std::uint8_t kPresenceWidth = 1;
static_assert([] {
    for (const auto& row : auth_schema_catalog::kTypes) {
        if (row.slotType == kSlotType) {
            return row.schema == kRecordClassId && row.minimumBits == kRootFieldCount;
        }
    }
    return false;
}());

[[nodiscard]] bool write_element(bits::Writer& writer, const Element& element) noexcept {
    bool encoded = writer.write(element.hasKey ? 1U : 0U, kPresenceWidth);
    if (encoded && element.hasKey) {

        encoded = writer.write(element.key, kElementKeyWidth);
    }
    for (std::size_t field = 0; encoded && field < kElementMiddleFieldCount; ++field) {
        encoded = writer.write(0, kPresenceWidth);
    }
    return encoded && writer.write(element.live ? 1U : 0U, 1);
}

} // namespace

bool write_record_body(bits::Writer& writer, const Record& record) noexcept {
    if (!valid(record)) {
        return false;
    }
    bool encoded =
        writer.write(1, kPresenceWidth) && writer.write(record.elementCount, kElementCountWidth);
    for (std::size_t index = 0; encoded && index < record.elementCount; ++index) {
        encoded = write_element(writer, record.elements[index]);
    }
    encoded = encoded && writer.write(record.hasTeamList ? 1U : 0U, kPresenceWidth);
    if (encoded && record.hasTeamList) {
        encoded = writer.write(record.teamCount, kTeamCountWidth);
        for (std::size_t team = 0; encoded && team < record.teamCount; ++team) {
            encoded = writer.write(0, kNeutralTeamEntryBits);
        }
    }
    for (std::size_t field = kTeamListFieldIndex + 1; encoded && field < kRootFieldCount; ++field) {
        encoded = writer.write(0, kPresenceWidth);
    }
    return encoded;
}

bool valid(const Record& record) noexcept {
    if (record.elementCount > kElementCount) {
        return false;
    }
    if (record.hasTeamList && record.teamCount > kTeamCount) {
        return false;
    }
    for (std::size_t index = 0; index < kElementCount; ++index) {
        const Element& element = record.elements[index];
        // A live seat with no key resolves to nothing and a keyed seat that is not live is skipped
        // by the block's own walker, so neither is worth putting on the wire.
        if (element.live != element.hasKey) {
            return false;
        }
        if (element.hasKey && element.key == 0) {
            return false;
        }
        // Only the first `elementCount` seats are encoded. A seat past the count would be dropped
        // in silence, and the record would claim a roster it never published.
        if (index >= record.elementCount && (element.live || element.hasKey)) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::middleware::bap::activity_message::scoreboard_record
