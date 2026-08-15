#include "web_service_envelope.h"

#include "../encoding/bit_writer.h"
#include "../encoding/byte_order.h"
#include "status_fields.h"

namespace sunrise::middleware::web_service {

/** Parses the fixed big-endian Web Service header without interpreting its payload. */
bool parse_request(std::span<const std::byte> input, Message& message) noexcept {
    message = {};
    if (input.size() < kEnvelopeHeaderSize) {
        return false;
    }
    message.opcode = encoding::read_u16_be(input.first<encoding::kU16Size>());
    message.transactionId =
        encoding::read_u32_be(input.subspan<encoding::kU16Size, encoding::kU32Size>());
    message.payload = input.subspan(kEnvelopeHeaderSize);
    return true;
}

/** Encodes the echoed header, the chosen status layout, and two absent trailer flags. */
bool encode_response(const Message& request,
                     ResponseShape shape,
                     const StatusResponse& status,
                     std::span<std::byte> output,
                     std::size_t& written) noexcept {
    written = 0;
    if (output.size() < kEnvelopeHeaderSize) {
        return false;
    }
    encoding::write_u16_be(output.first<encoding::kU16Size>(), request.opcode);
    encoding::write_u32_be(output.subspan<encoding::kU16Size, encoding::kU32Size>(),
                           request.transactionId);

    encoding::bits::Writer writer(output.subspan(kEnvelopeHeaderSize));
    bool encoded = sunrise::middleware::web_service::status::write_fields(writer, shape, status);
    encoded = encoded && writer.write(0, kAbsentTrailerWidth);

    std::size_t payloadSize = 0;
    if (!encoded || !writer.finish(payloadSize)) {
        return false;
    }
    written = kEnvelopeHeaderSize + payloadSize;
    return true;
}

} // namespace sunrise::middleware::web_service
