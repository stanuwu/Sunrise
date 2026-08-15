#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::middleware::datagen::character_record::appearance {
namespace {

namespace buckets = state::build_data::abilities;

/** @param character Authored character. @return Its 5 selected socket entries. */
[[nodiscard]] buckets::Selection selection_of(const state::CharacterState& character) noexcept {
    return {character.movementAbilityEntry,
            character.grenadeAbilityEntry,
            character.superAbilityEntry,
            character.meleeAbilityEntry,
            character.classAbilityEntry};
}

} // namespace

/** Fills the 12 ability buckets from the character's subclass and ability picks. */
bool apply_ability_buckets(const state::CharacterState& character,
                           const family4::loadout::ResolvedInstances& instances,
                           layout::Appearance& appearance) noexcept {
    for (std::size_t index = 0; index < instances.itemCount; ++index) {
        if (instances.items[index].equipmentSlot != kSubclassEquipmentSlot) {
            continue;
        }
        details::Definition detail{};
        buckets::Definition published{};
        if (!state::build_data::find_configured_item_detail(
                instances.items[index].instance.baseDefinitionIndex, detail)
            || !state::build_data::find_ability_buckets(
                detail.socketEntryListIndex, selection_of(character), published)) {
            return false;
        }
        for (std::size_t bucket = 0; bucket < appearance.abilityBuckets.size(); ++bucket) {
            layout::AbilityBucket& target = appearance.abilityBuckets[bucket];
            target.kind = static_cast<std::int8_t>(published.buckets[bucket].kind);
            for (std::size_t entry = 0; entry < published.buckets[bucket].hashCount; ++entry) {
                target.hashes[entry] = published.buckets[bucket].hashes[entry];
            }
        }
        // The overflow bank takes the subclass hashes no bucket category claims. Whatever it does
        // not fill stays at the no-hash sentinel for the equipped plug hashes to take.
        for (std::size_t entry = 0; entry < published.overflowCount; ++entry) {
            appearance.overflowHashes[entry] = published.overflow[entry];
        }
        return true;
    }
    // A character with no subclass equipped publishes empty buckets, which is what the client
    // computes for it as well.
    return true;
}

} // namespace sunrise::middleware::datagen::character_record::appearance
