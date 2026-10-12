#include "web_service_runtime.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>

#include "../../core/logging/log.h"
#include "../../middleware/web_service/messages/opcode1801.h"
#include "../../middleware/web_service/messages/opcode1821.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/messages/opcode205.h"
#include "../../middleware/web_service/messages/opcode206.h"
#include "../../middleware/web_service/messages/opcode2400.h"
#include "../../middleware/web_service/messages/opcode501_codec.h"
#include "../../middleware/web_service/messages/opcode503.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode601/opcode601_codec.h"
#include "../../middleware/web_service/messages/opcode701/opcode701_codec.h"
#include "../../middleware/web_service/messages/opcode702.h"
#include "../../middleware/web_service/messages/opcode801.h"
#include "../../middleware/web_service/messages/opcode901/opcode901_codec.h"
#include "../../middleware/web_service/messages/opcode903.h"
#include "../../middleware/web_service/messages/opcode904/opcode904_codec.h"
#include "../../middleware/web_service/web_service_envelope.h"
#include "../../state/account/account_state.h"
#include "../../state/activity/membership/activity_membership_query.h"
#include "../../state/build_data/runtime.h"
#include "../../state/runtime/runtime.h"
#include "opcode_routes.h"
#include "web_service_actions.h"

namespace sunrise::server::web_service {

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
/**
 * Logical status of a refused action. The descriptor biases logical zero to the wire success the
 * Client expects, so any other logical value reports a refusal. Its five bits hold no error
 * taxonomy, so one code covers every reason and the log line names the actual one.
 */
constexpr std::int32_t kRefusedStatus = 1;

/**
 * Opcodes whose reply may name a resident the client no longer holds.
 * Kept sorted; the lookup below is a binary search.
 */
constexpr auto kResidentDependentOpcodes =
    std::to_array<std::uint16_t>({402, 403, 404, 406, 504, 903, 1801, 1820, 1901, 2400});

/** One refusal line carries both request indices, the clock presence, and the clock verdict. */
constexpr std::size_t kPurchaseLineCapacity = 128;
constexpr std::size_t kEchoLineCapacity = 64;
/** Season of Arrivals artifact vendor row in the installed build's vendor index. */
constexpr std::int16_t kArtifactVendorIndex = 430;
/** Glimmer the artifact vendor charges to reset its mods, as retail charges. */
constexpr std::int32_t kArtifactResetGlimmerCost = 20'000;
/** The artifact vendor's reset row. Every lower row unlocks one mod tier. */
constexpr std::uint16_t kArtifactResetSaleIndex = 5;

/**
 * Reads the server's own clock for the purchase clock rule.
 * The system clock counts from the Unix epoch, which is the same base the request field uses.
 * @return Current time in Unix seconds.
 */
[[nodiscard]] std::int64_t server_clock_seconds() noexcept {
    const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch).count();
}

/** Issues a strictly increasing family-5 clock, including multiple requests in one second. */
std::uint64_t next_family5_clock() noexcept {
    static std::atomic<std::uint64_t> issued{0};
    const auto wall = static_cast<std::uint64_t>(server_clock_seconds());
    std::uint64_t previous = issued.load(std::memory_order_relaxed);
    std::uint64_t next = 0;
    do {
        next = wall > previous ? wall : previous + 1;
    } while (!issued.compare_exchange_weak(previous, next, std::memory_order_relaxed));
    return next;
}

/** Records the authoritative world state carried by the client's character write-back. */
bool note_character_writeback(
    const middleware::web_service::Message& message,
    std::span<const state::account::inventory::PresentedItemRow> presentation) noexcept {
    namespace writeback = middleware::web_service::messages::opcode702;
    writeback::Request request{};
    const bool parsed = writeback::parse_request(message, request);
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=writeback result=%s world_state=%u "
                                      "activity=%d/%d/%d selector=%d",
                                      parsed ? "ok" : "unparsed",
                                      static_cast<unsigned>(request.worldState),
                                      static_cast<int>(request.activityBytes[0]),
                                      static_cast<int>(request.activityBytes[1]),
                                      static_cast<int>(request.activityBytes[2]),
                                      static_cast<int>(request.activitySelector));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         parsed ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
    if (parsed && request.hasWorldState) {
        state::activity::membership::note_client_writeback(request.worldState
                                                           == writeback::kInWorld);
    }
    return parsed
           && (!request.newItems
               || state::account::inventory::record_character_seen(*request.newItems,
                                                                   presentation));
}

