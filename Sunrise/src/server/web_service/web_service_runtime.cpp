#include "web_service_runtime.h"

#include <array>
#include <algorithm>
#include <cstdio>
#include <cstring>

#include "../../core/logging/log.h"
#include "../../middleware/web_service/messages/opcode205.h"
#include "../../middleware/web_service/messages/opcode206.h"
#include "../../middleware/web_service/messages/opcode501_codec.h"
#include "../../middleware/web_service/messages/opcode503.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/web_service_envelope.h"
#include "../../state/account/account_state.h"
#include "../../state/runtime/runtime.h"
#include "opcode_routes.h"

namespace sunrise::server::web_service {

/** One log line carries the opcode and its fixed prefix. */
constexpr std::size_t kOpcodeLineCapacity = 64;

/**
 * Logs which Web Service opcode arrived. One svc-10 frame looks like any other in the log, and
 * the opcodes the Client sends are what drive its queuez state machine.
 * @param opcode Parsed wire opcode.
 */
void report_opcode(const middleware::web_service::Message& message) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    int written = std::snprintf(line.data(),
                                line.size(),
                                "ev=ws stage=request opcode=%u tx=%u bytes=%zu head=",
                                message.opcode,
                                message.transactionId,
                                message.payload.size());
    constexpr std::size_t kPreviewBytes = 32;
    const std::size_t preview = (std::min)(message.payload.size(), kPreviewBytes);
    for (std::size_t index = 0;
         written > 0 && index < preview
         && static_cast<std::size_t>(written) + 2 < line.size();
         ++index) {
        const int appended = std::snprintf(line.data() + written,
                                           line.size() - static_cast<std::size_t>(written),
                                           "%02X",
                                           std::to_integer<unsigned>(message.payload[index]));
        if (appended <= 0) {
            break;
        }
        written += appended;
    }
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }

    // Mutation requests are still being mapped. Preserve their complete payload in bounded
    // chunks so settings saves and collection withdrawals can be decoded from one test run.
    const bool mutationCandidate = message.opcode == 701
                                   || (message.opcode >= 1200 && message.opcode < 1400);
    constexpr std::size_t kChunkBytes = 48;
    if (!mutationCandidate) {
        return;
    }
    for (std::size_t offset = 0; offset < message.payload.size(); offset += kChunkBytes) {
        std::array<char, core::log::kLineCapacity> chunkLine{};
        int chunkWritten = std::snprintf(chunkLine.data(),
                                         chunkLine.size(),
                                         "ev=ws stage=payload opcode=%u offset=%zu data=",
                                         message.opcode,
                                         offset);
        const std::size_t chunkSize =
            (std::min)(kChunkBytes, message.payload.size() - offset);
        for (std::size_t index = 0;
             chunkWritten > 0 && index < chunkSize
             && static_cast<std::size_t>(chunkWritten) + 2 < chunkLine.size();
             ++index) {
            const int appended =
                std::snprintf(chunkLine.data() + chunkWritten,
                              chunkLine.size() - static_cast<std::size_t>(chunkWritten),
                              "%02X",
                              std::to_integer<unsigned>(message.payload[offset + index]));
            if (appended <= 0) {
                break;
            }
            chunkWritten += appended;
        }
        if (chunkWritten > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             {chunkLine.data(), static_cast<std::size_t>(chunkWritten)});
        }
    }
}

/** One line carries the picked id and whether the selection moved. */
constexpr std::size_t kSelectLineCapacity = 96;

/**
 * Records the player's character pick, which arrives nowhere else.
 * A bad or unknown id leaves the selection alone. The reply is the status pair either way. The
 * Family-4 object move follows this call, and the family-zero pair after it.
 * @param message Parsed select-character request.
 * @param outcome Gets the picked key once the selection has moved in State.
 */
