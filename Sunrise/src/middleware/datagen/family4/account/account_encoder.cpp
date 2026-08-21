#include <atomic>

#include "../../../../state/build_data/nodes/node_catalog.h"
#include "account_encoder.h"

#include <algorithm>
#include <cstring>
#include <limits>

#include "../../../../state/build_data/runtime.h"
#include "../../../../state/build_data/records/definition.h"
#include "../../../../state/record_claims/record_claims.h"
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

/**
 * Places one authored profile item in the slot run its inventory bucket owns.
 * The slot is not authored: the bucket descriptor names the first slot of its run, and items
 * sharing a bucket take consecutive slots in configuration order.
 * @param item Authored account-wide item.
 * @param taken Slots already claimed inside each bucket, indexed by bucket id.
 * @param rows Profile inventory rows.
 * @return True when the item resolves to a free profile slot.
 */
[[nodiscard]] bool place_profile_item(const state::account::inventory::ProfileItem& item,
                                      std::array<std::uint16_t, kBucketIdentityCapacity>& taken,
                                      std::span<inventory::layout::Entry> rows) noexcept {
    // The dense item table already carries the bucket, so a profile item needs no detail record.
    // Only equipped items and their plugs have one.
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
    return true;
}

} // namespace

/** Encodes a sentinel-correct account object from authored State. */
bool encode(const state::AccountState& state, std::span<std::byte> output) noexcept {
    if (state.primarySoid == 0 || !state::account::valid(state)
        || output.size() < layout::kMinimumSize) {
        return false;
    }

    layout::Object object{};
    object.accountSoid = state.primarySoid;
    object.selectedCharacterSoid = state::account::selected_character_soid(state);
    if (!roster::initialize(state, object.roster)
        || !preferences::encode(state.settings, object.preferences, object.bindings)) {
        return false;
    }

    // Acquired flags and objective progress are authored policy, published once per process.
    const state::unlocks::Table& unlocks = state::unlocks::get();
    object.acquiredFlags = unlocks.accountFlags;
    // Claims arrive after boot, so they cannot be in the authored policy. Lay them over the
    // bank here, which is the one place every Family-4 account image passes through.
    (void)state::record_claims::apply(object.acquiredFlags);
    // Lore book categories are gated on a flag they cannot set by being played: with no title shown
    // there is nothing inside to claim, and nothing to claim leaves the gate shut.
    (void)state::build_data::nodes::apply_visibility(object.acquiredFlags);
    object.profileUnlockFlags = unlocks.profileFlags;
    object.objectiveValues = unlocks.objectiveValues;
    // Triumph Score is a plain replicated value the client never derives, so total the claims made
    // this session over whatever the policy authored and publish the sum.
    if (state::build_data::records::kTriumphScoreValueIndex < object.objectiveValues.size()) {
        auto& score = object.objectiveValues[state::build_data::records::kTriumphScoreValueIndex];
        score += static_cast<std::int32_t>(state::record_claims::total_score());
    }
    // A node's progress bar reads a value slot and shows whatever it holds, so the claimed children
    // have to be counted into it here or the bar never moves.
    (void)state::record_claims::apply_node_progress(object.objectiveValues);
    for (layout::CharacterUnlockBlock& block : object.characterUnlocks) {
        block.flags = unlocks.characterFlags;
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
        if (!place_profile_item(state.profileItems[index], takenSlots, object.profileItems)) {
            return false;
        }
    }
    object.profileItemCount = static_cast<std::uint32_t>(state.profileItemCount);

    // Commit only after every fallible conversion succeeds so callers never receive a partial
    // account object.
    std::fill(output.begin(), output.end(), std::byte{});
    std::memcpy(output.data(), &object, sizeof object);
    return true;
}

} // namespace sunrise::middleware::datagen::family4::account
