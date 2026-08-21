#include "web_service_actions.h"

#include <array>
#include <cstdio>
#include <limits>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../middleware/web_service/messages/opcode1801.h"
#include "../../state/record_claims/record_claims.h"
#include "../../middleware/web_service/messages/opcode1820.h"
#include "../../middleware/web_service/messages/opcode1901.h"
#include "../../middleware/web_service/messages/opcode402.h"
#include "../../middleware/web_service/messages/opcode403.h"
#include "../../middleware/web_service/messages/opcode406.h"
#include "../../middleware/web_service/messages/opcode504.h"
#include "../../middleware/web_service/messages/opcode801.h"
#include "../../middleware/web_service/messages/opcode903.h"
#include "../../state/account/account_state.h"
#include "../../state/build_data/runtime.h"
#include "../../state/runtime/runtime.h"

namespace sunrise::server::web_service {

namespace {

/** Socket kind the shader model occupies, which is the only kind a shader swap may target. */
constexpr std::uint8_t kEquippedShaderModelSocketKind = 0;
/** Index stored when no definition resolves. The catalog is u16-indexed, so this cannot be one. */
constexpr std::uint32_t kUnavailableDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();

} // namespace

/** Logs one exact correlated equipment response after its Queuez update is staged. */
void report_equip_response(const middleware::web_service::Message& message,
                           std::int32_t family4Version,
                           std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(line.data(),
                                     line.size(),
                                     "ev=equipment stage=response opcode=%u transaction=%u "
                                     "family_version=%d bytes=%zu hex=",
                                     static_cast<unsigned>(message.opcode),
                                     static_cast<unsigned>(message.transactionId),
                                     family4Version,
                                     response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(prefix);
    (void)core::log::append_hex(line, length, response);
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the final item-creation status pair and the exact Family-4 revision it promises. */
void report_item_acquisition_response(const middleware::web_service::Message& message,
                                      std::int32_t family4Version,
                                      std::uint64_t acquiredInstanceSoid,
                                      std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=acquire stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(acquiredInstanceSoid),
        response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(prefix);
    (void)core::log::append_hex(line, length, response);
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the final profile-stack status pair and the exact Family-4 account revision it promises. */
void report_profile_item_acquisition_response(const middleware::web_service::Message& message,
                                              std::int32_t family4Version,
                                              std::uint32_t definitionHash,
                                              std::int32_t quantity,
                                              std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=profile_acquire stage=response result=ok opcode=%u transaction=%u "
                      "family_version=%d definition_hash=0x%08X quantity=%d bytes=%zu hex=",
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      family4Version,
                      definitionHash,
                      quantity,
                      response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(prefix);
    (void)core::log::append_hex(line, length, response);
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the final dismantle status pair and the exact Family-4 revision it promises. */
void report_item_dismantle_response(const middleware::web_service::Message& message,
                                    std::int32_t family4Version,
                                    std::uint64_t dismantledInstanceSoid,
                                    std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=dismantle stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(dismantledInstanceSoid),
        response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(prefix);
    (void)core::log::append_hex(line, length, response);
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the exact opcode-903 status pair and the item-instance revision it promises. */
void report_socket_plug_response(const middleware::web_service::Message& message,
                                 std::int32_t family4Version,
                                 std::uint64_t targetInstanceSoid,
                                 std::uint8_t socketLane,
                                 std::uint16_t plugDefinitionIndex,
                                 std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix = std::snprintf(
        line.data(),
        line.size(),
        "ev=socket_plug stage=response result=ok opcode=%u transaction=%u family_version=%d "
        "instance=0x%llX lane=%u plug_definition=%u bytes=%zu hex=",
        static_cast<unsigned>(message.opcode),
        static_cast<unsigned>(message.transactionId),
        family4Version,
        static_cast<unsigned long long>(targetInstanceSoid),
        static_cast<unsigned>(socketLane),
        static_cast<unsigned>(plugDefinitionIndex),
        response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    std::size_t length = static_cast<std::size_t>(prefix);
    (void)core::log::append_hex(line, length, response);
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
}

/** Logs the exact opcode-801 status pair and subclass item revision it promises. */
void report_subclass_selection_response(const middleware::web_service::Message& message,
                                        std::int32_t family4Version,
                                        const state::PendingSubclassSelection& mutation,
                                        std::span<const std::byte> response) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int prefix =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=subclass_select stage=response result=ok opcode=%u transaction=%u "
                      "family_version=%d instance=0x%llX entry=%u bytes=%zu hex=",
                      static_cast<unsigned>(message.opcode),
                      static_cast<unsigned>(message.transactionId),
                      family4Version,
                      static_cast<unsigned long long>(mutation.subclassInstanceSoid),
                      static_cast<unsigned>(mutation.requestedEntry),
                      response.size());
    if (prefix <= 0 || static_cast<std::size_t>(prefix) >= line.size()) {
        return;
    }
    // Upper-case hex, which is the form the rest of the log lines use.
    constexpr char kHex[] = "0123456789ABCDEF";
    std::size_t length = static_cast<std::size_t>(prefix);
    for (const std::byte byte : response) {
        if (length + 2 >= line.size()) {
            break;
        }
        const unsigned value = std::to_integer<unsigned>(byte);
        line[length++] = kHex[(value >> 4U) & 0xFU];
        line[length++] = kHex[value & 0xFU];
    }
    core::log::write(core::log::Channel::server, core::log::Level::debug, {line.data(), length});
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
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
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
        std::array<char, 112> line{};
        const int count = std::snprintf(line.data(),
                                        line.size(),
                                        "ev=equipment stage=parse result=fail opcode=%u "
                                        "payload_bytes=%zu",
                                        static_cast<unsigned>(message.opcode),
                                        message.payload.size());
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    state::PendingEquipmentSwap mutation;
    const bool prepared = unequip
                              ? state::prepare_equipment_unequip(requestedInstanceSoid, mutation)
                              : state::prepare_equipment_swap(requestedInstanceSoid, mutation);
    if (!prepared) {
        std::array<char, 144> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=equipment stage=prepare result=fail opcode=%u action=%s requested=0x%llX",
            static_cast<unsigned>(message.opcode),
            unequip ? "unequip" : "equip",
            static_cast<unsigned long long>(requestedInstanceSoid));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }
    outcome.mutation = mutation;

    std::array<char, 224> line{};
    const int count =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=equipment stage=prepare result=ok opcode=%u action=%s character=0x%llX "
                      "previous=0x%llX requested=0x%llX native_slot=%u moved_items=%zu",
                      static_cast<unsigned>(message.opcode),
                      unequip ? "unequip" : "equip",
                      static_cast<unsigned long long>(mutation.characterSoid),
                      static_cast<unsigned long long>(mutation.previousInstanceSoid),
                      static_cast<unsigned long long>(mutation.requestedInstanceSoid),
                      static_cast<unsigned>(mutation.nativeEquipmentSlot),
                      mutation.movedItemCount);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Parses and prepares one exact selected-character opcode-801 subclass node selection. */
void mutate_subclass_selection(const middleware::web_service::Message& message,
                               Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode801::Request request{};
    if (!middleware::web_service::messages::opcode801::parse_request(message, request)) {
        std::array<char, 128> line{};
        const int count =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=ws801 stage=parse result=fail transaction=%u payload_bytes=%zu",
                          static_cast<unsigned>(message.transactionId),
                          message.payload.size());
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    state::PendingSubclassSelection mutation{};
    if (!state::prepare_subclass_selection(
            request.subclassInstanceSoid, request.socketEntry, mutation)) {
        std::array<char, 160> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws801 stage=prepare result=fail transaction=%u instance=0x%llX entry=%u",
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned long long>(request.subclassInstanceSoid),
            static_cast<unsigned>(request.socketEntry));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    outcome.mutation = mutation;
    std::array<char, 224> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws801 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "entry=%u socket_list=%u",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.subclassInstanceSoid),
        static_cast<unsigned>(mutation.requestedEntry),
        static_cast<unsigned>(mutation.socketEntryListIndex));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(count)});
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
        std::array<char, 192> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws903 stage=parse result=fail transaction=%u payload_bytes=%zu has_instance=%u "
            "instance=0x%llX has_target_definition=%u socket=%u has_plug_definition=%u",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            static_cast<unsigned>(request.hasInstance),
            static_cast<unsigned long long>(request.instanceSoid),
            static_cast<unsigned>(request.hasTargetDefinition),
            request.socketIndex,
            static_cast<unsigned>(request.hasPlugDefinition));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    state::PendingSocketPlug mutation{};
    if (!state::prepare_socket_plug(request.instanceSoid,
                                    static_cast<std::uint8_t>(request.socketIndex),
                                    request.plugDefinitionIndex,
                                    mutation)) {
        std::array<char, 192> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws903 stage=prepare result=fail transaction=%u instance=0x%llX lane=%u "
            "plug_definition=%u",
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned long long>(request.instanceSoid),
            request.socketIndex,
            static_cast<unsigned>(request.plugDefinitionIndex));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    outcome.mutation = mutation;
    std::array<char, 240> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws903 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "target_definition=%u target_bucket=%u lane=%u plug_definition=%u plug_bucket=%u "
        "equipped=%u item_index=%zu",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        static_cast<unsigned>(mutation.targetBucketId),
        static_cast<unsigned>(mutation.socketLane),
        static_cast<unsigned>(mutation.plugDefinitionIndex),
        static_cast<unsigned>(mutation.plugBucketId),
        static_cast<unsigned>(mutation.targetEquipped),
        mutation.itemIndex);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Parses and prepares one character-location opcode-1901 socket selection. */
