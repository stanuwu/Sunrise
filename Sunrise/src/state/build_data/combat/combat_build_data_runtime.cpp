#include "../runtime.h"
#include "../runtime/persistence/publication_transaction.h"
#include "combat_catalog.h"

namespace sunrise::state::build_data {

/** @return True when the actor class races and label groups are published. */
bool combat_taxonomy_ready() noexcept {
    return combat::count() != 0;
}

/** @return True once the installed data has settled that no taxonomy can be read. */
bool combat_taxonomy_unsupported() noexcept {
    return combat::unsupported();
}

/** Checks both tables against the catalog's publication rules. */
bool valid_combat_taxonomy(std::span<const combat::EnemyRace> races,
                           std::span<const combat::LabelGroup> groups) noexcept {
    return combat::valid(races) && combat::valid(groups);
}

/** Publishes the actor class races and label groups together. */
bool publish_combat_taxonomy(std::span<const combat::EnemyRace> races,
                             std::span<const combat::LabelGroup> groups) noexcept {
    runtime::persistence::Transaction transaction;
    return transaction.active()
           && transaction.finish(combat::replace(races, groups), combat::clear);
}

/** Records the settlement, and forgets it when the first cache write after it fails. */
void settle_combat_taxonomy_unsupported() noexcept {
    runtime::persistence::Transaction transaction;
    if (!transaction.active()) {
        return;
    }
    combat::settle_unsupported();
    static_cast<void>(transaction.finish(true, combat::clear));
}

/** Reads the race of one actor class. */
bool find_enemy_race(std::uint32_t classHash, std::uint8_t& race) noexcept {
    return combat::find_race(classHash, race);
}

/** Tests one label's membership in one group. */
bool label_in_group(std::uint32_t groupHash, std::uint32_t labelHash) noexcept {
    return combat::in_group(groupHash, labelHash);
}

} // namespace sunrise::state::build_data
