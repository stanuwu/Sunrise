#include "web_service_actions.h"

#include <array>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../core/runtime/server_clock.h"
#include "../../middleware/crypto/random_bytes.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/messages/opcode2002.h"
#include "../../middleware/web_service/messages/opcode402.h"
#include "../../middleware/web_service/messages/opcode403.h"
#include "../../middleware/web_service/messages/opcode406.h"
#include "../../middleware/web_service/messages/opcode501_request_codec.h"
#include "../../middleware/web_service/messages/opcode502.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode701/opcode701_codec.h"
#include "../../middleware/web_service/messages/opcode801.h"
#include "../../middleware/web_service/messages/opcode903.h"
#include "../../state/build_data/items/item_catalog.h"
#include "../../state/build_data/runtime.h"
#include "../../state/investment/store_internal.h"
#include "../../state/runtime/runtime.h"
#include "internal_actions.h"

namespace sunrise::server::web_service {

namespace {

/** Socket kind the shader model occupies, which is the only kind a shader swap may target. */
constexpr std::uint8_t kEquippedShaderModelSocketKind = 0;

} // namespace

/** Decodes and prepares one sparse account-settings writeback without publishing State. */
state::SettingsUpdateDisposition mutate_settings(const middleware::web_service::Message& message,
                                                 Outcome& outcome) noexcept {
    namespace opcode701 = middleware::web_service::messages::opcode701;

    opcode701::Request request{};
    if (!opcode701::parse_request(message, request)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws701 stage=prepare result=rejected reason=parse");
        return state::SettingsUpdateDisposition::rejected;
    }

    state::investment::store::Transaction writeback;
    if (!writeback.ready()) {
        return state::SettingsUpdateDisposition::rejected;
    }
    if (request.newItems && !state::account::inventory::record_profile_seen(*request.newItems)) {
        return state::SettingsUpdateDisposition::rejected;
    }
    // The completion marker rides the same body; the caller's status path reports the result.
    if (request.profileSetupCompleted) {
        if (state::complete_profile_setup()) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::info,
                             "ev=ws701 stage=profile_setup result=complete");
        } else {
            outcome.profileSetupRefused = true;
        }
    }

    state::PendingSettingsUpdate mutation{};
    const state::SettingsUpdateDisposition disposition =
        state::prepare_settings_update(request.settings, mutation);
    if (disposition == state::SettingsUpdateDisposition::preparedMutation) {
        if (emplace_mutation<state::PendingSettingsUpdate>(outcome, mutation) == nullptr) {
            return state::SettingsUpdateDisposition::rejected;
        }
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         "ev=ws701 stage=prepare result=ready");
        if (!writeback.commit()) {
            clear_mutation(outcome);
            return state::SettingsUpdateDisposition::rejected;
        }
        return disposition;
    }
    if (disposition == state::SettingsUpdateDisposition::acceptedNoChange) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         "ev=ws701 stage=prepare result=no_change");
        if (!writeback.commit()) {
            clear_mutation(outcome);
            return state::SettingsUpdateDisposition::rejected;
        }
        return disposition;
    }

    core::log::write(core::log::Channel::server,
                     core::log::Level::warn,
                     "ev=ws701 stage=prepare result=rejected reason=validation");
    return state::SettingsUpdateDisposition::rejected;
}

/**
 * Records the player's character pick, which arrives nowhere else.
 * A bad or unknown id leaves the selection alone. The reply is the status pair either way. The
 * Family-4 object move follows this call, and the family-zero pair after it.
 * @param message Parsed select-character request.
 * @param outcome Gets the picked key, whether it changed character, and whether it earned perks.
 */
void select_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode504::Request picked;
    if (!middleware::web_service::messages::opcode504::parse_request(message, picked)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws504 stage=parse result=fail");
        return;
    }
    if (!state::set_selected_character(
            picked.characterSoid, outcome.selectionMoved, outcome.selectionEarnedPerks)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws504 stage=select result=unknown");
        return;
    }
    outcome.hasSelectedCharacter = true;
    outcome.selectedCharacterSoid = picked.characterSoid;
}