void mutate_equipped_socket_plug(const middleware::web_service::Message& message,
                                 Outcome& outcome) noexcept {
    namespace opcode1901 = middleware::web_service::messages::opcode1901;
    opcode1901::Request request{};
    const bool parsed = opcode1901::parse_request(message, request);
    // A request prepares at most one State mutation, so a run naming several sockets cannot be
    // applied as the one transaction it has to be. It is understood and declined rather than
    // treated as malformed, and the reply now carries that refusal.
    const opcode1901::Replacement& replacement = request.replacements.front();
    if (!parsed || request.replacementCount != 1
        || replacement.modelSocketKind != kEquippedShaderModelSocketKind
        || replacement.auxiliary != 0
        || replacement.socketIndex >= state::account::inventory::kPlugCapacity
        || request.instanceIdentityToken == 0) {
        std::array<char, 256> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws1901 stage=parse result=fail transaction=%u payload_bytes=%zu replacements=%zu "
            "plug_definition=%u canonical_kind=%u model_kind=%u socket=%u auxiliary=0x%llX "
            "equipment_selector=%llu",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            request.replacementCount,
            static_cast<unsigned>(replacement.plugDefinitionIndex),
            static_cast<unsigned>(replacement.canonicalSocketKind),
            static_cast<unsigned>(replacement.modelSocketKind),
            replacement.socketIndex,
            static_cast<unsigned long long>(replacement.auxiliary),
            static_cast<unsigned long long>(request.equipmentSelector));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    const std::uint64_t identityToken = request.instanceIdentityToken;
    state::PendingSocketPlug mutation{};
    if (!state::prepare_character_selector_socket_plug(
            request.instanceIdentityToken,
            static_cast<std::uint8_t>(replacement.socketIndex),
            replacement.plugDefinitionIndex,
            mutation)) {
        std::array<char, 224> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws1901 stage=prepare result=fail transaction=%u equipment_selector=%llu "
            "identity_token=%llu lane=%u plug_definition=%u canonical_kind=%u model_kind=%u "
            "auxiliary=0x%llX",
            static_cast<unsigned>(message.transactionId),
            static_cast<unsigned long long>(request.equipmentSelector),
            static_cast<unsigned long long>(identityToken),
            replacement.socketIndex,
            static_cast<unsigned>(replacement.plugDefinitionIndex),
            static_cast<unsigned>(replacement.canonicalSocketKind),
            static_cast<unsigned>(replacement.modelSocketKind),
            static_cast<unsigned long long>(replacement.auxiliary));
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    outcome.mutation = mutation;
    std::array<char, 288> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1901 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "equipment_selector=%llu identity_token=%llu target_definition=%u target_bucket=%u "
        "lane=%u plug_definition=%u plug_bucket=%u canonical_kind=%u model_kind=%u "
        "auxiliary=0x%llX",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned long long>(request.equipmentSelector),
        static_cast<unsigned long long>(identityToken),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        static_cast<unsigned>(mutation.targetBucketId),
        static_cast<unsigned>(mutation.socketLane),
        static_cast<unsigned>(mutation.plugDefinitionIndex),
        static_cast<unsigned>(mutation.plugBucketId),
        static_cast<unsigned>(replacement.canonicalSocketKind),
        static_cast<unsigned>(replacement.modelSocketKind),
        static_cast<unsigned long long>(replacement.auxiliary));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Parses and prepares one complete accumulated item-state value from opcode 406. */
