#include "../../../protobuf/codec.h"
#include "field_selection.h"

namespace sunrise::middleware::bap::matchmaking::request {
namespace {

using protobuf::Field;
using protobuf::Reader;
using protobuf::WireType;

/** Fields inside the root advertisement message. */
enum class AdvertisementField : std::uint32_t {
    /** Optional existing advertisement id. */
    existingId = 1,
    /** Advertisement record with the stable key and descriptor. */
    record = 2,
};

/** Fields inside the advertisement record. */
enum class RecordField : std::uint32_t {
    /** Descriptor wrapper. It carries the whole opaque join descriptor. */
    descriptor = 2,
    /** Stable advertisement variant key. */
    variantKey = 4,
};

/** The descriptor wrapper carries its opaque bytes in field one. */
constexpr std::uint32_t kDescriptorBytesField = 1;

/** First-occurrence advertisement picks. */
struct AdvertisementSelection final {
    bool sawExistingId{};
    bool hasExistingId{};
    std::uint64_t existingId{};
    bool sawRecord{};
    bool hasRecordMessage{};
    std::span<const std::byte> recordMessage{};
};

/** First-occurrence record picks. */
struct RecordSelection final {
    bool sawDescriptor{};
    bool hasDescriptorMessage{};
    std::span<const std::byte> descriptorMessage{};
    bool sawVariantKey{};
    bool hasVariantKey{};
    std::uint64_t variantKey{};
};

/** First-occurrence descriptor pick. */
struct DescriptorSelection final {
    bool sawBytes{};
    bool hasBytes{};
    std::span<const std::byte> bytes{};
};

/**
 * Picks fields and checks the whole advertisement message.
 * @param input Advertisement submessage.
 * @param selected Receives borrowed first-occurrence fields.
 * @return True when every field has a whole, supported wire encoding.
 */
[[nodiscard]] bool select_advertisement(std::span<const std::byte> input,
                                        AdvertisementSelection& selected) noexcept {
    Reader reader(input);
    while (reader.remaining() != 0) {
        Field field;
        if (!reader.next(field)) {
            return false;
        }
        // Mark a field before checking its wire type. A later duplicate then cannot replace
        // the first occurrence.
        if (field.fieldNumber == static_cast<std::uint32_t>(AdvertisementField::existingId)
            && !selected.sawExistingId) {
            selected.sawExistingId = true;
            if (field.wireType == WireType::varint) {
                selected.hasExistingId = true;
                selected.existingId = field.value;
            }
        } else if (field.fieldNumber == static_cast<std::uint32_t>(AdvertisementField::record)
                   && !selected.sawRecord) {
            selected.sawRecord = true;
            if (field.wireType == WireType::lengthDelimited) {
                selected.hasRecordMessage = true;
                selected.recordMessage = field.bytes;
            }
        }
    }
    return true;
}

/**
 * Picks fields and checks the whole advertisement record.
 * @param input Advertisement-record submessage.
 * @param selected Receives borrowed first-occurrence fields.
 * @return True when every field has a whole, supported wire encoding.
 */
[[nodiscard]] bool select_record(std::span<const std::byte> input,
                                 RecordSelection& selected) noexcept {
    Reader reader(input);
    while (reader.remaining() != 0) {
        Field field;
        if (!reader.next(field)) {
            return false;
        }
        // The first occurrence wins even when its wire type is not usable here.
        if (field.fieldNumber == static_cast<std::uint32_t>(RecordField::descriptor)
            && !selected.sawDescriptor) {
            selected.sawDescriptor = true;
            if (field.wireType == WireType::lengthDelimited) {
                selected.hasDescriptorMessage = true;
                selected.descriptorMessage = field.bytes;
            }
        } else if (field.fieldNumber == static_cast<std::uint32_t>(RecordField::variantKey)
                   && !selected.sawVariantKey) {
            selected.sawVariantKey = true;
            if (field.wireType == WireType::varint) {
                selected.hasVariantKey = true;
                selected.variantKey = field.value;
            }
        }
    }
    return true;
}

/**
 * Picks the bytes and checks the whole descriptor wrapper.
 * @param input Descriptor-wrapper submessage.
 * @param selected Receives the borrowed first bytes field.
 * @return True when every field is valid and any bytes found are the right size.
 */
[[nodiscard]] bool select_descriptor(std::span<const std::byte> input,
                                     DescriptorSelection& selected) noexcept {
    Reader reader(input);
    while (reader.remaining() != 0) {
        Field field;
        if (!reader.next(field)) {
            return false;
        }
        // A wrong wire type on the first occurrence makes the bytes absent. Duplicates are
        // still ignored.
        if (field.fieldNumber == kDescriptorBytesField && !selected.sawBytes) {
            selected.sawBytes = true;
            if (field.wireType == WireType::lengthDelimited) {
                selected.hasBytes = true;
                selected.bytes = field.bytes;
            }
        }
    }
    return !selected.hasBytes || selected.bytes.size() == kJoinDescriptorSize;
}

} // namespace

/** Parses the kind-2 path into borrowed request fields. */
bool parse_update(std::span<const std::byte> input, AdvertisementUpdate& update) noexcept {
    update = {};
    AdvertisementSelection advertisement;
    if (!select_advertisement(input, advertisement)) {
        return false;
    }
    update.hasExistingId = advertisement.hasExistingId;
    update.existingId = advertisement.existingId;
    if (!advertisement.hasRecordMessage) {
        return true;
    }

    // Only the submessages we pick are read further. Other byte fields stay opaque.
    RecordSelection record;
    if (!select_record(advertisement.recordMessage, record)) {
        return false;
    }
    if (record.hasVariantKey) {
        update.variantKey = record.variantKey;
    }
    if (!record.hasDescriptorMessage) {
        return true;
    }

    DescriptorSelection descriptor;
    if (!select_descriptor(record.descriptorMessage, descriptor)) {
        return false;
    }
    update.hasDescriptor = descriptor.hasBytes;
    update.descriptor = descriptor.bytes;
    return true;
}

} // namespace sunrise::middleware::bap::matchmaking::request