/**
 * Saves the character an opcode-501 request describes, with its starter set and subclasses.
 * Both writes share one save transaction, so a refusal leaves no trace.
 * @param message Parsed create-character request.
 * @param outcome Gets the roster change and the new id once both writes commit.
 */
void create_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace opcode501 = middleware::web_service::messages::opcode501;
    opcode501::Request request{};
    if (!opcode501::parse_request(message, request)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws501 stage=parse result=fail");
        return;
    }
    state::investment::store::Transaction transaction;
    std::uint64_t characterSoid = 0;
    if (!transaction.ready()
        || !state::create_character(request.characterClass,
                                    request.gender,
                                    request.race,
                                    request.appearanceHeader,
                                    characterSoid)
        || !state::ensure_character_subclasses() || !transaction.commit()) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws501 stage=create result=fail");
        return;
    }
    outcome.hasRosterChange = true;
    outcome.createdCharacterSoid = characterSoid;
}

/**
 * Removes the character an opcode-502 request names.
 * @param message Parsed delete-character request.
 * @param outcome Gets the roster change once the removal commits, and whether it dropped the
 *        selection, as a pick reports a moved one.
 */
void delete_character(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    namespace opcode502 = middleware::web_service::messages::opcode502;
    opcode502::Request request{};
    if (!opcode502::parse_request(message, request)
        || !state::delete_character(request.characterSoid, outcome.selectionMoved)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws502 stage=delete result=fail");
        return;
    }
    outcome.hasRosterChange = true;
}

/** Reads the shared opcode-403/404 SOID descriptor through its codec. */
[[nodiscard]] bool parse_equipment_instance(const middleware::web_service::Message& message,
                                            std::uint64_t& instanceSoid) noexcept {
    middleware::web_service::messages::opcode403::Request request{};
    const bool parsed =
        middleware::web_service::messages::opcode403::parse_request(message, request);
    instanceSoid = request.instanceSoid;
    return parsed;
}

/** Prepares one opcode-403/404 equipment mutation without publishing State early. */
void mutate_equipment(const middleware::web_service::Message& message,
                      bool unequip,
                      Outcome& outcome) noexcept {
    std::uint64_t requestedInstanceSoid = 0;
    if (!parse_equipment_instance(message, requestedInstanceSoid)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=equipment stage=parse result=fail");
        return;
    }

    auto* mutation = emplace_mutation<state::PendingEquipmentSwap>(outcome);
    if (mutation == nullptr) {
        return;
    }
    const bool prepared = unequip
                              ? state::prepare_equipment_unequip(requestedInstanceSoid, *mutation)
                              : state::prepare_equipment_swap(requestedInstanceSoid, *mutation);
    if (!prepared) {
        clear_mutation(outcome);
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=equipment stage=prepare result=fail");
        return;
    }
}

/** Parses and prepares one exact selected-character opcode-801 subclass node selection. */
void mutate_subclass_selection(const middleware::web_service::Message& message,
                               Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode801::Request request{};
    if (!middleware::web_service::messages::opcode801::parse_request(message, request)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws801 stage=parse result=fail");
        return;
    }

    auto* mutation = emplace_mutation<state::PendingSubclassSelection>(outcome);
    if (mutation == nullptr) {
        return;
    }
    if (!state::prepare_subclass_selection(
            request.subclassInstanceSoid, request.socketEntry, *mutation)) {
        clear_mutation(outcome);
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws801 stage=prepare result=fail");
        return;
    }
}