void mutate_item_state(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode406::Request request{};
    if (!middleware::web_service::messages::opcode406::parse_request(message, request)) {
        std::array<char, 224> line{};
        const int count = std::snprintf(
            line.data(),
            line.size(),
            "ev=ws406 stage=parse result=fail transaction=%u payload_bytes=%zu instance=0x%llX "
            "definition=%u flags=0x%X",
            static_cast<unsigned>(message.transactionId),
            message.payload.size(),
            static_cast<unsigned long long>(request.instanceSoid),
            static_cast<unsigned>(request.definitionIndex),
            request.flags);
        if (count > 0) {
            core::log::write(core::log::Channel::server,
                             core::log::Level::warn,
                             {line.data(), static_cast<std::size_t>(count)});
        }
        return;
    }

    const std::uint64_t instanceSoid = request.instanceSoid;
    const std::uint32_t flags = request.flags;
    state::PendingItemState mutation{};
    if (!state::prepare_item_state(instanceSoid, request.definitionIndex, flags, mutation)) {
        return;
    }
    outcome.mutation = mutation;
    std::array<char, 224> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws406 stage=prepare result=ok transaction=%u character=0x%llX instance=0x%llX "
        "definition=%u flags_before=0x%X flags_after=0x%X equipped=%u item_index=%zu",
        static_cast<unsigned>(message.transactionId),
        static_cast<unsigned long long>(mutation.characterSoid),
        static_cast<unsigned long long>(mutation.targetInstanceSoid),
        static_cast<unsigned>(mutation.targetDefinitionIndex),
        mutation.beforeFlags,
        mutation.afterFlags,
        mutation.targetEquipped ? 1U : 0U,
        mutation.itemIndex);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Records strict opcode-402 parsing, identity checks, and State preparation outcomes. */
