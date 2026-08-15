#include <array>
#include <cstdint>

#include "../../protobuf/codec.h"
#include "encoder.h"

namespace sunrise::middleware::content::manifest {
namespace {

/** MsgB field 1 is the manifest handle the Client finds the definition by. */
constexpr std::uint32_t kHandleField = 1;
/** MsgB field 2 is a required zero the Client reads before the name wrapper. */
constexpr std::uint32_t kReservedField = 2;
/** MsgB field 3 wraps the definition name in one nested message. */
constexpr std::uint32_t kNameWrapperField = 3;
/** MsgB field 4 picks the entitlement kind. Installed content uses kind 2. */
constexpr std::uint32_t kKindField = 4;
/** MsgB field 5 is a required zero the Client reads after the kind. */
constexpr std::uint32_t kKindReservedField = 5;
/** MsgB field 7 is the last required zero of the definition. */
constexpr std::uint32_t kTrailingReservedField = 7;
/** The name wrapper holds one inner message in its field 4. */
constexpr std::uint32_t kNameInnerField = 4;
/** The inner message holds the definition name in its field 1. */
constexpr std::uint32_t kNameField = 1;
/** Kind 2 is the value every installed-content definition carries. */
constexpr std::uint64_t kInstalledContentKind = 2;
/** Reserved definition fields are present and zero. */
constexpr std::uint64_t kReservedValue = 0;
/** The name and its two wrapper messages fit inside one fixed scratch buffer. */
constexpr std::size_t kNameWrapperCapacity = state::entitlements::kNameCapacity + 8;

} // namespace

/** Encodes one ContentConfig entitlement definition into caller storage. */
bool encode_entitlement(const state::entitlements::Entitlement& entry,
                        std::uint32_t definitionHandle,
                        std::span<std::byte> output,
                        std::size_t& size) noexcept {
    size = 0;
    const auto name = std::as_bytes(std::span(entry.name.data(), entry.nameLength));
    std::array<std::byte, kNameWrapperCapacity> inner{};
    protobuf::Writer innerWriter(inner);
    if (!innerWriter.write_length_delimited(kNameField, name)) {
        return false;
    }
    std::array<std::byte, kNameWrapperCapacity> wrapper{};
    protobuf::Writer wrapperWriter(wrapper);
    if (!wrapperWriter.write_length_delimited(kNameInnerField,
                                              std::span(inner).first(innerWriter.size()))) {
        return false;
    }

    protobuf::Writer writer(output);
    // Field order matches the definition the Client decodes: handle, reserved, name, kind.
    if (!writer.write_varint(kHandleField, definitionHandle)
        || !writer.write_varint(kReservedField, kReservedValue)
        || !writer.write_length_delimited(kNameWrapperField,
                                          std::span(wrapper).first(wrapperWriter.size()))
        || !writer.write_varint(kKindField, kInstalledContentKind)
        || !writer.write_varint(kKindReservedField, kReservedValue)
        || !writer.write_varint(kTrailingReservedField, kReservedValue)) {
        return false;
    }
    size = writer.size();
    return true;
}

} // namespace sunrise::middleware::content::manifest