void select_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode504::Request picked;
    if (!middleware::web_service::messages::opcode504::parse_request(message, picked)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws504 stage=parse result=fail");
        return;
    }
    bool changed = false;
    if (!state::set_selected_character(picked.characterSoid, changed)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws504 stage=select result=unknown");
        return;
    }
    outcome.hasSelectedCharacter = true;
    outcome.selectedCharacterSoid = picked.characterSoid;

    std::array<char, kSelectLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=ws504 stage=select result=ok soid=0x%llX changed=%u",
                                      static_cast<unsigned long long>(picked.characterSoid),
                                      static_cast<unsigned>(changed));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

void reacquire_collection_item(const middleware::web_service::Message& message,
                               Outcome& outcome) noexcept {
    if (message.payload.size() != 3) return;
    const std::uint32_t value = std::to_integer<std::uint8_t>(message.payload[0])
                                | (std::to_integer<std::uint8_t>(message.payload[1]) << 8U)
                                | (std::to_integer<std::uint8_t>(message.payload[2]) << 16U);
    if (value > UINT16_MAX) return;
    std::uint64_t instanceSoid = 0;
    if (!state::reacquire_collection_item(static_cast<std::uint16_t>(value), instanceSoid)) {
        core::log::write(core::log::Channel::server, core::log::Level::warn,
                         "ev=ws1820 stage=reacquire result=refused");
        return;
    }
    outcome.hasInventoryMutation = true;
    outcome.acquiredInstanceSoid = instanceSoid;
    std::array<char, 128> line{};
    const int count = std::snprintf(line.data(), line.size(),
                                    "ev=ws1820 stage=reacquire result=ok index=%u soid=0x%llX",
                                    value, static_cast<unsigned long long>(instanceSoid));
    if (count > 0) core::log::write(core::log::Channel::server, core::log::Level::info,
                                    {line.data(), static_cast<std::size_t>(count)});
}

void apply_item_plug(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    if (message.payload.size() != 24) return;
    const auto read_u32_be = [&](std::size_t offset) noexcept {
        std::uint32_t value = 0;
        for (std::size_t byte = 0; byte < 4; ++byte)
            value = (value << 8U) | std::to_integer<std::uint8_t>(message.payload[offset + byte]);
        return value;
    };
    const auto read_u64_be = [&](std::size_t offset) noexcept {
        std::uint64_t value = 0;
        for (std::size_t byte = 0; byte < 8; ++byte)
            value = (value << 8U) | std::to_integer<std::uint8_t>(message.payload[offset + byte]);
        return value;
    };
    const std::uint32_t plugSelector = read_u32_be(0);
    const std::uint32_t socketSelector = read_u32_be(4);
    const std::uint64_t marker = read_u64_be(8);
    const std::uint64_t encodedInstance = read_u64_be(16);
    if (marker != 1 || socketSelector < 2 || (socketSelector - 2) % 4 != 0
        || plugSelector < 0x18000000U) return;
    const std::uint32_t plugIndex = (plugSelector >> 12U) - 0x18000U;
    const std::uint32_t lane = (socketSelector - 2U) / 4U;
    const std::uint64_t instanceSoid = 0x4000000000000000ULL | (encodedInstance >> 2U);
    if (plugIndex > UINT16_MAX || lane >= state::account::inventory::kPlugCapacity
        || !state::apply_item_plug(instanceSoid, static_cast<std::uint8_t>(lane),
                                  static_cast<std::uint16_t>(plugIndex))) {
        core::log::write(core::log::Channel::server, core::log::Level::warn,
                         "ev=ws1901 stage=plug result=refused");
        return;
    }
    outcome.hasInventoryMutation = true;
    outcome.acquiredInstanceSoid = instanceSoid;
    core::log::write(core::log::Channel::server, core::log::Level::info,
                     "ev=ws1901 stage=plug result=ok");
}

/**
 * Answers a request whose own codec refused with the bare correlated echo.
 * The Client matches on the echoed transaction id. A missing body is worse than a thin one. It
 * under-runs the decoder and takes the BAP connection down.
 * @param message Parsed request whose correlation fields are echoed.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size in bytes.
 * @return True when the echo fits.
 */