/** Parses and prepares one exact selected-character opcode-903 socket selection. */
void mutate_socket_plug(const middleware::web_service::Message& message,
                        Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode903::Request request{};
    if (!middleware::web_service::messages::opcode903::parse_request(message, request)
        || !request.hasInstance || request.instanceSoid == 0 || request.hasTargetDefinition
        || !request.hasPlugDefinition
        || request.socketIndex >= state::account::inventory::kPlugCapacity) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws903 stage=parse result=fail");
        return;
    }

    auto* mutation = emplace_mutation<state::PendingSocketPlug>(outcome);
    if (mutation == nullptr) {
        return;
    }
    if (!state::prepare_socket_plug(request.instanceSoid,
                                    static_cast<std::uint8_t>(request.socketIndex),
                                    request.plugDefinitionIndex,
                                    core::runtime::server_clock_seconds(),
                                    *mutation)) {
        clear_mutation(outcome);
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws903 stage=prepare result=fail");
        return;
    }
}

/** Parses and prepares one character-location opcode-1901 socket selection. */
void mutate_equipped_socket_plug(const middleware::web_service::Message& message,
                                 Outcome& outcome) noexcept {
    namespace opcode1901 = middleware::web_service::messages::opcode1901;
    opcode1901::Request request{};
    const bool parsed = opcode1901::parse_request(message, request);
    // One transaction can stage one socket mutation, so understood batch requests are refused.
    const opcode1901::Replacement& replacement = request.replacements.front();
    if (!parsed || request.replacementCount != 1
        || replacement.modelSocketKind != kEquippedShaderModelSocketKind
        || replacement.socketIndex >= state::account::inventory::kPlugCapacity) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws1901 stage=parse result=fail");
        return;
    }

    auto* mutation = emplace_mutation<state::PendingSocketPlug>(outcome);
    if (mutation == nullptr) {
        return;
    }
    if (!state::prepare_equipped_socket_plug(request.targetInstanceSoid,
                                             static_cast<std::uint8_t>(replacement.socketIndex),
                                             replacement.plugDefinitionIndex,
                                             core::runtime::server_clock_seconds(),
                                             *mutation)) {
        clear_mutation(outcome);
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=ws1901 stage=prepare result=fail");
        return;
    }
}

/** Parses and prepares one complete accumulated item-state value from opcode 406. */
void mutate_item_state(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode406::Request request{};
    if (!middleware::web_service::messages::opcode406::parse_request(message, request)) {
        core::log::write(
            core::log::Channel::server, core::log::Level::warn, "ev=ws406 stage=parse result=fail");
        return;
    }

    auto* mutation = emplace_mutation<state::PendingItemState>(outcome);
    if (mutation == nullptr) {
        return;
    }
    if (!state::prepare_item_state(
            request.instanceSoid, request.definitionIndex, request.flags, *mutation)) {
        clear_mutation(outcome);
        return;
    }
}

/** Reports an opcode-402 validation failure. */
void report_item_dismantle(const middleware::web_service::Message& message,
                           std::string_view reason,
                           std::uint64_t instanceSoid,
                           std::uint32_t definitionIndex,
                           std::uint32_t definitionHash,
                           std::uint32_t quantity) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws402 stage=prepare result=fail reason=%.*s transaction=%u payload_bytes=%zu "
        "instance=0x%llX definition_index=%u definition_hash=0x%08X quantity=%u",
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        static_cast<unsigned long long>(instanceSoid),
        definitionIndex,
        definitionHash,
        quantity);
    write_warning(line, count);
}

/**
 * Redeems a completed pursuit through the shared reward grant. The seed is drawn in this layer.
 * @return True when the pursuit is redeemed or its refusal is reported, which settles the request.
 *         False when it is not redeemable, so the request removes it as an ordinary item.
 */
[[nodiscard]] bool redeem_pursuit(const middleware::web_service::Message& message,
                                  std::uint64_t instanceSoid,
                                  const state::build_data::items::Definition& definition,
                                  Outcome& outcome) noexcept {
    auto* grant = emplace_mutation<state::PendingRecordRewardGrant>(outcome);
    std::uint64_t seed = 0;
    const char* reason = grant == nullptr ? "storage" : "random_source";
    auto result = state::PursuitRedemption::refused;
    if (grant != nullptr && middleware::crypto::random::draw_seed(seed)) {
        result = state::prepare_pursuit_redemption(instanceSoid,
                                                   definition.definitionHash,
                                                   core::runtime::server_clock_seconds(),
                                                   seed,
                                                   *grant,
                                                   &reason);
    }
    if (result == state::PursuitRedemption::prepared) {
        return true;
    }
    clear_mutation(outcome);
    if (result == state::PursuitRedemption::notRedeemable) {
        return false;
    }
    report_item_dismantle(message,
                          reason != nullptr ? reason : "pursuit_redemption",
                          instanceSoid,
                          definition.definitionIndex,
                          definition.definitionHash,
                          middleware::web_service::messages::opcode402::kSingleQuantity);
    return true;
}

