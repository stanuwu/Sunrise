#include "account_encoder.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <span>

#include "../../../../state/build_data/runtime.h"
#include "../../../../state/unlocks/unlocks_runtime.h"
#include "../progression/progression_bank_keys.h"
#include "layout.h"
#include "preferences/preferences_encoder.h"

namespace sunrise::middleware::datagen::family4::account {
namespace {

/** Every bit set is the native empty biased 16-bit definition index. */
constexpr std::uint16_t kEmptyDefinitionIndex = (std::numeric_limits<std::uint16_t>::max)();
/** Signed 32-bit maximum keeps publicity deadlines beyond a normal session clock. */
constexpr std::uint64_t kSuppressedPublicityDeadline =
    static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)());
/** Every set bit marks all default account messages as already seen. */
constexpr std::byte kSeenMessageByte{0xFF};
/** A native inventory bucket id is 1 byte, so this covers every bucket. */
constexpr std::size_t kBucketIdentityCapacity = 256;

/** Places one profile item in the next free row of its inventory bucket. */
[[nodiscard]] bool place_profile_item(const state::account::inventory::ProfileItem& item,
                                      std::array<std::uint16_t, kBucketIdentityCapacity>& taken,
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
    const std::uint16_t used = taken[definition.bucketId];
    if (used >= bucket.slotCount) {
        return false;
    }
    const std::size_t slot = static_cast<std::size_t>(bucket.firstSlot) + used;
    if (slot >= rows.size() || rows[slot].definitionIndex != kEmptyDefinitionIndex) {
        return false;
    }
    taken[definition.bucketId] = static_cast<std::uint16_t>(used + 1);
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
    object.publicityExpiries.fill(kSuppressedPublicityDeadline);
    object.seenMessages.fill(kSeenMessageByte);
    for (inventory::layout::Entry& item : object.profileItems) {
        item.definitionIndex = kEmptyDefinitionIndex;
    }
    for (inventory::layout::Entry& item : object.secondaryItems) {
        item.definitionIndex = kEmptyDefinitionIndex;
    }
    if (!progression::key_bank(state::build_data::progressions::Scope::account,
                               object.progressions)) {
        return false;
    }
    // Profile rows are sentinelled above, so placement only has to claim its own slots.
    std::array<std::uint16_t, kBucketIdentityCapacity> takenSlots{};
    for (std::size_t index = 0; index < state.profileItemCount; ++index) {
        if (!place_profile_item(
                state.profileItems[index], takenSlots, object.profileItems, object.newItemFlags)) {
            return false;
        }
    }
    object.profileItemCount = static_cast<std::uint32_t>(state.profileItemCount);

    // Publish only after every fallible conversion succeeds.
    std::fill(output.begin(), output.end(), std::byte{});
    std::memcpy(output.data(), &object, sizeof object);
    return true;
}

} // namespace sunrise::middleware::datagen::family4::account
