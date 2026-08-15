#include "activity_manager_selection_parser.h"

#include "../../../../protobuf/codec.h"
#include "internal.h"

namespace sunrise::middleware::bap::activity_host_manager::request::selection {
namespace {

using protobuf::Field;
using protobuf::Reader;
using protobuf::WireType;

/** Service-6 field one holds the optional encoded activity-manager request. */
constexpr std::uint32_t kSelectionFieldNumber = 1;
/** Service-6 field two holds the investment snapshot, which repeats the same descriptor. */
constexpr std::uint32_t kSnapshotFieldNumber = 2;

/** Selected field state, kept only for one parser call. */
struct SelectedField final {
    bool seen{};
    std::span<const std::byte> bytes{};
};

/**
 * Finds the first length-delimited copy of both carrier fields. Best effort: a field that will
 * not decode keeps whatever was already found, because dropping it would put an authored
 * destination in place of the client's. A later duplicate is ignored.
 *
 * @param input Complete declared service-6 protobuf bytes.
 * @param request Receives a borrowed field-one view when one was found.
 * @param snapshot Receives a borrowed field-two view when one was found.
 */
void select_fields(std::span<const std::byte> input,
                   SelectedField& request,
                   SelectedField& snapshot) noexcept {
    request = {};
    snapshot = {};
    Reader reader(input);
    while (reader.remaining() != 0) {
        Field field{};
        if (!reader.next(field)) {
            return;
        }
        if (field.wireType != WireType::lengthDelimited) {
            continue;
        }
        SelectedField* target = field.fieldNumber == kSelectionFieldNumber  ? &request
                                : field.fieldNumber == kSnapshotFieldNumber ? &snapshot
                                                                            : nullptr;
        if (target == nullptr || target->seen) {
            continue;
        }
        target->seen = true;
        target->bytes = field.bytes;
    }
}

} // namespace

/** Takes the safe scalars of both descriptor copies out of a service-6 protobuf. */
bool parse_selection(std::span<const std::byte> input,
                     ActivityManagerSelectionResult& result) noexcept {
    result = {};
    SelectedField request{};
    SelectedField snapshot{};
    select_fields(input, request, snapshot);

    ActivityManagerSelectionResult committed{};
    if (request.seen && !request.bytes.empty()) {
        committed.hasSelection = parse_field_one(request.bytes, committed.selection);
    }
    if (snapshot.seen && !snapshot.bytes.empty()) {
        committed.hasSecondary = parse_field_two(snapshot.bytes, committed.secondary);
    }
    result = committed;
    // A missing or empty carrier means no selection, which is fine. Only a carrier that is there
    // and failed to decode is worth reporting, and then only when no copy survived.
    return committed.hasSelection || committed.hasSecondary
           || !(request.seen && !request.bytes.empty());
}

} // namespace sunrise::middleware::bap::activity_host_manager::request::selection
