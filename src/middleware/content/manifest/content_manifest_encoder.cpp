#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../../protobuf/codec.h"
#include "encoder.h"

namespace sunrise::middleware::content::manifest {
namespace {

/** ContentConfig repeats entitlement definition messages in top-level field 2. */
constexpr std::uint32_t kEntitlementsField = 2;
/** ContentConfig repeats package descriptor messages in top-level field 3. */
constexpr std::uint32_t kPackageRowsField = 3;
/** ContentConfig carries the fetch id as top-level field 5. */
constexpr std::uint32_t kGuidField = 5;
/** MsgC field 1 is the offer key package registration uses. */
constexpr std::uint32_t kOfferKeyField = 1;
/** MsgC field 2 is the package filename stem. It is opened with a `.pkg` suffix. */
constexpr std::uint32_t kPackageNameField = 2;
/** MsgC field 3 is the public package id, read as a 16-bit value. */
constexpr std::uint32_t kPackageIdField = 3;
/** MsgC field 4 is the exact public build signature from header offset +0x08. */
constexpr std::uint32_t kBuildSignatureField = 4;
/** One MsgC scratch buffer covers a 255-byte name and every field prefix. */
constexpr std::size_t kEncodedRowCapacity = 288;
/** One MsgB scratch buffer covers the name, both wrapper messages, and every field prefix. */
constexpr std::size_t kEncodedEntitlementCapacity = 96;
/** Canonical UUID hyphens sit at text offsets 8, 13, 18, and 23. */
constexpr std::array<std::size_t, 4> kGuidHyphens{8, 13, 18, 23};

/** @param value ASCII byte. @return True for lowercase hex text. */
[[nodiscard]] bool is_lower_hex(char value) noexcept {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

/**
 * The Client compares 37 bytes with the token it got at fetch time. A shorter id makes that
 * compare read past the string.
 * @param guid Candidate ContentConfig fetch id.
 * @return True for canonical UUID text.
 */
[[nodiscard]] bool valid_guid(std::string_view guid) noexcept {
    if (guid.size() != state::content_manifest::kGuidCapacity - 1) {
        return false;
    }
    for (std::size_t index = 0; index < guid.size(); ++index) {
        bool hyphen = false;
        for (const std::size_t offset : kGuidHyphens) {
            hyphen = hyphen || index == offset;
        }
        if ((hyphen && guid[index] != '-') || (!hyphen && !is_lower_hex(guid[index]))) {
            return false;
        }
    }
    return true;
}

/**
 * Encodes one whole MsgC row into fixed scratch storage.
 * @param row Public package row.
 * @param output Cleared fixed nested-message storage.
 * @param size Cleared output receiving the nested byte count.
 * @return True when every required MsgC field fits.
 */
[[nodiscard]] bool encode_row(const state::content_manifest::Row& row,
                              std::span<std::byte> output,
                              std::size_t& size) noexcept {
    size = 0;
    protobuf::Writer writer(output);
    // The stored length is the only field that can reach past the row's own name storage.
    const std::size_t nameLength =
        (std::min)(static_cast<std::size_t>(row.nameLength), row.name.size());
    const auto name = std::as_bytes(std::span(row.name.data(), nameLength));
    if (!writer.write_varint(kOfferKeyField, state::entitlements::kOfferKey)
        || !writer.write_length_delimited(kPackageNameField, name)
        || !writer.write_varint(kPackageIdField, row.packageId)
        || !writer.write_varint(kBuildSignatureField, row.buildSignature)) {
        return false;
    }
    size = writer.size();
    return true;
}

} // namespace

/**
 * Encodes the raw ContentConfig body: field-2 entitlement definitions, field-3 package rows, and
 * the field-5 public GUID.
 */
bool encode(const state::entitlements::Table& entitlements,
            std::span<const state::content_manifest::Row> rows,
            std::string_view guid,
            std::span<std::byte> output,
            std::size_t& size) noexcept {
    size = 0;
    if (!valid_guid(guid)) {
        return false;
    }
    protobuf::Writer writer(output);
    // Only the count needs a bound. It decides how much of the fixed table is written.
    const std::size_t definitionCount = (std::min)(entitlements.count, entitlements.entries.size());
    const std::span<const state::entitlements::Entitlement> definitions =
        std::span(entitlements.entries).first(definitionCount);
    for (std::size_t index = 0; index < definitions.size(); ++index) {
        std::array<std::byte, kEncodedEntitlementCapacity> nested{};
        std::size_t nestedSize = 0;
        if (!encode_entitlement(
                definitions[index], state::entitlements::handle(index), nested, nestedSize)
            || !writer.write_length_delimited(kEntitlementsField,
                                              std::span(nested).first(nestedSize))) {
            return false;
        }
    }
    for (const state::content_manifest::Row& row : rows) {
        std::array<std::byte, kEncodedRowCapacity> nested{};
        std::size_t nestedSize = 0;
        if (!encode_row(row, nested, nestedSize)
            || !writer.write_length_delimited(kPackageRowsField,
                                              std::span(nested).first(nestedSize))) {
            return false;
        }
    }
    const auto guidBytes = std::as_bytes(std::span(guid.data(), guid.size()));
    if (!writer.write_length_delimited(kGuidField, guidBytes)) {
        return false;
    }
    size = writer.size();
    return true;
}

} // namespace sunrise::middleware::content::manifest