/** @return True when a purchase names the seasonal artifact vendor, which is answered here. */
[[nodiscard]] bool names_artifact_vendor(const middleware::web_service::Message& message) noexcept {
    namespace purchase_codec = middleware::web_service::messages::opcode901;
    purchase_codec::Request purchase{};
    return purchase_codec::parse_request(message, purchase)
           && purchase.vendorIndex == kArtifactVendorIndex;
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
    std::array<char, kPurchaseLineCapacity> line{};
    const int length =
        parsed ? std::snprintf(line.data(),
                               line.size(),
                               "ev=ws901 stage=purchase result=refuse vendor=%d sale=%d present=%u",
                               static_cast<int>(purchase.vendorIndex),
                               static_cast<int>(purchase.saleIndex),
                               purchase.hasClock ? 1U : 0U)
               : std::snprintf(line.data(),
                               line.size(),
                               "ev=ws901 stage=purchase result=refuse reason=parse");
    if (length > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(length)});
    }
    middleware::web_service::StatusResponse status{};
    status.code = kRefusedStatus;
    // The trailing bool drives a local action effect on the client, so it stays clear.
    status.trailingBool = false;
    return middleware::web_service::encode_response(
        message,
        middleware::web_service::ResponseShape::statusPairWithBool,
        status,
        response,
        written);
}

/** Accepts one affordable, unlocked-tier artifact mod and reports the local purchase effect. */
[[nodiscard]] bool purchase_artifact_mod(const middleware::web_service::Message& message,
                                         std::span<std::byte> response,
                                         std::size_t& written,
                                         Outcome& outcome) noexcept {
    namespace purchase_codec = middleware::web_service::messages::opcode901;
    purchase_codec::Request purchase{};
    if (!purchase_codec::parse_request(message, purchase)
        || purchase.vendorIndex != kArtifactVendorIndex || purchase.saleIndex < 0
        || purchase.saleIndex
               >= static_cast<std::int16_t>(state::build_data::kArtifactSaleRowCapacity)) {
        return false;
    }
    const auto saleIndex = static_cast<std::uint16_t>(purchase.saleIndex);
    if (saleIndex == kArtifactResetSaleIndex) {
        state::ArtifactResetResult reset{};
        if (!state::reset_artifact(kArtifactResetGlimmerCost, reset)) {
            return false;
        }
        middleware::web_service::StatusResponse status{};
        status.trailingBool = true;
        const bool encoded = middleware::web_service::encode_response(
            message,
            middleware::web_service::ResponseShape::statusPairWithBool,
            status,
            response,
            written);
        outcome.hasArtifactReset = encoded;
        if (encoded) {
            outcome.artifactReset = reset;
        }
        return encoded;
    }
    auto* mutation = emplace_mutation<state::PendingArtifactPurchase>(outcome);
    if (mutation == nullptr || !state::prepare_artifact_mod_unlock(saleIndex, *mutation)) {
        clear_mutation(outcome);
        return false;
    }
    std::array<char, kPurchaseLineCapacity> line{};
    const int length = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=ws901 stage=artifact result=ok vendor=%d sale=%d",
                                     static_cast<int>(purchase.vendorIndex),
                                     static_cast<int>(purchase.saleIndex));
    if (length > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(length)});
    }
    middleware::web_service::StatusResponse status{};
    status.trailingBool = true;
    const bool encoded = middleware::web_service::encode_response(
        message,
        middleware::web_service::ResponseShape::statusPairWithBool,
        status,
        response,
        written);
    if (!encoded) {
        // The purchase was written when it was prepared, so a refused reply undoes it.
        (void)state::replace_artifact_mod_mask(mutation->afterMask, mutation->beforeMask);
        clear_mutation(outcome);
        return false;
    }
    return true;
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
    std::array<char, kEchoLineCapacity> line{};
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
 * Encodes the refusal reply for a request whose answer may name a resident the client dropped.
 * @param request Whole decrypted svc-10 body.
 * @param response Svc-11 response-body storage owned by the caller.
 * @param written Gets the encoded response-body size; zero when the opcode is not refused here.
 * @param refused Gets true when the opcode is one of the resident-dependent set.
 * @return False when neither the refusal nor the bare echo could be encoded.
 */