void report_item_dismantle(const middleware::web_service::Message& message,
                           std::string_view result,
                           std::string_view reason,
                           std::uint64_t instanceSoid,
                           std::uint32_t definitionIndex,
                           std::uint32_t definitionHash,
                           std::uint32_t quantity) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws402 stage=prepare result=%.*s reason=%.*s transaction=%u payload_bytes=%zu "
        "instance=0x%llX definition_index=%u definition_hash=0x%08X quantity=%u",
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        static_cast<unsigned long long>(instanceSoid),
        definitionIndex,
        definitionHash,
        quantity);
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Prepares the exact fixed-width opcode-402 Character-inventory removal request. */
void dismantle_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode402::Request request{};
    if (!middleware::web_service::messages::opcode402::parse_request(message, request)) {
        report_item_dismantle(
            message, "fail", "payload_bits", request.instanceSoid, request.definitionIndex, 0, 0);
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
            message, "fail", "definition", instanceSoid, definitionIndex, 0, kSingleQuantity);
        return;
    }
    state::PendingItemDismantle mutation{};
    if (!state::prepare_item_dismantle(instanceSoid, mutation)) {
        report_item_dismantle(message,
                              "fail",
                              "state",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
    if (mutation.dismantledItem.definitionHash != definition.definitionHash
        || mutation.dismantledItem.quantity != static_cast<std::int32_t>(kSingleQuantity)) {
        report_item_dismantle(message,
                              "fail",
                              "identity",
                              instanceSoid,
                              definitionIndex,
                              definition.definitionHash,
                              kSingleQuantity);
        return;
    }
    outcome.mutation = mutation;
    report_item_dismantle(message,
                          "ok",
                          "ready",
                          instanceSoid,
                          definitionIndex,
                          definition.definitionHash,
                          kSingleQuantity);
}

/** Records opcode-1801 Triumphs claim requests and the record row each one names. */
void report_record_claim(const middleware::web_service::Message& message,
                         std::string_view result,
                         std::string_view reason,
                         std::uint32_t recordIndex,
                         std::uint32_t completionFlagIndex,
                         std::uint32_t scoreValue) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1801 stage=claim result=%.*s reason=%.*s transaction=%u payload_bytes=%zu "
        "record_index=%u completion_flag_index=%u score=%u total_score=%u claims=%zu",
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        recordIndex,
        completionFlagIndex,
        scoreValue,
        state::record_claims::total_score(),
        state::record_claims::count());
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Records strict opcode-1820 parsing, installed mapping, and State preparation outcomes. */
void report_item_acquisition(const middleware::web_service::Message& message,
                             std::string_view result,
                             std::string_view reason,
                             std::uint32_t collectibleIndex,
                             std::uint32_t itemDefinitionIndex,
                             std::uint32_t definitionHash,
                             std::uint64_t instanceSoid) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int count = std::snprintf(
        line.data(),
        line.size(),
        "ev=ws1820 stage=prepare result=%.*s reason=%.*s transaction=%u payload_bytes=%zu "
        "collectible_index=%u item_definition_index=%u definition_hash=0x%08X instance=0x%llX",
        static_cast<int>(result.size()),
        result.data(),
        static_cast<int>(reason.size()),
        reason.data(),
        static_cast<unsigned>(message.transactionId),
        message.payload.size(),
        collectibleIndex,
        itemDefinitionIndex,
        definitionHash,
        static_cast<unsigned long long>(instanceSoid));
    if (count > 0) {
        core::log::write(core::log::Channel::server,
                         result == "ok" ? core::log::Level::debug : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(count)});
    }
}

