#include "../../../protobuf/codec.h"
#include "field_selection.h"

namespace sunrise::middleware::bap::matchmaking::request {
namespace {

using protobuf::Field;
using protobuf::Reader;
using protobuf::WireType;

/** Service-42 root protobuf fields the responder reads. */
enum class RootField : std::uint32_t {
    /** Varint selector. It picks one of 8 request shapes. */
    kind = 2,
    /** Length-delimited advertisement request, used only by kind 2. */
    advertisement = 8,
};

/** Raw first-occurrence root selections. */
struct RawRoot final {
    bool sawKind{};
    bool hasKindValue{};
    std::uint64_t kindValue{};
    bool sawAdvertisement{};
    bool hasAdvertisementMessage{};
    std::span<const std::byte> advertisementMessage{};
};

/**
 * Converts one raw selector. Values outside the supported set are rejected.
 * @param value Unsigned service-42 field-two selector.
 * @return Request kind for a supported value, else none.
 */
[[nodiscard]] RequestKind request_kind(std::uint64_t value) noexcept {
    switch (value) {
    case static_cast<std::uint64_t>(RequestKind::sessionSearch):
        return RequestKind::sessionSearch;
    case static_cast<std::uint64_t>(RequestKind::advertisementUpdate):
        return RequestKind::advertisementUpdate;
    case static_cast<std::uint64_t>(RequestKind::advertisementDelete):
        return RequestKind::advertisementDelete;
    case static_cast<std::uint64_t>(RequestKind::configuration):
        return RequestKind::configuration;
    case static_cast<std::uint64_t>(RequestKind::rejoinAdvertisementUpdate):
        return RequestKind::rejoinAdvertisementUpdate;
    case static_cast<std::uint64_t>(RequestKind::rejoinAdvertisementDelete):
        return RequestKind::rejoinAdvertisementDelete;
    case static_cast<std::uint64_t>(RequestKind::locateSession):
        return RequestKind::locateSession;
    case static_cast<std::uint64_t>(RequestKind::liveStats):
        return RequestKind::liveStats;
    default:
        return RequestKind::none;
    }
}

/**
 * Picks the first root occurrences and checks the message.
 * @param input Whole service-42 protobuf body.
 * @param selected Receives raw borrowed picks.
 * @return True when every root field has a whole, supported wire encoding.
 */
[[nodiscard]] bool select_root(std::span<const std::byte> input, RawRoot& selected) noexcept {
    Reader reader(input);
    while (reader.remaining() != 0) {
        Field field;
        if (!reader.next(field)) {
            return false;
        }
        // Mark a field before checking its wire type. A later duplicate then cannot replace
        // the first occurrence.
        if (field.fieldNumber == static_cast<std::uint32_t>(RootField::kind) && !selected.sawKind) {
            selected.sawKind = true;
            if (field.wireType == WireType::varint) {
                selected.hasKindValue = true;
                selected.kindValue = field.value;
            }
        } else if (field.fieldNumber == static_cast<std::uint32_t>(RootField::advertisement)
                   && !selected.sawAdvertisement) {
            selected.sawAdvertisement = true;
            if (field.wireType == WireType::lengthDelimited) {
                selected.hasAdvertisementMessage = true;
                selected.advertisementMessage = field.bytes;
            }
        }
    }
    return true;
}

} // namespace

/** Picks the fields we need and checks the whole service-42 root. */
bool parse_root(std::span<const std::byte> input, Root& selected) noexcept {
    selected = {};
    RawRoot raw;
    if (!select_root(input, raw)) {
        return false;
    }
    if (raw.hasKindValue) {
        selected.kind = request_kind(raw.kindValue);
    }
    selected.hasAdvertisementMessage = raw.hasAdvertisementMessage;
    selected.advertisementMessage = raw.advertisementMessage;
    return true;
}

} // namespace sunrise::middleware::bap::matchmaking::request
