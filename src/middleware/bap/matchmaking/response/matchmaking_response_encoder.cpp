#include "matchmaking_response_encoder.h"

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
        return encode_empty_message(kConfigurationField, output, written);
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
