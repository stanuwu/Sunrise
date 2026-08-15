#include "../../../../../middleware/datagen/definitions.h"
#include "../../../../../middleware/datagen/family4/loadout/loadout_resolver.h"
#include "../../../../../state/equipment/light/resolution/configured_equipment_light_resolver.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::snapshot {
namespace {

namespace family4_datagen = middleware::datagen::family4;
namespace light_resolution = state::equipment::light::resolution;

} // namespace

/** Finds the selected character row inside one validated account snapshot. */
std::optional<std::size_t> find_character_index(const state::AccountState& account) noexcept {
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            return index;
        }
    }
    return std::nullopt;
}

/** Finds one selected row plus its character schema and, if items exist, the instance schema. */
bool resolve(const state::AccountState& account,
             std::size_t characterIndex,
             Resolved& output) noexcept {
    Resolved staged{};
    staged.characterIndex = characterIndex;
    if (!family4_datagen::loadout::resolve(account, characterIndex, staged.loadout)
        || !middleware::datagen::object_id(
            kAccountFamilyType, kCharacterDefinitionSlotIndex, staged.characterObjectId)
        || (staged.loadout.itemCount != 0
            && !middleware::datagen::object_id(
                kAccountFamilyType, kItemDefinitionSlotIndex, staged.itemInstanceObjectId))
        || !light_resolution::resolve(account, characterIndex, staged.lightEvaluation)) {
        return false;
    }
    output = staged;
    return true;
}

} // namespace sunrise::server::bap::encrypted::push::snapshot
