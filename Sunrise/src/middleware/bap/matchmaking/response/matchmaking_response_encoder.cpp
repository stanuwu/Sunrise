#include "matchmaking_response_encoder.h"

#include <array>

#include "../../../protobuf/codec.h"
#include "matchmaking_dynamic_response.h"

namespace sunrise::middleware::bap::matchmaking::response {
namespace {

using protobuf::Writer;

/** Search results use service-43 field 3. */
constexpr std::uint32_t kSearchResultsField = 3;
/** Matchmaking configuration uses service-43 field 4. */
constexpr std::uint32_t kConfigurationField = 4;
/** Live matchmaking statistics use service-43 field 8. */
constexpr std::uint32_t kLiveStatsField = 8;
/** The activity matchmaking configuration is field 1 of the kind-4 body. */
constexpr std::uint32_t kActivityConfigurationField = 1;
/** Field 8 of the activity configuration: a party this size or larger counts as a big fireteam. */
constexpr std::uint32_t kBigFireteamThresholdField = 8;
/**
 * 7 is above the largest fireteam, so no party counts as big and a full fireteam passes the
 * composition check. A big party with no team to hold it is refused. The retail value is unread.
 */
constexpr std::uint64_t kBigFireteamThreshold = 7;
/** Two nested varint fields fit inside this. */
constexpr std::size_t kConfigurationScratchSize = 32;

/**
 * Encodes one present, zero-length submessage.
 * @param fieldNumber Service-43 field whose presence finishes the request.
 * @param output Caller-owned response storage.
 * @param written Receives 2 encoded bytes or zero on failure.
 * @return True when the empty submessage fits.
 */
[[nodiscard]] bool encode_empty_message(std::uint32_t fieldNumber,
                                        std::span<std::byte> output,
                                        std::size_t& written) noexcept {
    Writer writer(output);
    if (!writer.write_length_delimited(fieldNumber, {})) {
        return false;
    }
    written = writer.size();
    return true;
}

/**
 * Encodes the kind-4 matchmaking configuration.
 * The client copies field 1's field 8 into the big-fireteam threshold its composition check
 * counts against. An empty body leaves it 0, and a solo fireteam is then rejected as too large.
 * @param output Caller-owned response storage.
 * @param written Receives the encoded size or zero on failure.
 * @return True when the body fits.
 */
[[nodiscard]] bool encode_configuration(std::span<std::byte> output,
                                        std::size_t& written) noexcept {
    std::array<std::byte, kConfigurationScratchSize> activity{};
    Writer activityWriter(activity);
    if (!activityWriter.write_varint(kBigFireteamThresholdField, kBigFireteamThreshold)) {
        return false;
    }
    std::array<std::byte, kConfigurationScratchSize> body{};
    Writer bodyWriter(body);
    if (!bodyWriter.write_length_delimited(kActivityConfigurationField,
                                           std::span(activity).first(activityWriter.size()))) {
        return false;
    }
    Writer writer(output);
    if (!writer.write_length_delimited(kConfigurationField,
                                       std::span(body).first(bodyWriter.size()))) {
        return false;
    }
    written = writer.size();
    return true;
}

} // namespace

/** Encodes one service-43 body. The service-42 request kind picks the shape. */
bool encode(const Response& response, std::span<std::byte> output, std::size_t& written) noexcept {
    written = 0;
    switch (response.kind) {
    case RequestKind::none:
    case RequestKind::advertisementDelete:
    case RequestKind::rejoinAdvertisementDelete:
        return true;
    case RequestKind::sessionSearch:
        return encode_empty_message(kSearchResultsField, output, written);
    case RequestKind::advertisementUpdate:
        return encode_advertisement_id(true, response.advertisementId, output, written);
    case RequestKind::configuration:
        return encode_configuration(output, written);
    case RequestKind::rejoinAdvertisementUpdate:
        return encode_advertisement_id(false, response.advertisementId, output, written);
    case RequestKind::locateSession:
        if (response.descriptor.empty()) {
            return true;
        }
        return encode_locate_result(response.advertisementId, response.descriptor, output, written);
    case RequestKind::liveStats:
        return encode_empty_message(kLiveStatsField, output, written);
    }
    return false;
}

} // namespace sunrise::middleware::bap::matchmaking::response