bool encode_echo(const middleware::web_service::Message& message,
                 std::span<std::byte> response,
                 std::size_t& written) noexcept {
    std::array<char, kOpcodeLineCapacity> line{};
    const int count = std::snprintf(
        line.data(), line.size(), "ev=ws stage=body result=echo opcode=%u", message.opcode);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
    namespace ws = middleware::web_service;
    return ws::encode_response(
        message, ws::ResponseShape::generic, ws::StatusResponse{}, response, written);
}

/**
 * Parses and answers one Web Service request with its whole descriptor layout.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @return False only when the envelope header does not parse.
 */
bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written) noexcept {
    Outcome outcome;
    return consume(request, response, written, outcome);
}

/**
 * Parses one request, encodes its response, and publishes checked side effects last.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @param outcome Gets a valid family selector only after the response is encoded.
 * @return False only when the envelope header does not parse.
 */
bool consume(std::span<const std::byte> request,
             std::span<std::byte> response,
             std::size_t& written,
             Outcome& outcome) noexcept {
    written = 0;
    outcome = {};
    middleware::web_service::Message message;
    if (!middleware::web_service::parse_request(request, message)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws stage=parse result=fail");
        return false;
    }
    report_opcode(message);

    if (message.opcode == middleware::web_service::messages::opcode205::kOpcode) {
        const auto investment = state::investment_snapshot();
        return middleware::web_service::messages::opcode205::encode_response(
                   message, investment, response, written)
               || encode_echo(message, response, written);
    }

    if (message.opcode == middleware::web_service::messages::opcode503::kOpcode) {
        middleware::web_service::messages::opcode503::Request bootstrap;
        const bool parsed =
            middleware::web_service::messages::opcode503::parse_request(message, bootstrap);
        // The request's own key is echoed and adopted. An authored id here costs the ship and the
        // banner.
        if (!bootstrap.hasPrimarySoid) {
            bootstrap.primarySoid = state::account_snapshot().primarySoid;
        }
        const auto investment = state::investment_snapshot();
        if (!parsed
            || !middleware::web_service::messages::opcode503::encode_response(
                message, bootstrap, investment, response, written)) {
            return encode_echo(message, response, written);
        }
        if (bootstrap.hasPrimarySoid && !state::set_primary_soid(bootstrap.primarySoid)) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             "ev=ws503 stage=adopt result=fail");
        }
        return true;
    }

    if (message.opcode == middleware::web_service::messages::opcode501::kOpcode) {
        // Returns a SOID family three already publishes. The request body is not parsed.
        const std::uint64_t characterSoid =
            state::account::selected_character_soid(state::account_snapshot());
        return middleware::web_service::messages::opcode501::encode_response(
                   message, characterSoid, response, written)
               || encode_echo(message, response, written);
    }

    // A subscribe whose body does not parse is still answered; only the subscription is dropped.
    middleware::queuez::Subscription subscription;
    const bool subscribes =
        message.opcode == middleware::web_service::messages::opcode206::kOpcode
        && middleware::web_service::messages::opcode206::parse_request(message, subscription);

    middleware::web_service::ResponseShape shape{};
    resolve_response_shape(message.opcode, shape);
    if (!middleware::web_service::encode_response(
            message, shape, middleware::web_service::StatusResponse{}, response, written)) {
        return encode_echo(message, response, written);
    }
    if (subscribes) {
        // Publish the subscription only after its correlated response is complete.
        outcome.hasSubscription = true;
        outcome.subscription = subscription;
        return true;
    }
    if (message.opcode == middleware::web_service::messages::opcode504::kOpcode) {
        // The selection is State, not a response field, so it publishes after the reply encodes.
        select_character(message, outcome);
    } else if (message.opcode == 1820) {
        reacquire_collection_item(message, outcome);
    } else if (message.opcode == 1901) {
        apply_item_plug(message, outcome);
    }
    return true;
}

} // namespace sunrise::server::web_service