/** Prepares the exact three-byte opcode-1820 Collections item request. */
void acquire_item(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    middleware::web_service::messages::opcode1820::Request request{};
    if (!middleware::web_service::messages::opcode1820::parse_request(message, request)) {
        report_item_acquisition(message,
                                "fail",
                                "payload_bits",
                                kUnavailableDefinitionIndex,
                                kUnavailableDefinitionIndex,
                                0,
                                0);
        return;
    }
    const std::uint16_t collectibleIndex = request.collectibleIndex;
    std::uint16_t itemDefinitionIndex = 0;
    if (!state::build_data::find_collectible_item_definition_index(collectibleIndex,
                                                                   itemDefinitionIndex)) {
        report_item_acquisition(message,
                                "fail",
                                "collectible_definition",
                                collectibleIndex,
                                kUnavailableDefinitionIndex,
                                0,
                                0);
        return;
    }

    state::build_data::items::Definition definition{};
    if (!state::build_data::find_item_definition_index(itemDefinitionIndex, definition)) {
        report_item_acquisition(
            message, "fail", "item_definition", collectibleIndex, itemDefinitionIndex, 0, 0);
        return;
    }

    state::build_data::items::details::Definition detail{};
    state::build_data::inventory::buckets::Descriptor bucket{};
    if (!state::build_data::find_configured_item_detail(itemDefinitionIndex, detail)
        || detail.definitionIndex != itemDefinitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || !state::build_data::find_inventory_bucket_descriptor(detail.bucketId, bucket)) {
        report_item_acquisition(message,
                                "fail",
                                "item_detail_or_bucket",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }

    namespace bucket_domain = state::build_data::inventory::buckets;
    namespace detail_domain = state::build_data::items::details;
    if (bucket.arraySelector == bucket_domain::ArraySelector::profile) {
        if (detail.instancedDefinitionState != detail_domain::InstancedDefinitionState::stackable) {
            report_item_acquisition(message,
                                    "fail",
                                    "profile_item_instanced",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return;
        }
        state::PendingProfileItemAcquisition mutation{};
        if (!state::prepare_profile_item_acquisition(
                collectibleIndex, definition.definitionHash, mutation)) {
            report_item_acquisition(message,
                                    "fail",
                                    "profile_state",
                                    collectibleIndex,
                                    itemDefinitionIndex,
                                    definition.definitionHash,
                                    0);
            return;
        }
        outcome.mutation = mutation;
        report_item_acquisition(message,
                                "ok",
                                "profile_ready",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }
    if (bucket.arraySelector != bucket_domain::ArraySelector::character) {
        report_item_acquisition(message,
                                "fail",
                                "unsupported_inventory_array",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }

    state::PendingItemAcquisition mutation{};
    if (!state::prepare_item_acquisition(collectibleIndex, definition.definitionHash, mutation)) {
        report_item_acquisition(message,
                                "fail",
                                "state",
                                collectibleIndex,
                                itemDefinitionIndex,
                                definition.definitionHash,
                                0);
        return;
    }
    outcome.mutation = mutation;
    report_item_acquisition(message,
                            "ok",
                            "ready",
                            collectibleIndex,
                            itemDefinitionIndex,
                            definition.definitionHash,
                            mutation.acquiredInstanceSoid);
}

/** Decodes one opcode-1801 Triumphs claim and reports the record it names. */
void claim_record(const middleware::web_service::Message& message, Outcome& outcome) noexcept {
    // Deliberately no mutation. The shared reply path answers an action that prepares nothing with
    // the refusal status, so preparing a placeholder here would turn a silently-accepted claim into
    // an explicitly rejected one. Attach the transition here once claimed state is identified.
    namespace records = state::build_data::records;
    middleware::web_service::messages::opcode1801::Request request{};
    if (!middleware::web_service::messages::opcode1801::parse_request(message, request)) {
        report_record_claim(message, "fail", "payload_bits", 0, records::kUnavailableFlagIndex, 0);
        return;
    }
    records::Definition definition{};
    if (!state::build_data::find_record_definition(request.recordIndex, definition)) {
        report_record_claim(message,
                            "fail",
                            "record_definition",
                            request.recordIndex,
                            records::kUnavailableFlagIndex,
                            0);
        return;
    }
    if (definition.completionFlagIndex == records::kUnavailableFlagIndex) {
        // The record carries no completion flag, or its slot has no row in the account bank.
        report_record_claim(message,
                            "fail",
                            "no_completion_flag",
                            request.recordIndex,
                            records::kUnavailableFlagIndex,
                            definition.scoreValue);
        return;
    }
    if (!state::record_claims::claim(definition.completionFlagIndex, definition.scoreValue)) {
        report_record_claim(message,
                            "fail",
                            "flag_index_range",
                            request.recordIndex,
                            definition.completionFlagIndex,
                            definition.scoreValue);
        return;
    }
    // The claim is already in the store, so the account image only has to be sent again.
    outcome.hasRecordClaim = true;
    report_record_claim(message,
                        "ok",
                        "claimed",
                        request.recordIndex,
                        definition.completionFlagIndex,
                        definition.scoreValue);
}

} // namespace sunrise::server::web_service
