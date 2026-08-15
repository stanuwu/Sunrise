#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::bap::activity_message::client_identity {

/** Logical fields in the client's per-join identity record. */
struct ClientIdentity final {
    std::uint64_t memberKey{};
    /** Decoded signed value from the 10-bit bias-1 field. */
    std::int32_t field1{};
    /** Decoded signed value from the 32-bit sign-biased field. */
    std::int32_t field2{};
    /** First opaque identity that changes between joins. */
    std::uint64_t field3{};
    std::uint64_t accountSoid{};
    /** Identity field left unnamed until its role is confirmed. */
    std::uint64_t field5{};
    /** Second opaque identity that changes between joins. */
    std::uint64_t field6{};
};

/** Client identity updates use activity message type 23. */
inline constexpr std::uint32_t kMessageType = 23;
/** The identity schema carries 362 meaningful bits. */
inline constexpr std::size_t kMeaningfulBitCount = 362;
/** 6 trailing padding bits finish the 46-byte identity body. */
inline constexpr std::size_t kEncodedSize = 46;

/**
 * Parses the fixed client-identity prefix into logical signed and identity fields.
 * @param input Activity message body holding the whole 46-byte identity prefix.
 * @param identity Cleared first. Filled in only on success.
 * @return True when every field and all 6 trailing padding bits are there.
 */
[[nodiscard]] bool parse_client_identity(std::span<const std::byte> input,
                                         ClientIdentity& identity) noexcept;

} // namespace sunrise::middleware::bap::activity_message::client_identity
