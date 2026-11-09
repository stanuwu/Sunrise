#include "pursuit_snapshot_runtime.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <utility>

#include "../account/account_state.h"
#include "../build_data/runtime.h"
#include "../rewards/reward_resolver.h"
#include "saved_selection_runtime.h"

namespace sunrise::state {
namespace {

namespace pursuits = build_data::pursuits;

using SavedState = SavedSelection<AccountState>;

/** Copies the selected character's saved view, or returns why it could not be copied. */
[[nodiscard]] PursuitSnapshotRefusal read_saved_state(SavedState& saved) noexcept {
    switch (read_saved_selection(saved)) {
    case SavedSelectionRead::read:
        return PursuitSnapshotRefusal::none;
    case SavedSelectionRead::accountStorage:
    case SavedSelectionRead::accountInvalid:
        return PursuitSnapshotRefusal::accountUnavailable;
    case SavedSelectionRead::unselected:
        return PursuitSnapshotRefusal::characterUnavailable;
    case SavedSelectionRead::bankStorage:
        break;
    }
    return PursuitSnapshotRefusal::storage;
}

/**
 * Evaluates one inventory resident when it is a pursuit.
 * @return False when the resident is not in the pursuit bucket.
 */
[[nodiscard]] bool read_held_pursuit(const account::inventory::Item& item,
                                     const reward_resolution::Context& context,
                                     HeldPursuitStatus& held) {
    build_data::items::Definition definition{};
    if (!build_data::find_item_definition_hash(item.definitionHash, definition)
        || !build_data::items::pursuit_bucket_item(definition)) {
        return false;
    }
    held.instanceSoid = item.instanceSoid;
    held.definitionHash = item.definitionHash;
    held.mutationSerial = item.mutationSerial;
    held.flags = item.flags;
    pursuits::Definition contract{};
    if (!build_data::find_pursuit_definition(definition.definitionIndex, contract)
        || contract.definitionHash != item.definitionHash) {
        return true;
    }
    held.lifetimeSeconds = contract.lifetimeSeconds;
    const account::inventory::ItemObjectives values =
        item.objectives.value_or(account::inventory::ItemObjectives{});
    held.completion = inspect_pursuit_completion(contract, context, values);
    return true;
}

} // namespace

/** Reads one coherent saved view and evaluates each held pursuit's installed contract. */
PursuitSnapshotRefusal read_pursuit_snapshot(PursuitSnapshot& output) noexcept {
    output = {};
    if (build_data::pursuit_catalog_unsupported()) {
        return PursuitSnapshotRefusal::catalogUnsupported;
    }
    if (!build_data::objective_definitions_ready() || !build_data::pursuit_definitions_ready()) {
        return PursuitSnapshotRefusal::catalogUnavailable;
    }
    const std::uint64_t definitionsRevision = build_data::pursuit_definitions_revision();
    const std::unique_ptr<SavedState> saved(new (std::nothrow) SavedState{});
    if (!saved) {
        return PursuitSnapshotRefusal::storage;
    }
    const PursuitSnapshotRefusal refusal = read_saved_state(*saved);
    if (refusal != PursuitSnapshotRefusal::none) {
        return refusal;
    }
    const CharacterState& character = saved->account.characters[saved->slot];
    const reward_resolution::Context context = saved->context();
    try {
        PursuitSnapshot snapshot{};
        snapshot.accountSoid = saved->account.primarySoid;
        snapshot.characterSoid = character.soid;
        for (std::size_t i = 0; i < character.inventory.count; ++i) {
            HeldPursuitStatus held{};
            if (read_held_pursuit(character.inventory.values[i], context, held)) {
                snapshot.held.push_back(held);
            }
        }
        // A republish between the contract reads would mix two catalogs in one snapshot.
        if (build_data::pursuit_definitions_revision() != definitionsRevision) {
            return PursuitSnapshotRefusal::catalogUnavailable;
        }
        output = std::move(snapshot);
        return PursuitSnapshotRefusal::none;
    } catch (...) {
        return PursuitSnapshotRefusal::storage;
    }
}

/** Each source is evaluated alone, as a one-instruction program over the same saved copy. */
PursuitSnapshotRefusal read_unlock_values(std::span<const unlocks::Instruction> sources,
                                          std::span<UnlockRead> output,
                                          UnlockReadOrigin& origin) noexcept {
    origin = {};
    std::fill(output.begin(), output.end(), UnlockRead{});
    const std::unique_ptr<SavedState> saved(new (std::nothrow) SavedState{});
    if (!saved) {
        return PursuitSnapshotRefusal::storage;
    }
    const PursuitSnapshotRefusal refusal = read_saved_state(*saved);
    if (refusal != PursuitSnapshotRefusal::none) {
        return refusal;
    }
    const reward_resolution::Context context = saved->context();
    const std::size_t count = (std::min)(sources.size(), output.size());
    for (std::size_t index = 0; index < count; ++index) {
        UnlockRead& read = output[index];
        read.known = reward_resolution::evaluate(sources.subspan(index, 1), context, read.value);
    }
    origin = {saved->account.primarySoid, saved->account.characters[saved->slot].soid};
    return PursuitSnapshotRefusal::none;
}

} // namespace sunrise::state
