#include "account_encoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>

#include "../../../../state/account/inventory/synthesizer_tier.h"
#include "../../../../state/build_data/runtime.h"
#include "../../../../state/runtime/runtime.h"
#include "../../../../state/unlocks/unlocks_runtime.h"
#include "../../definitions.h"
#include "../progression/progression_bank_keys.h"
#include "layout.h"
#include "preferences/preferences_encoder.h"

namespace sunrise::middleware::datagen::family4::account {
namespace {

/** Signed 32-bit maximum keeps publicity deadlines beyond a normal session clock. */
constexpr std::uint64_t kSuppressedPublicityDeadline =
    static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
/** Every set bit marks all default account messages as already seen. */
constexpr std::byte kSeenMessageByte{0xFF};

static_assert(layout::kCharacterValueCapacity
              == state::build_data::constants::kCharacterValueRowCount);

/** Publishes each character's Synthesizer tier, read from the Synthesizer it holds. */
void publish_synthesizer_tiers(const state::AccountState& state, layout::Object& object) noexcept {
    state::build_data::constants::InvestmentConstants constants{};
    if (!state::build_data::find_investment_constants(constants)
        || constants.synthesizerTierValueRow == state::build_data::constants::kUnboundValueRow) {
        return;
    }
    for (std::size_t index = 0; index < state.characterCount; ++index) {
        const auto& items = state.characters[index].inventory;
        const std::size_t held = state::account::inventory::held_synthesizer(items);
        object.characterUnlocks[index].values[constants.synthesizerTierValueRow] =
            held == items.count
                ? state::account::inventory::kNoSynthesizerTier
                : state::account::inventory::synthesizer_tier(items.values[held].definitionHash);
    }
}

/** Places one profile item in the array row State resolved for it. */
[[nodiscard]] bool place_profile_item(const state::account::inventory::ProfileItem& item,
                                      std::size_t slot,
                                      std::span<inventory::layout::Entry> rows,
                                      std::span<std::uint32_t> newItems) noexcept {
    state::build_data::items::Definition definition{};
    state::build_data::items::details::Definition detail{};
    state::build_data::inventory::buckets::Descriptor bucket{};
    if (item.quantity <= 0 || item.mutationSerial < 0
        || !state::build_data::find_item_definition_hash(item.definitionHash, definition)
        || !state::build_data::find_configured_item_detail(definition.definitionIndex, detail)
        || detail.definitionIndex != definition.definitionIndex
        || detail.definitionHash != definition.definitionHash
        || detail.bucketId != definition.bucketId
        || detail.instancedDefinitionState
               != state::build_data::items::details::InstancedDefinitionState::stackable
        || !state::build_data::find_inventory_bucket_descriptor(definition.bucketId, bucket)
        || bucket.arraySelector != state::build_data::inventory::buckets::ArraySelector::profile) {
        return false;
    }
    const bool actionSource = state::build_data::is_profile_action_source(
        definition.definitionIndex, definition.bucketId);
    if (actionSource != (item.instanceSoid != 0)) {
        return false;
    }
    if (slot >= rows.size() || rows[slot].definitionIndex != kEmptyDefinitionIndex) {
        return false;
    }
    rows[slot].definitionIndex = definition.definitionIndex;
    rows[slot].instanceSoid = item.instanceSoid;
    rows[slot].quantity = item.quantity;
    rows[slot].mutationSerial = item.mutationSerial;
    // Each profile bitmap word covers 32 inventory rows.
    constexpr std::size_t kWordBits = 32;
    if (!item.seen) {
        newItems[slot / kWordBits] |= 1U << (slot % kWordBits);
    }
    return true;
}

} // namespace

/** Encodes a sentinel-correct account object from live State. */
bool encode(const state::AccountState& state, std::span<std::byte> output) noexcept {
    state::unlocks::Table unlocks;
    return state::unlocks::snapshot(unlocks) && encode(state, output, unlocks);
}

/**
 * Account-wide unlocks use the supplied snapshot; per-character flags still use saved state.
 * @param state Account identity, roster, preferences, and inventory to encode.
 * @param output Receives the account object; unchanged on failure.
 * @param unlocks Account unlocks from the same live or prepared view as state.
 * @return False when state, saved flags, mappings, or output bounds are invalid.
 */
bool encode(const state::AccountState& state,
            std::span<std::byte> output,
            const state::unlocks::Table& unlocks) noexcept {
    if (state.primarySoid == 0 || !state::account::valid(state)
        || output.size() < layout::kMinimumSize) {
        return false;
    }

    layout::Object object{};
    object.accountSoid = state.primarySoid;
    object.selectedCharacterSoid = state::account::selected_character_soid(state);
    object.profileSetupCompleted = state.profileSetupCompleted ? 1U : 0U;
    if (!roster::initialize(state, object.roster)
        || !preferences::encode(state.settings, object.preferences, object.bindings)) {
        return false;
    }

    object.acquiredFlags = unlocks.accountFlags;
    object.profileUnlockFlags = unlocks.profileFlags;
    object.objectiveValues = unlocks.objectiveValues;

    for (std::size_t index = 0; index < state.characterCount; ++index) {
        state::unlocks::Table character;
        if (!state::unlocks::snapshot(character, static_cast<int>(index))) {
            return false;
        }
        object.characterUnlocks[index].flags = character.characterFlags;
    }
    publish_synthesizer_tiers(state, object);
    object.publicityExpiries.fill(kSuppressedPublicityDeadline);
    object.seenMessages.fill(kSeenMessageByte);
    for (inventory::layout::Entry& item : object.profileItems) {
        item.definitionIndex = kEmptyDefinitionIndex;
    }
    for (inventory::layout::Entry& item : object.secondaryItems) {
        item.definitionIndex = kEmptyDefinitionIndex;
    }
    if (!progression::key_bank(
            state::build_data::progressions::Scope::account, object.progressions, unlocks)) {
        return false;
    }
    // Profile rows are sentinelled above, so placement only has to claim its own slots.
    const auto profileItems = std::span(state.profileItems).first(state.profileItemCount);
    std::array<std::uint16_t, state::account::inventory::kProfileItemCapacity> profileRows{};
    if (!state::account::inventory::resolve_profile_rows(profileItems, profileRows)) {
        return false;
    }
    for (std::size_t index = 0; index < profileItems.size(); ++index) {
        if (!place_profile_item(profileItems[index],
                                profileRows[index],
                                object.profileItems,
                                object.newItemFlags)) {
            return false;
        }
    }
    object.profileItemCount = static_cast<std::uint32_t>(state.profileItemCount);

    // A committed wallet sync advances this generation; ordinary inventory updates keep it.
    if (!state::read_store_wallet_sync(object.storeSyncRevision)) {
        return false;
    }
    if (object.storeSyncRevision != 0) {
        object.storeSyncState = layout::kStoreSyncStateSynced;
    }

    // Publish only after every fallible conversion succeeds.
    std::fill(output.begin(), output.end(), std::byte{});
    std::memcpy(output.data(), &object, sizeof object);
    return true;
}

} // namespace sunrise::middleware::datagen::family4::account
