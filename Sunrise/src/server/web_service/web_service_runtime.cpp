#include "web_service_runtime.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../middleware/encoding/bit_reader.h"
#include "../../middleware/encoding/byte_order.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/messages/opcode205.h"
#include "../../middleware/web_service/messages/opcode206.h"
#include "../../middleware/web_service/messages/opcode501_codec.h"
#include "../../middleware/web_service/messages/opcode503.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode601/opcode601_codec.h"
#include "../../middleware/web_service/messages/opcode801.h"
#include "../../middleware/web_service/messages/opcode901/opcode901_codec.h"
#include "../../middleware/web_service/messages/opcode903.h"
#include "../../middleware/web_service/web_service_envelope.h"
#include "../../state/account/account_state.h"
#include "../../state/build_data/runtime.h"
#include "../../state/runtime/runtime.h"
#include "opcode_routes.h"
#include "web_service_actions.h"

namespace sunrise::server::web_service {

/** One ordinary event line carries an opcode and its fixed prefix. */
constexpr std::size_t kOpcodeLineCapacity = 64;
/** A request trace keeps enough payload to identify an item-action descriptor. */
constexpr std::size_t kRequestPayloadTraceBytes = 192;
/** Marks a trace that stopped at the cap, so a short hex string is not read as a short payload. */
constexpr std::string_view kTruncated = " truncated=1";
/** Web Service opcode used by the Character screen's Equip action. */
constexpr std::uint16_t kEquipOpcode = 403;
/** Web Service opcode used by the Character screen's Unequip action. */
constexpr std::uint16_t kUnequipOpcode = 404;
/** Web Service opcode used by item-state actions such as finisher Favorite. */
constexpr std::uint16_t kItemStateOpcode = 406;
/** Web Service opcode used by the Character screen's Dismantle action. */
constexpr std::uint16_t kItemDismantleOpcode = 402;
/** Web Service opcode used by Collections to create one item instance. */
constexpr std::uint16_t kItemAcquisitionOpcode = 1820;
/** The mutation variant's first alternative is the empty one, so index zero prepared nothing. */
constexpr std::size_t kNoMutation = 0;
/**
 * Logical status of a refused action. The descriptor biases logical zero to the wire success the
 * Client expects, so any other logical value reports a refusal. Its five bits hold no error
 * taxonomy, so one code covers every reason and the log line names the actual one.
 */
constexpr std::int32_t kRefusedStatus = 1;

/**
 * Logs the Web Service opcode and a bounded payload trace.
 * One svc-10 frame looks like any other, and the opcode drives the client's queuez state machine.
 * @param message Parsed request envelope and borrowed payload.
 */
void report_request(const middleware::web_service::Message& message) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=ws stage=request opcode=%u transaction=%u payload_bytes=%zu payload_hex=",
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      message.payload.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }

    std::size_t length = static_cast<std::size_t>(prefix);
    const std::size_t traced =
        (std::min)(message.payload.size(), static_cast<std::size_t>(kRequestPayloadTraceBytes));
    (void)core::log::append_hex(line, length, message.payload.first(traced));
    if (traced != message.payload.size() && length + kTruncated.size() < line.size()) {
        std::memcpy(line.data() + length, kTruncated.data(), kTruncated.size());
        length += kTruncated.size();
    }
    if (length != 0) {
        core::log::write(core::log::Channel::server, core::log::Level::info, {line.data(), length});
    }
}

/** One refusal line carries both request indices, the clock presence, and the clock verdict. */
constexpr std::size_t kPurchaseLineCapacity = 128;
/**
 * Status code answered to a purchase request.
 * Any non-zero value refuses. Zero is the success code, so it must not be used here.
 */
constexpr std::int32_t kPurchaseRefusedCode = 1;

/**
 * Reads the server's own clock for the purchase clock rule.
 * The system clock counts from the Unix epoch, which is the same base the request field uses.
 * @return Current time in Unix seconds.
 */
[[nodiscard]] std::int64_t server_clock_seconds() noexcept {
    const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch).count();
}

/**
 * Refuses one vendor purchase and answers it.
 * No award, cost or stock rule exists yet, so no purchase can succeed. The refusal must still be
 * answered, because no answer holds the head of the client's pending queue.
 * @param message Parsed purchase request.
 * @param response Response-body storage owned by the caller.
 * @param written Receives the encoded response size.
 * @return True when the refusal was encoded.
 */
