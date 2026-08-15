#include "signon_extended_fields.h"

#include <array>
#include <cstdio>
#include <span>
#include <string_view>

namespace sunrise::middleware::signon::extended {
namespace {

/** Success field 12 carries the optional extended sub-message. */
constexpr unsigned kExtendedField = 12;
/** Success fields 14 through 16 carry the three config URLs. */
constexpr unsigned kFirstUrlField = 14;
/** Extended sub-message field numbers. */
constexpr unsigned kExtendedFlagField = 1;
constexpr unsigned kExtendedFirstBlobField = 3;
constexpr unsigned kExtendedSecondBlobField = 4;
/** Fixed flag value. The Client accepts it, and no reader is proven. */
constexpr std::uint64_t kExtendedFlag = 1;
/** Both extended blobs are 16 bytes. */
constexpr std::size_t kExtendedBlobSize = 16;
/** Any non-secret filler works. Neither blob has a proven Client reader. */
constexpr std::byte kFirstBlobFill{0x11};
constexpr std::byte kSecondBlobFill{0x22};
/** The extended sub-message holds at most two blobs, one flag, and their field keys. */
constexpr std::size_t kExtendedBufferSize = 64;
/** Config URL suffix letters, one per field from 14 upward. */
constexpr std::array<char, 3> kUrlSuffixes{'a', 'b', 'c'};
/** A dotted-quad loopback URL fits inside this fixed text buffer. */
constexpr std::size_t kUrlCapacity = 48;
/** Relay addresses are packed high byte first, matching the required relay field. */
constexpr unsigned kAddressByteBits = 8;
constexpr std::uint32_t kAddressByteMask = 0xFF;

/**
 * Formats one config URL for the relay address.
 * @param relayAddress Relay address packed high byte first.
 * @param suffix Trailing path letter.
 * @param size Cleared, then receives the text length.
 * @return True when the whole URL fits.
 */
[[nodiscard]] bool format_url(std::uint32_t relayAddress,
                              char suffix,
                              std::span<char> output,
                              std::size_t& size) noexcept {
    size = 0;
    const int written = std::snprintf(output.data(),
                                      output.size(),
                                      "http://%u.%u.%u.%u/cfg_%c/",
                                      (relayAddress >> (3 * kAddressByteBits)) & kAddressByteMask,
                                      (relayAddress >> (2 * kAddressByteBits)) & kAddressByteMask,
                                      (relayAddress >> kAddressByteBits) & kAddressByteMask,
                                      relayAddress & kAddressByteMask,
                                      suffix);
    if (written <= 0 || static_cast<std::size_t>(written) >= output.size()) {
        return false;
    }
    size = static_cast<std::size_t>(written);
    return true;
}

/**
 * Encodes the extended sub-message into fixed storage.
 * @param size Cleared, then receives the encoded byte count.
 * @return True when the flag and both blobs fit.
 */
[[nodiscard]] bool encode_extended(std::span<std::byte> output, std::size_t& size) noexcept {
    size = 0;
    std::array<std::byte, kExtendedBlobSize> first{};
    std::array<std::byte, kExtendedBlobSize> second{};
    first.fill(kFirstBlobFill);
    second.fill(kSecondBlobFill);
    Writer writer(output);
    if (!writer.varint(kExtendedFlagField, kExtendedFlag)
        || !writer.bytes(kExtendedFirstBlobField, first)
        || !writer.bytes(kExtendedSecondBlobField, second)) {
        return false;
    }
    size = writer.size();
    return true;
}

} // namespace

/** Appends the optional SignOn success fields: the extended blob and the three config URLs. */
bool append(Writer& success, std::uint32_t relayAddress) noexcept {
    std::array<std::byte, kExtendedBufferSize> extended{};
    std::size_t extendedSize = 0;
    if (!encode_extended(extended, extendedSize)
        || !success.bytes(kExtendedField, std::span(extended).first(extendedSize))) {
        return false;
    }
    for (std::size_t index = 0; index < kUrlSuffixes.size(); ++index) {
        std::array<char, kUrlCapacity> url{};
        std::size_t length = 0;
        if (!format_url(relayAddress, kUrlSuffixes[index], url, length)
            || !success.bytes(kFirstUrlField + static_cast<unsigned>(index),
                              text_bytes(std::string_view(url.data(), length)))) {
            return false;
        }
    }
    return true;
}

} // namespace sunrise::middleware::signon::extended
