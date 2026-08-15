#include "../../../../state/build_data/runtime.h"
#include "internal.h"

namespace sunrise::middleware::datagen::character_record::appearance {
namespace {

/**
 * The three art stages, in the order the client folds them.
 * Base stage 1, then plug stage 1, plug stage 0, base stage 2, plug stage 2. The order decides
 * which 6 keys survive, so the stages cannot be merged.
 */
enum class Stage : std::uint8_t {
    plugOnly = 0,
    base = 1,
    late = 2,
};

/** Insertion-ordered material keys. Only the first 6 reach the record, so 6 is the whole set. */
struct Fold {
    std::array<layout::MaterialPair, layout::kMaterialPairCapacity> pairs{};
    std::size_t count{};
};

/**
 * Records one material row, keeping insertion order and letting a later stage update a value.
 * A key arriving after 6 distinct keys are held can never reach the record, so it is dropped.
 * @param fold Ordered material keys.
 */
void record(Fold& fold, std::int8_t key, std::uint16_t value) noexcept {
    for (std::size_t entry = 0; entry < fold.count; ++entry) {
        if (fold.pairs[entry].key == key) {
            fold.pairs[entry].value = value;
            return;
        }
    }
    if (fold.count < fold.pairs.size()) {
        fold.pairs[fold.count].key = key;
        fold.pairs[fold.count].value = value;
        ++fold.count;
    }
}

/**
 * Folds one definition's rows for a single art stage.
 * @param detail Installed detail carrying the definition's override rows.
 * @param stage Art stage to take.
 * @param fold Ordered material keys.
 */
void fold_stage(const details::Definition& detail, Stage stage, Fold& fold) noexcept {
    const std::size_t rows = detail.renderOverrideCount < detail.renderOverrides.size()
                                 ? detail.renderOverrideCount
                                 : detail.renderOverrides.size();
    for (std::size_t entry = 0; entry < rows; ++entry) {
        const details::RenderOverride& row = detail.renderOverrides[entry];
        if (row.stage == static_cast<std::uint8_t>(stage)
            && row.key != details::kEmptyOverrideKey) {
            record(fold, row.key, row.value);
        }
    }
}

/**
 * Folds every plug lane's rows for one art stage.
 * @param equipped Effective plug lanes.
 * @param stage Art stage to take.
 * @param fold Ordered material keys.
 */
void fold_plug_stage(const Equipped& equipped, Stage stage, Fold& fold) noexcept {
    for (std::size_t lane = 0; lane < equipped.laneCount; ++lane) {
        details::Definition plug{};
        if (equipped.plugs[lane] == details::kUnavailableItemIndex
            || !state::build_data::find_configured_item_detail(equipped.plugs[lane], plug)) {
            continue;
        }
        fold_stage(plug, stage, fold);
    }
}

/**
 * Builds the 6 material pairs one equipped item publishes.
 * @param detail Base item detail.
 * @param equipped Effective plug lanes.
 * @param entry Render row receiving the pairs.
 */
void apply_material_pairs(const details::Definition& detail,
                          const Equipped& equipped,
                          layout::RenderEntry& entry) noexcept {
    Fold fold{};
    fold_stage(detail, Stage::base, fold);
    fold_plug_stage(equipped, Stage::base, fold);
    fold_plug_stage(equipped, Stage::plugOnly, fold);
    fold_stage(detail, Stage::late, fold);
    fold_plug_stage(equipped, Stage::late, fold);
    for (std::size_t pair = 0; pair < fold.count; ++pair) {
        entry.materialPairs[pair] = fold.pairs[pair];
    }
}

} // namespace

/** Resolves one equipped instance to its detail and the plugs its sockets hold. */
bool resolve_equipped(const family4::loadout::SlottedInstance& slotted,
                      details::Definition& detail,
                      Equipped& equipped) noexcept {
    equipped = {};
    equipped.plugs.fill(details::kUnavailableItemIndex);
    equipped.equipmentSlot = slotted.equipmentSlot;
    equipped.definitionIndex = slotted.instance.baseDefinitionIndex;
    if (!state::build_data::find_configured_item_detail(slotted.instance.baseDefinitionIndex,
                                                        detail)) {
        return false;
    }
    equipped.laneCount = detail.ordinarySocketCount < equipped.plugs.size()
                             ? detail.ordinarySocketCount
                             : equipped.plugs.size();
    const auto& authored = slotted.instance.ordinarySockets.plugs;
    for (std::size_t lane = 0; lane < equipped.laneCount && lane < authored.size(); ++lane) {
        if (authored[lane].has_value()) {
            equipped.plugs[lane] = *authored[lane];
        }
    }
    return true;
}

/** Fills each equipped render row with its instance, definition, art and material pairs. */
bool apply_render(const family4::loadout::ResolvedInstances& instances,
                  layout::Appearance& appearance) noexcept {
    for (std::size_t index = 0; index < instances.itemCount; ++index) {
        const family4::loadout::SlottedInstance& slotted = instances.items[index];
        if (slotted.equipmentSlot >= appearance.render.size()) {
            return false;
        }
        layout::RenderEntry& entry = appearance.render[slotted.equipmentSlot];
        entry.instanceSoid = slotted.instance.instanceSoid;
        entry.definitionIndex = slotted.instance.baseDefinitionIndex;
        details::Definition detail{};
        Equipped equipped{};
        if (!resolve_equipped(slotted, detail, equipped)) {
            continue;
        }
        // Both art lookups accept 0, so an item with no art block keeps the empty sentinel
        // rather than taking art row 0.
        entry.art[layout::kGearArtSlot] = detail.gearArtIndex;
        entry.art[layout::kArtArrangementSlot] = detail.artArrangementIndex;
        apply_material_pairs(detail, equipped, entry);
    }
    return true;
}

} // namespace sunrise::middleware::datagen::character_record::appearance