bool encode_resident_dependent_refusal(std::span<const std::byte> request,
                                       std::span<std::byte> response,
                                       std::size_t& written,
                                       bool& refused) noexcept {
    written = 0;
    refused = false;
    middleware::web_service::Message message;
    if (!middleware::web_service::parse_request(request, message)
        || !std::binary_search(
            kResidentDependentOpcodes.begin(), kResidentDependentOpcodes.end(), message.opcode)) {
        return true;
    }
    refused = true;
    middleware::web_service::ResponseShape shape{};
    resolve_response_shape(message.opcode, shape);
    middleware::web_service::StatusResponse status{};
    status.code = kRefusedStatus;
    return middleware::web_service::encode_response(message, shape, status, response, written)
           || encode_echo(message, response, written);
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
             Outcome& outcome,
             std::span<const state::account::inventory::PresentedItemRow> presentation) noexcept {
    written = 0;
    outcome = {};
    middleware::web_service::Message message;
    if (!middleware::web_service::parse_request(request, message)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws stage=parse result=fail");
        return false;
    }
    if (message.opcode == middleware::web_service::messages::opcode702::kOpcode) {
        // A write-back the server cannot read is still answered; no reply drops the connection.
        static_cast<void>(note_character_writeback(message, presentation));
    }
    if (message.opcode == middleware::web_service::messages::opcode205::kOpcode) {
        state::InvestmentState investment{};
        return (state::investment_snapshot(investment)
                && middleware::web_service::messages::opcode205::encode_response(
                    message, investment, next_family5_clock(), response, written))
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
                message, bootstrap, investment, next_family5_clock(), response, written)) {
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
        // The reply names a character the roster already holds, so no revision publishes it.
        middleware::web_service::StatusResponse status{};
        status.value = middleware::web_service::kNoFamily4Publication;
        return middleware::web_service::messages::opcode501::encode_response(
                   message, status, characterSoid, response, written)
               || encode_echo(message, response, written);
    }

    // The artifact vendor is answered here. Every other vendor purchase falls through to the
    // shared response-shape path, which runs the action and answers its status: an action that
    // prepared no mutation is answered with the refused code.
    if (message.opcode == middleware::web_service::messages::opcode901::kOpcode
        && names_artifact_vendor(message)) {
        return purchase_artifact_mod(message, response, written, outcome)
               || refuse_purchase(message, response, written)
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
    // Most actions fill the outcome only after preparing a whole transition. WS-701 also accepts
    // a valid no-op heartbeat, so that one success is tracked separately from mutation presence.
    bool dispatched = true;
    bool acceptedWithoutMutation = false;
    bool profileSetupRefused = false;
    if (message.opcode == middleware::web_service::messages::opcode1801::kOpcode) {
        claim_record(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode504::kOpcode) {
        select_character(message, outcome);
    } else if (message.opcode == kItemDismantleOpcode) {
        dismantle_item(message, outcome);
    } else if (message.opcode == kEquipOpcode) {
        mutate_equipment(message, false, outcome);
    } else if (message.opcode == kUnequipOpcode) {
        mutate_equipment(message, true, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode801::kOpcode) {
        mutate_subclass_selection(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode1821::kOpcode) {
        equip_title(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode903::kOpcode) {
        mutate_socket_plug(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode1901::kOpcode) {
        mutate_equipped_socket_plug(message, outcome);
    } else if (message.opcode == kItemStateOpcode) {
        mutate_item_state(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode701::kOpcode) {
        const state::SettingsUpdateDisposition disposition = mutate_settings(message, outcome);
        acceptedWithoutMutation = disposition == state::SettingsUpdateDisposition::acceptedNoChange;
        profileSetupRefused = outcome.profileSetupRefused;
    } else if (message.opcode == kItemAcquisitionOpcode) {
        acquire_item(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode2400::kOpcode) {
        claim_season_pass_reward(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode901::kOpcode) {
        purchase_item(message, outcome);
    } else if (message.opcode == middleware::web_service::messages::opcode904::kOpcode) {
        acquire_quest(message, outcome);
    } else {
        dispatched = false;
    }
    const bool prepared = outcome.hasSelectedCharacter || outcome.hasTitleEquip
                          || outcome.hasRecordClaim || has_mutation(outcome);

    middleware::web_service::ResponseShape shape{};
    resolve_response_shape(message.opcode, shape);
    middleware::web_service::StatusResponse status{};
    if (awaits_family4_version(message.opcode)) {
        // Nothing is published from here. A staged mutation re-encodes this with its own revision.
        status.value = middleware::web_service::kNoFamily4Publication;
    }
    if ((dispatched && !prepared && !acceptedWithoutMutation) || profileSetupRefused) {
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
