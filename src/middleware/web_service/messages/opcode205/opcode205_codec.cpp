#include <algorithm>
#include <array>

#include "../../../encoding/byte_order.h"
#include "../../status_fields.h"
#include "../family5/family5_codec.h"
#include "../opcode205.h"

namespace sunrise::middleware::web_service::messages::opcode205 {
namespace {

/** 897 bytes hold the largest bounded family-5 response. */
constexpr std::size_t kMaximumResponseSize = 897;

} // namespace

/** Reports whether this request is the family-5 snapshot. */
bool parse_request(const Message& message) noexcept {
    return message.opcode == kOpcode;
}

/** Encodes status, family-5 State in descriptor order, and cleared trailers. */
bool encode_response(const Message& message,
                     const state::InvestmentState& investment,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    if (!parse_request(message) || !family5::valid(investment.family5)) {
        return false;
    }
    std::array<std::byte, kMaximumResponseSize> staged{};
    encoding::write_u16_be(std::span(staged).first<encoding::kU16Size>(), message.opcode);
    encoding::write_u32_be(std::span(staged).subspan<encoding::kU16Size, encoding::kU32Size>(),
                           message.transactionId);

    encoding::bits::Writer writer(std::span(staged).subspan(kEnvelopeHeaderSize));
    bool encoded = status::write_fields(writer, ResponseShape::statusOnly, StatusResponse{});
    encoded = encoded && family5::write(writer, investment.family5)
              && writer.write(0U, kAbsentTrailerWidth);

    std::size_t payloadSize = 0;
    if (!encoded || !writer.finish(payloadSize)) {
        return false;
    }
    const std::size_t responseSize = kEnvelopeHeaderSize + payloadSize;
    if (responseSize > output.size()) {
        return false;
    }
    std::copy_n(staged.begin(), responseSize, output.begin());
    written = responseSize;
    return true;
}

} // namespace sunrise::middleware::web_service::messages::opcode205
