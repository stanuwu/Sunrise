#include "account_translation_response.h"

#include "../../encoding/byte_order.h"

namespace sunrise::middleware::bap::account_translation {
namespace {

/** Fixed offsets for the complete svc-23 identity request. */
struct RequestLayout {
    /** The request starts with its big-endian entry count. */
    static constexpr std::size_t entryCount = 0;
    /** The first one-byte type follows the entry count. */
    static constexpr std::size_t typeA = entryCount + encoding::kU16Size;
    /** The second one-byte type follows the first type. */
    static constexpr std::size_t typeB = typeA + sizeof(std::uint8_t);
    /** The 8-byte big-endian request identity follows both types. */
    static constexpr std::size_t identity = typeB + sizeof(std::uint8_t);
    /** Svc 23 has no fields after its request identity. */
    static constexpr std::size_t size = identity + encoding::kU64Size;
};

/** Fixed offsets for the complete svc-24 account response. */
struct ResponseLayout {
    /** The account SOID follows the echoed request fields. */
    static constexpr std::size_t accountSoid = RequestLayout::size;
    /** Svc 24 has no fields after its 8-byte account SOID. */
    static constexpr std::size_t size = accountSoid + encoding::kU64Size;
    /** A zero-entry answer stops after the count and both types. */
    static constexpr std::size_t emptySize = RequestLayout::identity;
};

/** Entry count svc 24 sends when it has an identity to pair. */
constexpr std::uint16_t kAnsweredEntryCount = 1;
/** The zero-entry count of the empty answer. */
constexpr std::uint16_t kEmptyEntryCount = 0;
/** Only discriminator A wire value 12 means an 8-byte identity list. */
constexpr std::byte kIdentityTypeA{0x0C};
/** Discriminator B wire value 255 is the type svc 24 can always send. */
constexpr std::byte kTypeB{0xFF};
/** A zero account SOID names no account, so there is nothing to pair the identity with. */
constexpr std::uint64_t kMissingAccountSoid = 0;

/**
 * Writes the empty svc-24 answer: zero entries and the always-valid types. The Client's
 * bitstream over-runs on an empty body, so a request with no identity to pair still gets this.
 * @param output Caller-owned response-body storage.
 * @param written Receives the empty answer size, or zero when it does not fit.
 * @return True when the empty answer fits output storage.
 */
[[nodiscard]] bool encode_empty(std::span<std::byte> output, std::size_t& written) noexcept {
    written = 0;
    if (output.size() < ResponseLayout::emptySize) {
        return false;
    }
    encoding::write_u16_be(output.subspan<RequestLayout::entryCount, encoding::kU16Size>(),
                           kEmptyEntryCount);
    output[RequestLayout::typeA] = kTypeB;
    output[RequestLayout::typeB] = kTypeB;
    written = ResponseLayout::emptySize;
    return true;
}

} // namespace

/** Encodes the svc-24 answer to one svc-23 identity request. */
bool encode_response(std::span<const std::byte> requestBody,
                     std::uint64_t accountSoid,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    if (requestBody.size() < RequestLayout::size || accountSoid == kMissingAccountSoid
        || output.size() < ResponseLayout::size) {
        return encode_empty(output, written);
    }
    const std::uint16_t entryCount =
        encoding::read_u16_be(requestBody.subspan<RequestLayout::entryCount, encoding::kU16Size>());
    if (entryCount < kAnsweredEntryCount || requestBody[RequestLayout::typeA] != kIdentityTypeA) {
        return encode_empty(output, written);
    }

    // The identity is echoed as-is, so no re-encoding of it can disagree with the request.
    const std::uint64_t identity =
        encoding::read_u64_be(requestBody.subspan<RequestLayout::identity, encoding::kU64Size>());
    encoding::write_u16_be(output.subspan<RequestLayout::entryCount, encoding::kU16Size>(),
                           kAnsweredEntryCount);
    output[RequestLayout::typeA] = kIdentityTypeA;
    output[RequestLayout::typeB] = kTypeB;
    encoding::write_u64_be(output.subspan<RequestLayout::identity, encoding::kU64Size>(), identity);
    encoding::write_u64_be(output.subspan<ResponseLayout::accountSoid, encoding::kU64Size>(),
                           accountSoid);
    written = ResponseLayout::size;
    return true;
}

} // namespace sunrise::middleware::bap::account_translation
