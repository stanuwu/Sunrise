#include "studio_investment_payloads.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>

#include "../../state/account/account_state.h"
#include "../../state/account/inventory/occupied_bucket.h"
#include "../../state/build_data/runtime.h"
#include "../../state/runtime/pursuit_snapshot_runtime.h"
#include "../../state/runtime/runtime.h"
#include "../../state/runtime/season_pass_snapshot_runtime.h"
#include "../../state/unlocks/definition.h"
#include "../../state/unlocks/unlocks_expression.h"

namespace sunrise::server::studio {
namespace {

namespace wire = middleware::studio::investment_wire;

// The wire header declares its own capacities, and each one must equal the State capacity that
// owns it.
static_assert(wire::kProfileRowCapacity == state::account::inventory::kProfileItemCapacity);
static_assert(wire::kCharacterItemRecordCapacity
              == state::account::inventory::kCharacterItemCapacity
                     + state::account::inventory::kEquipmentSlotCount);
static_assert(wire::kCharacterStackRecordCapacity
              == state::account::inventory::kCharacterStackCapacity);
static_assert(wire::kBucketRecordCapacity
              == state::build_data::inventory::buckets::kDescriptorCapacity);
static_assert(wire::kNoBucket == state::build_data::inventory::buckets::kUnavailableBucketId
              && wire::kNoBucket == state::build_data::items::kUnresolvedBucketId);
static_assert(wire::kCharacterRecordCapacity == state::kCharacterCapacity);
static_assert(wire::kSeasonPassRowCapacity == state::build_data::season_pass::kRewardCapacity);
static_assert(wire::kHeldPursuitCapacity == state::account::inventory::kCharacterItemCapacity);
static_assert(wire::kPursuitObjectiveCapacity == state::build_data::pursuits::kObjectiveCapacity);
// A State equipment slot index never reads as unequipped.
static_assert(state::account::inventory::kEquipmentSlotCount < wire::kUnequipped);
// A source's 16-bit index addresses every row of each bank it can name.
static_assert((std::max)({state::unlocks::kAccountFlagCapacity,
                          state::unlocks::kProfileFlagCapacity,
                          state::unlocks::kCharacterObjectFlagCapacity,
                          state::unlocks::kObjectiveValueCapacity,
                          state::unlocks::kCharacterObjectValueCapacity})
              <= (std::numeric_limits<decltype(wire::UnlockSource::index)>::max)());

/** Copies one finished record to the front of the payload and binds the answer to context. */
template <typename Record>
[[nodiscard]] InvestmentAnswer
deliver(const Record& record, const wire::Context& context, InvestmentPayload payload) noexcept {
    static_assert(sizeof(Record) <= wire::kResponsePayloadBytes);
    std::memcpy(payload.data(), &record, sizeof(Record));
    return {.status = wire::Status::ok,
            .context = context,
            .payloadBytes = static_cast<std::uint32_t>(sizeof(Record))};
}

/** @return The wire identity block of an operation target. */
[[nodiscard]] wire::Context wire_context(const bap::InvestmentTarget& target) noexcept {
    return {target.accountSoid, target.characterSoid, target.contentHash};
}

/** @return A readiness bit for each catalog the editor's views and operation checks read. */
[[nodiscard]] std::uint32_t catalog_readiness() noexcept {
    std::uint32_t readiness = 0;
    readiness |= state::build_data::item_definitions_ready() ? wire::readyItems : 0U;
    readiness |= state::build_data::configured_item_details_ready() ? wire::readyItemDetails : 0U;
    readiness |= state::build_data::inventory_bucket_descriptors_ready() ? wire::readyBuckets : 0U;
    readiness |= state::build_data::progression_definitions_ready() ? wire::readyProgressions : 0U;
    readiness |= state::build_data::season_pass_ready() ? wire::readySeasonPass : 0U;
    readiness |= state::build_data::reward_definitions_ready() ? wire::readyRewards : 0U;
    readiness |= state::build_data::objective_definitions_ready() ? wire::readyObjectives : 0U;
    readiness |= state::build_data::pursuit_definitions_ready() ? wire::readyPursuits : 0U;
    return readiness;
}

/** @return The bucket a saved row occupies under its placement, or kNoBucket if unknown. */
[[nodiscard]] std::uint8_t bucket_of(std::uint32_t definitionHash,
                                     state::account::inventory::ItemPlacement placement) noexcept {
    state::build_data::items::Definition definition{};
    return state::build_data::find_item_definition_hash(definitionHash, definition)
               ? state::account::inventory::occupied_bucket_id(definition, placement)
               : wire::kNoBucket;
}

/** Copies every bucket the installed build defines. The unavailable id never has a descriptor. */
void append_buckets(wire::InventorySnapshot& snapshot) noexcept {
    std::size_t count = 0;
    for (std::size_t id = 0; id < wire::kBucketRecordCapacity; ++id) {
        state::build_data::inventory::buckets::Descriptor descriptor{};
        if (!state::build_data::find_inventory_bucket_descriptor(static_cast<std::uint8_t>(id),
                                                                 descriptor)) {
            continue;
        }
        wire::Bucket& bucket = snapshot.buckets[count++];
        bucket.bucketId = descriptor.bucketId;
        bucket.equipmentSlot = descriptor.equipmentSlot;
        bucket.slotCount = descriptor.slotCount;
    }
    snapshot.bucketCount = static_cast<std::uint16_t>(count);
}

/** Copies the account-wide rows in saved order. */
void append_profile(const state::AccountState& account,
                    wire::InventorySnapshot& snapshot) noexcept {
    const std::size_t count = (std::min)(account.profileItemCount, snapshot.profile.size());
    for (std::size_t index = 0; index < count; ++index) {
        const state::account::inventory::ProfileItem& item = account.profileItems[index];
        wire::ProfileRow& row = snapshot.profile[index];
        row.instanceSoid = item.instanceSoid;
        row.definitionHash = item.definitionHash;
        row.quantity = item.quantity;
        row.mutationSerial = item.mutationSerial;
        row.bucketId =
            bucket_of(item.definitionHash, state::account::inventory::ItemPlacement::authored);
    }
    snapshot.profileCount = static_cast<std::uint16_t>(count);
}

/** @param equipmentSlot The State slot the item is equipped in, or kUnequipped. */
[[nodiscard]] wire::CharacterItem character_item(const state::account::inventory::Item& item,
                                                 std::uint8_t equipmentSlot) noexcept {
    wire::CharacterItem record{};
    record.instanceSoid = item.instanceSoid;
    record.definitionHash = item.definitionHash;
    record.quantity = item.quantity;
    record.mutationSerial = item.mutationSerial;
    record.bucketId = bucket_of(item.definitionHash, item.placement);
    record.equipmentSlot = equipmentSlot;
    return record;
}

/** Equipped items come first, in slot order, then the unequipped rows in saved order. */
void append_items(const state::CharacterState& character,
                  wire::InventorySnapshot& snapshot) noexcept {
    std::size_t count = 0;
    for (std::size_t slot = 0; slot < character.equipment.slots.size(); ++slot) {
        if (character.equipment.slots[slot].has_value()) {
            snapshot.items[count++] =
                character_item(*character.equipment.slots[slot], static_cast<std::uint8_t>(slot));
        }
    }
    const std::size_t unequipped =
        (std::min)(character.inventory.count, character.inventory.values.size());
    for (std::size_t index = 0; index < unequipped; ++index) {
        snapshot.items[count++] =
            character_item(character.inventory.values[index], wire::kUnequipped);
    }
    snapshot.itemCount = static_cast<std::uint16_t>(count);
}

/** Copies the selected character's non-instanced stacks in saved order. */
void append_stacks(const state::CharacterState& character,
                   wire::InventorySnapshot& snapshot) noexcept {
    const std::size_t count = (std::min)(character.stacks.count, character.stacks.values.size());
    for (std::size_t index = 0; index < count; ++index) {
        const state::account::inventory::CharacterStack& stack = character.stacks.values[index];
        wire::CharacterStack& record = snapshot.stacks[index];
        record.definitionHash = stack.definitionHash;
        record.quantity = stack.quantity;
        record.mutationSerial = stack.mutationSerial;
        record.bucketId = bucket_of(stack.definitionHash, stack.placement);
    }
    snapshot.stackCount = static_cast<std::uint16_t>(count);
}

/** @return One pass row, whose index is its position in the snapshot. */
[[nodiscard]] wire::SeasonPassRow season_pass_row(const state::SeasonPassRowStatus& row) noexcept {
    std::uint32_t flags = 0;
    flags |= row.available ? wire::rowAvailable : 0U;
    flags |= row.claimed ? wire::rowClaimed : 0U;
    flags |= row.acquired ? wire::rowAcquired : 0U;
    flags |= row.automatic ? wire::rowAutomatic : 0U;
    flags |= row.rankMet ? wire::rowRankMet : 0U;
    flags |= row.conditionKnown ? wire::rowConditionKnown : 0U;
    flags |= row.conditionMet ? wire::rowConditionMet : 0U;
    wire::SeasonPassRow record{};
    record.itemHash = row.itemHash;
    record.requiredRank = row.requiredRank;
    record.flags = static_cast<std::uint16_t>(flags);
    return record;
}

/** @return The wire label for a pursuit snapshot refusal, reusing State's labels where it can. */
[[nodiscard]] const char* pursuit_refusal_label(state::PursuitSnapshotRefusal refusal) noexcept {
    switch (refusal) {
    case state::PursuitSnapshotRefusal::catalogUnavailable:
        return "not_ready";
    case state::PursuitSnapshotRefusal::catalogUnsupported:
        return "pursuits_unsupported";
    case state::PursuitSnapshotRefusal::accountUnavailable:
        return "account_state";
    case state::PursuitSnapshotRefusal::characterUnavailable:
        return "selected_character";
    case state::PursuitSnapshotRefusal::none:
    case state::PursuitSnapshotRefusal::storage:
        break;
    }
    return "transaction";
}

/** @return One declared objective with its evaluated value and target. */
[[nodiscard]] wire::PursuitObjective
pursuit_objective(const state::PursuitObjectiveStatus& objective) noexcept {
    std::uint32_t flags = 0;
    flags |= objective.known ? wire::objectiveKnown : 0U;
    flags |= objective.complete ? wire::objectiveComplete : 0U;
    flags |= objective.skipped ? wire::objectiveSkipped : 0U;
    wire::PursuitObjective record{};
    record.objectiveHash = objective.hash;
    record.value = objective.value;
    record.target = objective.target;
    record.flags = static_cast<std::uint8_t>(flags);
    return record;
}

/** @return One held pursuit with its declared objectives. */
[[nodiscard]] wire::HeldPursuit held_pursuit(const state::HeldPursuitStatus& held) noexcept {
    const state::PursuitCompletionStatus& completion = held.completion;
    std::uint32_t flags = 0;
    flags |= held.lifetimeSeconds > 0 ? wire::pursuitBounty : 0U;
    flags |= completion.objectivesKnown ? wire::pursuitObjectivesKnown : 0U;
    flags |= completion.objectivesComplete ? wire::pursuitObjectivesComplete : 0U;
    wire::HeldPursuit record{};
    record.instanceSoid = held.instanceSoid;
    record.itemHash = held.definitionHash;
    record.mutationSerial = held.mutationSerial;
    record.itemFlags = held.flags;
    record.flags = static_cast<std::uint8_t>(flags);
    const std::size_t count =
        (std::min)(static_cast<std::size_t>(completion.objectiveCount), record.objectives.size());
    record.objectiveCount = static_cast<std::uint8_t>(count);
    for (std::size_t index = 0; index < count; ++index) {
        record.objectives[index] = pursuit_objective(completion.objectives[index]);
    }
    return record;
}

/** @return The wire wait. An in-game presentation has no wire wait, so it maps to none. */
[[nodiscard]] wire::Wait receipt_wait(bap::InvestmentWait wait) noexcept {
    switch (wait) {
    case bap::InvestmentWait::publicationPeer:
        return wire::Wait::publicationPeer;
    case bap::InvestmentWait::accountResync:
        return wire::Wait::accountResync;
    case bap::InvestmentWait::catalog:
        return wire::Wait::catalog;
    case bap::InvestmentWait::none:
        break;
    }
    return wire::Wait::none;
}

/**
 * A wire deferral must name its wait, and the wire has none for an in-game presentation the peer
 * is still showing. That operation runs on one of the peer's next polls, so it reads as queued.
 */
[[nodiscard]] wire::ReceiptState receipt_state(bap::InvestmentReceiptState state,
                                               bap::InvestmentWait wait) noexcept {
    switch (state) {
    case bap::InvestmentReceiptState::queued:
        return wire::ReceiptState::queued;
    case bap::InvestmentReceiptState::deferred:
        return wait == bap::InvestmentWait::none ? wire::ReceiptState::queued
                                                 : wire::ReceiptState::deferred;
    case bap::InvestmentReceiptState::refused:
        return wire::ReceiptState::refused;
    case bap::InvestmentReceiptState::published:
        return wire::ReceiptState::published;
    case bap::InvestmentReceiptState::committedUnpublished:
        return wire::ReceiptState::committedUnpublished;
    }
    return wire::ReceiptState::unknown;
}

/** @return False for a kind the wire does not define. */
[[nodiscard]] bool unlock_opcode(wire::UnlockKind kind, state::unlocks::Opcode& output) noexcept {
    switch (kind) {
    case wire::UnlockKind::flag:
        output = state::unlocks::Opcode::flag;
        return true;
    case wire::UnlockKind::value:
        output = state::unlocks::Opcode::loadValue;
        return true;
    }
    return false;
}

/** @return False for a bank the wire does not define. */
[[nodiscard]] bool unlock_bank(wire::UnlockBank bank, state::unlocks::Bank& output) noexcept {
    switch (bank) {
    case wire::UnlockBank::account:
        output = state::unlocks::Bank::account;
        return true;
    case wire::UnlockBank::profile:
        output = state::unlocks::Bank::profile;
        return true;
    case wire::UnlockBank::character:
        output = state::unlocks::Bank::character;
        return true;
    }
    return false;
}

/** @return False for an undefined kind or bank, or for a value in the flag-only profile bank. */
[[nodiscard]] bool unlock_instruction(const wire::UnlockSource& source,
                                      state::unlocks::Instruction& output) noexcept {
    const bool profileValue =
        source.kind == wire::UnlockKind::value && source.bank == wire::UnlockBank::profile;
    output.operand = source.index;
    return !profileValue && unlock_opcode(source.kind, output.opcode)
           && unlock_bank(source.bank, output.bank);
}

/** @return True for an all-zero source, which every slot past the request's count must be. */
[[nodiscard]] bool unlock_source_empty(const wire::UnlockSource& source) noexcept {
    return source.kind == wire::UnlockKind{} && source.bank == wire::UnlockBank{}
           && source.index == 0;
}

/**
 * Decodes the counted sources. An index outside its bank passes through, and State reads it as
 * unknown.
 * @return False for a count, reserved field or source the request may not carry.
 */
[[nodiscard]] bool decode_unlock_sources(
    const wire::UnlockValuesRequest& request,
    std::span<state::unlocks::Instruction, wire::kUnlockReadCapacity> output) noexcept {
    if (request.count == 0 || request.count > wire::kUnlockReadCapacity || request.reserved != 0) {
        return false;
    }
    const auto unused = std::span(request.sources).subspan(request.count);
    if (!std::all_of(unused.begin(), unused.end(), unlock_source_empty)) {
        return false;
    }
    for (std::size_t index = 0; index < request.count; ++index) {
        if (!unlock_instruction(request.sources[index], output[index])) {
            return false;
        }
    }
    return true;
}

} // namespace

/** @return An invalid-request answer with one static label and no context or payload. */
InvestmentAnswer invalid(const char* reason) noexcept {
    return {.status = wire::Status::invalid, .reason = reason};
}

/** @return A refusal with one static label and no context or payload. */
InvestmentAnswer refused(const char* reason) noexcept {
    return {.status = wire::Status::refused, .reason = reason};
}

/** An unreadable account leaves the context empty and lists no characters. */
InvestmentAnswer encode_hello(InvestmentPayload payload) noexcept {
    bap::InvestmentContext live{};
    static_cast<void>(bap::investment_context(live));
    wire::Hello hello{};
    hello.readiness = catalog_readiness();
    const std::size_t count = (std::min)(live.account.characterCount, hello.characters.size());
    hello.characterCount = static_cast<std::uint8_t>(count);
    hello.peerOwnsAccount = static_cast<std::uint8_t>(live.peerOwnsAccount);
    for (std::size_t index = 0; index < count; ++index) {
        const state::CharacterIdentity& character = live.account.characters[index];
        wire::CharacterEntry& entry = hello.characters[index];
        entry.soid = character.soid;
        entry.characterClass = static_cast<std::uint8_t>(character.characterClass);
        entry.selected = static_cast<std::uint8_t>(character.selected);
    }
    return deliver(hello, wire_context(live.live), payload);
}

/** The account copy and the wire record are large, so both live on the heap for one request. */
InvestmentAnswer encode_inventory_snapshot(InvestmentPayload payload) noexcept {
    const std::unique_ptr<state::AccountState> account(
        new (std::nothrow) state::AccountState(state::bound_account_snapshot()));
    const std::unique_ptr<wire::InventorySnapshot> snapshot(new (std::nothrow)
                                                                wire::InventorySnapshot{});
    if (account == nullptr || snapshot == nullptr) {
        return refused("allocation");
    }
    if (!state::account::valid(*account)) {
        return refused("account_state");
    }
    const state::CharacterState* character = state::account::selected_character(*account);
    if (character == nullptr) {
        return refused("selected_character");
    }
    append_buckets(*snapshot);
    append_profile(*account, *snapshot);
    append_items(*character, *snapshot);
    append_stacks(*character, *snapshot);
    return deliver(*snapshot,
                   {account->primarySoid, character->soid, state::build_data::content_hash()},
                   payload);
}

/** Each row carries the catalog's item and rank beside the saved flags State read. */
InvestmentAnswer encode_season_pass_snapshot(InvestmentPayload payload) noexcept {
    state::SeasonPassSnapshot pass{};
    const char* reason = nullptr;
    if (!state::read_season_pass_snapshot(pass, &reason)) {
        return refused(reason != nullptr ? reason : "transaction");
    }
    wire::SeasonPassSnapshot snapshot{};
    snapshot.experience = pass.experience;
    snapshot.rank = pass.rank;
    const std::size_t count = (std::min)(pass.rowCount, snapshot.rows.size());
    snapshot.rowCount = static_cast<std::uint16_t>(count);
    for (std::size_t index = 0; index < count; ++index) {
        snapshot.rows[index] = season_pass_row(pass.rows[index]);
    }
    return deliver(snapshot,
                   {pass.accountSoid, pass.characterSoid, state::build_data::content_hash()},
                   payload);
}

/** Evaluates the selected character's held pursuits against the installed contracts. */
InvestmentAnswer encode_pursuit_snapshot(InvestmentPayload payload) noexcept {
    state::PursuitSnapshot pursuits{};
    const state::PursuitSnapshotRefusal refusal = state::read_pursuit_snapshot(pursuits);
    if (refusal != state::PursuitSnapshotRefusal::none) {
        return refused(pursuit_refusal_label(refusal));
    }
    const std::unique_ptr<wire::PursuitSnapshot> snapshot(new (std::nothrow)
                                                              wire::PursuitSnapshot{});
    if (snapshot == nullptr) {
        return refused("allocation");
    }
    const std::size_t count = (std::min)(pursuits.held.size(), snapshot->held.size());
    snapshot->heldCount = static_cast<std::uint16_t>(count);
    for (std::size_t index = 0; index < count; ++index) {
        snapshot->held[index] = held_pursuit(pursuits.held[index]);
    }
    return deliver(
        *snapshot,
        {pursuits.accountSoid, pursuits.characterSoid, state::build_data::content_hash()},
        payload);
}

/** Reads the selected character's saved rows, as the pursuit snapshot does. Unused values are 0. */
InvestmentAnswer encode_unlock_values(const wire::UnlockValuesRequest& request,
                                      InvestmentPayload payload) noexcept {
    std::array<state::unlocks::Instruction, wire::kUnlockReadCapacity> sources{};
    if (!decode_unlock_sources(request, sources)) {
        return invalid("request_field");
    }
    std::array<state::UnlockRead, wire::kUnlockReadCapacity> reads{};
    state::UnlockReadOrigin origin{};
    const state::PursuitSnapshotRefusal refusal =
        state::read_unlock_values(std::span(sources).first(request.count), reads, origin);
    if (refusal != state::PursuitSnapshotRefusal::none) {
        return refused(pursuit_refusal_label(refusal));
    }
    wire::UnlockValues values{};
    for (std::size_t index = 0; index < request.count; ++index) {
        wire::UnlockValue& value = values.values[index];
        value.state =
            reads[index].known ? wire::UnlockReadState::known : wire::UnlockReadState::invalid;
        value.value = reads[index].value;
    }
    return deliver(values,
                   {origin.accountSoid, origin.characterSoid, state::build_data::content_hash()},
                   payload);
}

/** A retained receipt is bound to its operation's target, and an unknown one to no context. */
InvestmentAnswer encode_receipt(const bap::InvestmentReceipt& receipt,
                                InvestmentPayload payload) noexcept {
    wire::Receipt record{};
    if (receipt.operationId == 0) {
        return deliver(record, {}, payload);
    }
    record.state = receipt_state(receipt.state, receipt.wait);
    record.wait = receipt_wait(receipt.wait);
    InvestmentAnswer answer = deliver(record, wire_context(receipt.target), payload);
    answer.reason = receipt.reason != nullptr ? receipt.reason : "";
    return answer;
}

} // namespace sunrise::server::studio
