#include "internal.h"

namespace sunrise::middleware::datagen::character_record::appearance {

/** Fills every empty-valued field with the sentinel its reader tests for. */
void apply_sentinels(layout::Appearance& appearance) noexcept {
    for (layout::AbilityBucket& bucket : appearance.abilityBuckets) {
        bucket.kind = layout::kEmptyKey;
        bucket.hashes.fill(layout::kNoHash);
    }
    appearance.overflowHashes.fill(layout::kNoHash);
    for (layout::RenderEntry& entry : appearance.render) {
        entry.definitionIndex = layout::kEmptyDefinitionIndex;
        entry.art.fill(layout::kEmptyDefinitionIndex);
        entry.overlays.fill(layout::kEmptyDefinitionIndex);
        for (layout::UnlockPair& pair : entry.unlockPairs) {
            pair.definitionHash = layout::kNoHash;
        }
        for (layout::MaterialPair& pair : entry.materialPairs) {
            pair.key = layout::kEmptyKey;
            pair.value = layout::kEmptyDefinitionIndex;
        }
    }
    for (layout::KeyedRow& row : appearance.keyedRows) {
        row.key = layout::kEmptyKey;
    }
    appearance.indexBank.fill(layout::kEmptyDefinitionIndex);
    appearance.smallBankA.fill(layout::kEmptyDefinitionIndex);
    appearance.smallBankB.fill(layout::kEmptyDefinitionIndex);
    appearance.smallBankC.fill(layout::kEmptyDefinitionIndex);
    for (layout::StatRow& row : appearance.characterStats) {
        row.key = layout::kEmptyKey;
        row.value = layout::kEmptyStatValue;
    }
    for (std::array<layout::StatRow, layout::kStatRowCapacity>& table : appearance.weaponStats) {
        for (layout::StatRow& row : table) {
            row.key = layout::kEmptyKey;
            row.value = layout::kEmptyStatValue;
        }
    }
}

} // namespace sunrise::middleware::datagen::character_record::appearance