/**
 * Prepares the exact fixed-width opcode-402 removal of an instance or of a stack's discard. A
 * completed pursuit is redeemed instead.
 */
void dismantle_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode402::Request request{};
    if (!middleware::web_service::messages::opcode402::parse_request(message, request)) {
        report_item_dismantle(
            message, "payload_bits", request.instanceSoid, request.definitionIndex, 0, 0);
        return;
    }
    const std::uint64_t instanceSoid = request.instanceSoid;
    const std::uint16_t definitionIndex = request.definitionIndex;
    // The codec owns the value; this alias keeps the dismantle checks below readable.
    constexpr std::uint32_t kSingleQuantity =
        middleware::web_service::messages::opcode402::kSingleQuantity;

    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(definitionIndex, definition)) {
        report_item_dismantle(
            message, "definition", instanceSoid, definitionIndex, 0, kSingleQuantity);
        return;
    }
    state::build_data::pursuits::Definition contract{};
    if (instanceSoid != 0 && state::build_data::find_pursuit_definition(definitionIndex, contract)
        && redeem_pursuit(message, instanceSoid, definition, outcome)) {
        return;
    }
    auto* mutation = emplace_mutation<state::PendingItemDismantle>(outcome);
    if (mutation == nullptr) {
        report_item_dismantle(message,
                              "storage",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
    if (instanceSoid == 0) {
        if (!state::prepare_stack_discard(definition.definitionHash, request.quantity, *mutation)) {
            clear_mutation(outcome);
            report_item_dismantle(message,
                                  "state",
                                  instanceSoid,
                                  definitionIndex,
                                  definition.definitionHash,
                                  static_cast<std::uint32_t>(request.quantity));
        }
        return;
    }
    if (!state::prepare_item_dismantle(instanceSoid, *mutation)) {
        clear_mutation(outcome);
        report_item_dismantle(message,
                              "state",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
    if (mutation->dismantledItem.definitionHash != definition.definitionHash
        || mutation->dismantledItem.quantity != static_cast<std::int32_t>(kSingleQuantity)) {
        clear_mutation(outcome);
        report_item_dismantle(message,
                              "identity",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
}

/** Opens one held engram through the shared reward grant. The seed is drawn in this layer. */
void open_stored_wrapper(const middleware::web_service::Message& message,
                         Outcome& outcome) noexcept {
    namespace opcode2002 = middleware::web_service::messages::opcode2002;
    opcode2002::Request request{};
    const char* reason = "payload_bits";
    if (opcode2002::parse_request(message, request)) {
        auto* grant = emplace_mutation<state::PendingRecordRewardGrant>(outcome);
        std::uint64_t seed = 0;
        reason = grant == nullptr ? "storage" : "random_source";
        if (grant != nullptr && middleware::crypto::random::draw_seed(seed)
            && state::prepare_wrapper_opening(request.instanceSoid,
                                              seed,
                                              core::runtime::server_clock_seconds(),
                                              *grant,
                                              &reason)
                   == state::RewardPreparation::prepared) {
            return;
        }
        clear_mutation(outcome);
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(line.data(),
                                    line.size(),
                                    "ev=ws2002 stage=prepare result=fail reason=%s instance=0x%llX",
                                    reason != nullptr ? reason : "wrapper_opening",
                                    static_cast<unsigned long long>(request.instanceSoid));
    write_warning(line, count);
}

} // namespace sunrise::server::web_service