[[nodiscard]] bool refuse_purchase(const middleware::web_service::Message& message,
                                   std::span<std::byte> response,
                                   std::size_t& written) noexcept {
    namespace purchase_codec = middleware::web_service::messages::opcode901;
    purchase_codec::Request purchase;
    const bool parsed = purchase_codec::parse_request(message, purchase);
    // The clock verdict is logged, never acted on. Nothing can pass while the route refuses.
    const auto policy = purchase_codec::check_clock(purchase, server_clock_seconds());
    std::array<char, kPurchaseLineCapacity> line{};
    const int length =
        parsed ? std::snprintf(
                     line.data(),
                     line.size(),
                     "ev=ws901 stage=purchase result=refuse vendor=%d sale=%d present=%u policy=%s",
                     static_cast<int>(purchase.vendorIndex),
                     static_cast<int>(purchase.saleIndex),
                     purchase.hasClock ? 1U : 0U,
                     purchase_codec::clock_policy_name(policy))
               : std::snprintf(line.data(),
                               line.size(),
                               "ev=ws901 stage=purchase result=refuse reason=parse");
    if (length > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::error,
                         {line.data(), static_cast<std::size_t>(length)});
    }
    middleware::web_service::StatusResponse status{};
    status.code = kPurchaseRefusedCode;
    // The trailing bool drives a local action effect on the client, so it stays clear.
    status.trailingBool = false;
    return middleware::web_service::encode_response(
        message,
        middleware::web_service::ResponseShape::statusPairWithBool,
        status,
        response,
        written);
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
 * Parses one request, prepares any action it names, and encodes the reply that reports it.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size, or zero when the header does not parse.
 * @param outcome Gets the prepared action for the caller to publish, and is left empty when
 * the action was refused or the reply could not be encoded.
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
    report_request(message);

    if (message.opcode == middleware::web_service::messages::opcode205::kOpcode) {
        state::InvestmentState investment{};
        return (state::investment_snapshot(investment)
                && middleware::web_service::messages::opcode205::encode_response(
                    message, investment, response, written))
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
        state::InvestmentState investment{};
        if (!parsed || !state::investment_snapshot(investment)
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

    // Runs before the shared response-shape path, which would answer the success status.
    if (message.opcode == middleware::web_service::messages::opcode901::kOpcode) {
        return refuse_purchase(message, response, written)
               || encode_echo(message, response, written);
    }

    if (message.opcode == middleware::web_service::messages::opcode601::kOpcode) {
        return middleware::web_service::messages::opcode601::encode_response(
                   message, response, written)
               || encode_echo(message, response, written);
    }

    // A subscribe whose body does not parse is still answered; only the subscription is dropped.
    middleware::queuez::Subscription subscription;
    const bool subscribes =
        message.opcode == middleware::web_service::messages::opcode206::kOpcode
        && middleware::web_service::messages::opcode206::parse_request(message, subscription);

    // The action runs before its reply is encoded, because the reply reports whether it worked.
    // An action fills the outcome only once it has prepared its whole transition, so an outcome
    // still empty afterwards is that action refusing the request. Nothing is published here.
    bool dispatched = true;
    if (message.opcode == middleware::web_service::messages::opcode504::kOpcode) {
        select_character(message, outcome);
    } else if (message.opcode == kItemDismantleOpcode) {
        dismantle_item(message, outcome);
    } else if (message.opcode == kEquipOpcode) {
        mutate_equipment(message, false, outcome);
    } else if (message.opcode == kUnequipOpcode) {
        mutate_equipment(message, true, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode801::kOpcode) {
        mutate_subclass_selection(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode903::kOpcode) {
        mutate_socket_plug(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode1901::kOpcode) {
        mutate_equipped_socket_plug(message, outcome);
    } else if (message.opcode == kItemStateOpcode) {
        mutate_item_state(message, outcome);
    } else if (message.opcode == kItemAcquisitionOpcode) {
        acquire_item(message, outcome);
    } else {
        dispatched = false;
    }
    const bool prepared = outcome.hasSelectedCharacter || outcome.mutation.index() != kNoMutation;

    middleware::web_service::ResponseShape shape{};
    resolve_response_shape(message.opcode, shape);
    middleware::web_service::StatusResponse status{};
    if (dispatched && !prepared) {
        status.code = kRefusedStatus;
    }
    if (!middleware::web_service::encode_response(message, shape, status, response, written)) {
        // The echo carries no status, so nothing may be published against it.
        outcome = {};
        return encode_echo(message, response, written);
    }
    if (subscribes) {
        // Publish the subscription only after its correlated response is complete.
        outcome.hasSubscription = true;
        outcome.subscription = subscription;
    }
    return true;
}

} // namespace sunrise::server::web_service
