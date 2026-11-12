/**
 * Dawning oven bakes: a Combine or recipe plug charges a recipe and pays its cookie. An acquired
 * oven opens its ingredient chooser.
 */

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>

#include "../build_data/runtime.h"
#include "bucket_admission.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail {

namespace {

namespace item_details = build_data::items::details;

/** One recipe: its plug, the discounted plug a masterworked oven offers instead, its cookie. */
struct OvenRecipe {
    std::uint32_t recipePlugHash{};
    std::uint32_t masterworkedPlugHash{};
    std::uint32_t cookieHash{};
};

/** One ingredient pickup and the oven plug whose balance it fills. */
struct IngredientPickup {
    std::uint32_t pickupHash{};
    std::uint32_t ingredientPlugHash{};
};

// --- Authored game data --------------------------------------------------------------------

// TODO: extract these rows from the packages; no extractor relates a recipe plug to its cookie or
// its masterworked plug, an unmatched Combine to its cookie, a pickup to the plug it fills, the
// oven to the tutorial quest whose row its ingredient chooser reads, or a Dawning bounty objective
// to the bake or delivery it counts.

/** The 22 recipes. Each recipe plug's own cost names the ingredient pair a Combine matches. */
constexpr std::array<OvenRecipe, 22> kOvenRecipes{{
    {2546992977U, 3884817923U, 2704624939U},
    {3457862632U, 1289611082U, 2704624938U},
    {2567160079U, 3766828181U, 2704624937U},
    {1566816806U, 1455708620U, 2704624936U},
    {2680500173U, 3593525111U, 2704624943U},
    {2690793028U, 1776437198U, 2704624942U},
    {3468840443U, 2252566553U, 2704624941U},
    {1972037794U, 1736316176U, 2704624940U},
    {1556827161U, 4164579835U, 2704624931U},
    {1040576784U, 2667777186U, 2704624930U},
    {1883538310U, 3051102764U, 4041767214U},
    {3626109295U, 1067152117U, 4041767215U},
    {2412808648U, 1481653034U, 4041767212U},
    {1460362289U, 3334529507U, 4041767213U},
    {461052802U, 2164922480U, 4041767210U},
    {3827035483U, 1319397369U, 4041767211U},
    {3986202916U, 4032646574U, 4041767208U},
    {423273389U, 4022027991U, 4041767209U},
    {2800565166U, 923317028U, 4041767206U},
    {2789946583U, 1655354797U, 4041767207U},
    {1551725739U, 3407940349U, 4024989627U},
    {4165341394U, 1494246964U, 4024989626U},
}};

/** Burnt Edge Transits: what a Combine bakes when no recipe pairs its two ingredients. */
constexpr std::uint32_t kBurntCookieHash = 2281231178U;
/** The Combine plug, which bakes from the two ingredients in the ingredient lanes. */
constexpr std::uint32_t kCombinePlugHash = 2161676424U;
/** The Masterwork plug, which swaps the recipe lane to the discounted recipes. */
constexpr std::uint32_t kMasterworkPlugHash = 2525467079U;
/** Lanes holding the two ingredients a Combine bakes. */
constexpr std::array<std::uint8_t, 2> kIngredientLanes{0, 1};
/** Lane holding the Masterwork plug. A bake returns every lane before it to its initial plug. */
constexpr std::uint8_t kMasterworkLane = 4;
/** A burnt bake spends one unit of each ingredient, as every recipe does. */
constexpr std::uint32_t kBurntIngredientCost = 1;

/** The 20 ingredients, each pickup beside the plug that counts it. */
constexpr std::array<IngredientPickup, 20> kIngredientPickups{{
    {4057968891U, 3010348881U}, {4057968890U, 3010348880U}, {4057968889U, 3010348883U},
    {4057968888U, 3010348882U}, {4057968895U, 3010348885U}, {4057968894U, 3010348884U},
    {4057968893U, 3010348887U}, {4057968892U, 3010348886U}, {4057968883U, 3010348889U},
    {4057968882U, 3010348888U}, {3173226942U, 3331998400U}, {3173226943U, 3331998401U},
    {3173226940U, 3331998402U}, {3173226941U, 3331998403U}, {3173226938U, 3331998404U},
    {3173226939U, 3331998405U}, {3173226936U, 3331998406U}, {3173226937U, 3331998407U},
    {3173226934U, 3331998408U}, {3173226935U, 3331998409U},
}};

/** The Dawning oven. */
constexpr std::uint32_t kOvenHash = 2742763040U;
/**
 * Items held while the Dawning Begins tutorial is under way. Three are its quest steps, which name
 * the quest row the oven's ingredient chooser reads.
 */
constexpr std::array<std::uint32_t, 4> kTutorialItemHashes{
    2457995100U, 613001527U, 2659620686U, 544905217U};

/** The tutorial quest value that opens the ingredient chooser; no quest step starts from it. */
constexpr std::int32_t kChooserReadyValue = build_data::items::kInvalidQuestInitialValue;

/** One Dawning bounty objective a bake or a delivery advances by one. */
struct DawningBountyRule {
    std::uint32_t bountyHash{};
    std::uint32_t objectiveHash{};
    DawningEvent event{};
    /** The cookie the event must name, or kAnyCookie. */
    std::uint32_t cookieHash{};
};

/** A rule's cookie when any recipe's cookie counts. */
constexpr std::uint32_t kAnyCookie = 0;

/**
 * Sweet Gifts counts bakes and The Spirit of Giving counts deliveries. Each Gift bounty after them
 * counts deliveries of its own cookie.
 */
constexpr auto kDawningBountyRules = std::to_array<DawningBountyRule>({
    {2568759381U, 2757754782U, DawningEvent::bake, kAnyCookie},
    {2568759380U, 22889993U, DawningEvent::delivery, kAnyCookie},
    {3089242929U, 2548740524U, DawningEvent::delivery, 2704624939U},
    {2121605528U, 773356093U, DawningEvent::delivery, 2704624938U},
    {711780003U, 3624753994U, DawningEvent::delivery, 2704624937U},
    {1387738947U, 596105834U, DawningEvent::delivery, 2704624936U},
    {3609320918U, 2454287063U, DawningEvent::delivery, 2704624943U},
    {782388144U, 1605382853U, DawningEvent::delivery, 2704624942U},
    {1486111804U, 926975233U, DawningEvent::delivery, 2704624941U},
    {3529619172U, 874584025U, DawningEvent::delivery, 2704624940U},
    {3292943447U, 2595545358U, DawningEvent::delivery, 2704624931U},
    {3512551717U, 2708783760U, DawningEvent::delivery, 2704624930U},
    {1932203931U, 876421010U, DawningEvent::delivery, 4041767214U},
    {3112855567U, 2037977526U, DawningEvent::delivery, 4041767215U},
    {1494697569U, 1080833820U, DawningEvent::delivery, 4041767212U},
    {789196172U, 355117905U, DawningEvent::delivery, 4041767213U},
    {729780848U, 2874539525U, DawningEvent::delivery, 4041767210U},
    {90105468U, 2319574209U, DawningEvent::delivery, 4041767211U},
    {1491123344U, 3363642277U, DawningEvent::delivery, 4041767208U},
    {3503381224U, 2462970189U, DawningEvent::delivery, 4041767209U},
    {2212074115U, 2889593642U, DawningEvent::delivery, 4041767206U},
    {833201720U, 4279320669U, DawningEvent::delivery, 4041767207U},
    {2514375981U, 1082188232U, DawningEvent::delivery, 4024989627U},
    {2420899653U, 3129351088U, DawningEvent::delivery, 4024989626U},
});

// The bake's charge and its own writes must fit the transaction's unlock writes together.
static_assert(kSocketUnlockWriteCapacity
              >= build_data::material_requirements::kRequirementCapacity + kIngredientLanes.size());

/** @return The recipe a recipe plug, normal or masterworked, bakes; null for any other plug. */
[[nodiscard]] const OvenRecipe* find_recipe(std::uint32_t plugHash) noexcept {
    for (const OvenRecipe& recipe : kOvenRecipes) {
        if (recipe.recipePlugHash == plugHash || recipe.masterworkedPlugHash == plugHash) {
            return &recipe;
        }
    }
    return nullptr;
}

/**
 * Reads whether a recipe is discovered: the flag every plug set offering its plug requires.
 * @return False when the recipe plug or its flag does not resolve.
 */
[[nodiscard]] bool recipe_discovered(const OvenRecipe& recipe,
                                     const AccountUnlockWrites& writes,
                                     bool& discovered) noexcept {
    build_data::items::Definition plug{};
    std::int32_t flag = 0;
    if (!build_data::find_item_definition_hash(recipe.recipePlugHash, plug)
        || plug.plugSetFlagIndex == build_data::items::kUnavailableFlagIndex
        || !read_account_unlock(writes, AccountUnlockBank::flags, plug.plugSetFlagIndex, flag)) {
        return false;
    }
    discovered = flag == unlocks::kFlagSet;
    return true;
}

/**
 * @param reason Receives a static label when the result is false.
 * @return True when every recipe is discovered, which the Masterwork plug requires.
 */
[[nodiscard]] bool all_recipes_discovered(const char*& reason) noexcept {
    const AccountUnlockWrites unstaged{};
    for (const OvenRecipe& recipe : kOvenRecipes) {
        bool discovered = false;
        if (!recipe_discovered(recipe, unstaged, discovered)) {
            reason = "oven_recipe_flag";
            return false;
        }
        if (!discovered) {
            reason = "oven_masterwork_undiscovered";
            return false;
        }
    }
    return true;
}

/**
 * Reads the two ingredients a recipe plug's own cost names: the items of its value-slot rows.
 * @return False unless the cost holds exactly two such rows.
 */
[[nodiscard]] bool recipe_ingredients(const build_data::items::Definition& recipePlug,
                                      std::array<std::uint16_t, 2>& ingredients) noexcept {
    build_data::material_requirements::Definition cost{};
    if (!build_data::find_material_requirement_set(
            recipePlug.insertionMaterialRequirementSetIndex, cost)) {
        return false;
    }
    std::size_t count = 0;
    for (const auto& requirement : std::span(cost.requirements).first(cost.requirementCount)) {
        if (requirement.condition == build_data::material_requirements::kUnconditionalRequirement) {
            continue;
        }
        if (count == ingredients.size()) {
            return false;
        }
        ingredients[count++] = requirement.itemDefinitionIndex;
    }
    return count == ingredients.size();
}

/**
 * Finds the recipe whose cost names the two plugged ingredients, in either order.
 * @param recipe Receives the recipe, or null when none pairs them.
 * @param recipePlug Receives the matched recipe's plug.
 * @return False when a recipe does not resolve or two recipes pair the ingredients.
 */
[[nodiscard]] bool match_recipe(std::uint16_t first,
                                std::uint16_t second,
                                const OvenRecipe*& recipe,
                                build_data::items::Definition& recipePlug) noexcept {
    recipe = nullptr;
    for (const OvenRecipe& candidate : kOvenRecipes) {
        build_data::items::Definition plug{};
        std::array<std::uint16_t, 2> pair{};
        if (!build_data::find_item_definition_hash(candidate.recipePlugHash, plug)
            || !recipe_ingredients(plug, pair)) {
            return false;
        }
        if ((pair[0] == first && pair[1] == second) || (pair[0] == second && pair[1] == first)) {
            if (recipe != nullptr) {
                return false;
            }
            recipe = &candidate;
            recipePlug = plug;
        }
    }
    return true;
}

/** @return False when the cookie is not installed. */
[[nodiscard]] bool find_cookie(std::uint32_t cookieHash, OvenBake& bake) noexcept {
    build_data::items::Definition cookie{};
    if (!build_data::find_item_definition_hash(cookieHash, cookie)) {
        return false;
    }
    bake.cookieIndex = cookie.definitionIndex;
    return true;
}

/**
 * Chooses what a Combine bakes from the plugged ingredients: the recipe they pair, or a burnt
 * cookie that charges the Combine's own cost and one unit of each ingredient's balance.
 * @param reason Receives a static label when the result is false.
 * @return False when a lane holds no ingredient, a balance does not cover it, or no recipe
 * resolves.
 */
[[nodiscard]] bool stage_combine(const account::inventory::Sockets& sockets,
                                 const build_data::items::Definition& combine,
                                 OvenBake& bake,
                                 const char*& reason) noexcept {
    std::array<build_data::items::Definition, kIngredientLanes.size()> ingredients{};
    for (std::size_t index = 0; index < ingredients.size(); ++index) {
        const auto& plug = sockets.plugs[kIngredientLanes[index]];
        if (!plug.has_value()
            || !build_data::find_item_definition_hash(*plug, ingredients[index])) {
            reason = "oven_ingredient_lane";
            return false;
        }
    }
    const OvenRecipe* recipe = nullptr;
    build_data::items::Definition recipePlug{};
    if (!match_recipe(
            ingredients[0].definitionIndex, ingredients[1].definitionIndex, recipe, recipePlug)) {
        reason = "oven_recipe_match";
        return false;
    }
    if (recipe != nullptr) {
        bake.costSetIndex = recipePlug.insertionMaterialRequirementSetIndex;
        bake.discoveryFlagIndex = recipePlug.plugSetFlagIndex;
        if (bake.discoveryFlagIndex == build_data::items::kUnavailableFlagIndex) {
            reason = "oven_recipe_flag";
            return false;
        }
        if (!find_cookie(recipe->cookieHash, bake)) {
            reason = "oven_cookie";
            return false;
        }
        return true;
    }
    // Each ingredient plug counts its own balance, which is the one a burnt bake spends.
    for (const auto& ingredient : ingredients) {
        if (ingredient.quantityValueIndex == build_data::items::kUnavailableValueIndex) {
            reason = "oven_ingredient_value";
            return false;
        }
        if (!debit_account_value(
                bake.unlockWrites, ingredient.quantityValueIndex, kBurntIngredientCost)) {
            reason = "oven_ingredient_balance";
            return false;
        }
    }
    bake.costSetIndex = combine.insertionMaterialRequirementSetIndex;
    if (!find_cookie(kBurntCookieHash, bake)) {
        reason = "oven_cookie";
        return false;
    }
    return true;
}

/**
 * Returns every lane before the Masterwork lane to its initial plug.
 * @param reason Receives a static label when the result is false.
 * @return False when a lane has no initial plug or the requested lane is not reset.
 */
[[nodiscard]] bool reset_lanes(const item_details::Definition& oven,
                               const account::inventory::Sockets& sockets,
                               std::uint8_t socketLane,
                               OvenBake& bake,
                               const char*& reason) noexcept {
    bake.sockets = sockets;
    for (std::uint8_t lane = 0; lane < kMasterworkLane; ++lane) {
        const std::uint16_t initialPlugIndex = oven.initialPlugIndices[lane];
        build_data::items::Definition plug{};
        if (initialPlugIndex == item_details::kUnavailableItemIndex
            || !build_data::find_item_definition_index(initialPlugIndex, plug)) {
            reason = "oven_initial_plug";
            return false;
        }
        bake.sockets.plugs[lane] = plug.definitionHash;
        if (lane == socketLane) {
            bake.lanePlug = plug;
        }
    }
    if (socketLane >= kMasterworkLane) {
        reason = "oven_lane";
        return false;
    }
    return true;
}

/** @return The oven plug a pickup fills, or null for any other item. */
[[nodiscard]] const IngredientPickup* find_pickup(std::uint32_t pickupHash) noexcept {
    for (const IngredientPickup& pickup : kIngredientPickups) {
        if (pickup.pickupHash == pickupHash) {
            return &pickup;
        }
    }
    return nullptr;
}

/** An item the item table does not hold is final once the catalogs a grant reads have settled. */
[[nodiscard]] RewardPreparation missing_item() noexcept {
    return catalogs_settled() ? RewardPreparation::unresolvable : RewardPreparation::notReady;
}

/**
 * A contract or objective the pursuit catalog does not hold is final once the contracts and their
 * objectives are published, or the catalog is settled unsupported. Until then nothing is decided.
 */
[[nodiscard]] RewardPreparation missing_contract() noexcept {
    const bool settled =
        (build_data::pursuit_definitions_ready() && build_data::objective_definitions_ready())
        || build_data::pursuit_catalog_unsupported();
    return settled ? RewardPreparation::unresolvable : RewardPreparation::notReady;
}

/**
 * Reads the account row the tutorial's quest steps share.
 * @return Unresolvable when a step names another bank or two steps name different rows. A
 *         tutorial item that is not installed ends as missing_item does, and contracts that name
 *         no step as missing_contract does.
 */
[[nodiscard]] RewardPreparation tutorial_quest_row(std::uint16_t& row) noexcept {
    bool found = false;
    for (const std::uint32_t hash : kTutorialItemHashes) {
        build_data::items::Definition item{};
        build_data::pursuits::Definition contract{};
        if (!build_data::find_item_definition_hash(hash, item)) {
            return missing_item();
        }
        if (!build_data::find_pursuit_definition(item.definitionIndex, contract)
            || contract.questStep.membership != build_data::pursuits::QuestMembership::step) {
            continue;
        }
        const build_data::pursuits::QuestStep& step = contract.questStep;
        if (step.scope != build_data::items::QuestInitialization::Scope::account
            || (found && step.row != row)) {
            return RewardPreparation::unresolvable;
        }
        row = step.row;
        found = true;
    }
    return found ? RewardPreparation::prepared : missing_contract();
}

/** @return True when a character holds a tutorial item. */
[[nodiscard]] bool holds_tutorial_item(const AccountState& saved) noexcept {
    const auto tutorial = [](const account::inventory::Item& item) noexcept {
        return item.quantity > 0
               && std::find(kTutorialItemHashes.begin(),
                            kTutorialItemHashes.end(),
                            item.definitionHash)
                      != kTutorialItemHashes.end();
    };
    for (const CharacterState& character :
         std::span(saved.characters).first(saved.characterCount)) {
        const auto held = std::span(character.inventory.values).first(character.inventory.count);
        if (std::any_of(held.begin(), held.end(), tutorial)) {
            return true;
        }
    }
    return false;
}

/** @return True for the cookie a recipe bakes. */
[[nodiscard]] bool recipe_cookie(std::uint32_t cookieHash) noexcept {
    return std::any_of(
        kOvenRecipes.begin(), kOvenRecipes.end(), [cookieHash](const OvenRecipe& recipe) noexcept {
            return recipe.cookieHash == cookieHash;
        });
}

/** @return True when the rule advances this item on the event. */
[[nodiscard]] bool rule_counts(const DawningBountyRule& rule,
                               std::uint32_t itemHash,
                               DawningEvent event,
                               std::uint32_t cookieHash) noexcept {
    return rule.bountyHash == itemHash && rule.event == event
           && (rule.cookieHash == kAnyCookie || rule.cookieHash == cookieHash);
}

/**
 * Advances one objective of a held bounty by one, unless it is finished.
 * @param advanced Set when the objective moved.
 * @return Unresolvable when the published contract does not hold the objective or names no lane
 *         for it; not ready while its objectives are unpublished; deferred when the saved lane is
 *         negative.
 */
[[nodiscard]] RewardPreparation advance_objective(const build_data::pursuits::Definition& contract,
                                                  std::uint32_t objectiveHash,
                                                  account::inventory::ItemObjectives& values,
                                                  bool& advanced) noexcept {
    for (std::uint8_t ordinal = 0; ordinal < contract.objectiveCount; ++ordinal) {
        build_data::objectives::Definition objective{};
        if (!build_data::find_objective_definition(contract.objectives[ordinal], objective)) {
            return missing_contract();
        }
        if (objective.definitionHash != objectiveHash) {
            continue;
        }
        std::size_t lane = 0;
        std::int32_t target = 0;
        if (!objective_lane(contract, ordinal, lane, target)) {
            return missing_contract();
        }
        // Only a later save can make a negative lane count again.
        if (values[lane] < 0) {
            return RewardPreparation::deferred;
        }
        if (values[lane] < target) {
            ++values[lane];
            advanced = true;
        }
        return RewardPreparation::prepared;
    }
    return RewardPreparation::unresolvable;
}

} // namespace

/** Bounties are matched by the authored rules; their lanes come from the installed contract. */
RewardPreparation credit_dawning_bounties(CharacterState& character,
                                          DawningEvent event,
                                          std::uint32_t cookieHash,
                                          std::int64_t nowSeconds) noexcept {
    if (!recipe_cookie(cookieHash)) {
        return RewardPreparation::prepared;
    }
    const auto rules = std::span(kDawningBountyRules);
    for (auto& held : std::span(character.inventory.values).first(character.inventory.count)) {
        const auto counted = [&](const DawningBountyRule& rule) noexcept {
            return rule_counts(rule, held.definitionHash, event, cookieHash);
        };
        if (std::none_of(rules.begin(), rules.end(), counted)) {
            continue;
        }
        build_data::items::Definition item{};
        build_data::pursuits::Definition contract{};
        if (!build_data::find_item_definition_hash(held.definitionHash, item)) {
            return missing_item();
        }
        if (!build_data::find_pursuit_definition(item.definitionIndex, contract)) {
            return missing_contract();
        }
        if (!contract.readable) {
            return RewardPreparation::unresolvable;
        }
        account::inventory::ItemObjectives values =
            held.objectives.value_or(account::inventory::ItemObjectives{});
        if (bounty_expired(contract, values, nowSeconds)) {
            continue;
        }
        bool advanced = false;
        for (const DawningBountyRule& rule : rules) {
            if (!counted(rule)) {
                continue;
            }
            const auto outcome = advance_objective(contract, rule.objectiveHash, values, advanced);
            if (outcome != RewardPreparation::prepared) {
                return outcome;
            }
        }
        if (!advanced) {
            continue;
        }
        held.objectives = values;
        // Exhausted serials are saved State, which only a later save changes.
        if (!next_character_serial(character, held.mutationSerial)) {
            return RewardPreparation::deferred;
        }
    }
    return RewardPreparation::prepared;
}

/** Chooses what an oven plug bakes, its cost and the lanes it resets, or gates the Masterwork. */
OvenAction stage_oven_bake(const item_details::Definition& oven,
                           const account::inventory::Sockets& sockets,
                           std::uint8_t socketLane,
                           const build_data::items::Definition& requested,
                           OvenBake& bake,
                           const char*& reason) noexcept {
    bake = {};
    reason = nullptr;
    if (requested.definitionHash == kMasterworkPlugHash) {
        return all_recipes_discovered(reason) ? OvenAction::none : OvenAction::refused;
    }
    const OvenRecipe* recipe = find_recipe(requested.definitionHash);
    const bool combine = requested.definitionHash == kCombinePlugHash;
    if (recipe == nullptr && !combine) {
        return OvenAction::none;
    }
    if (oven.ordinarySocketCount <= kMasterworkLane) {
        reason = "oven_socket_count";
        return OvenAction::refused;
    }
    if (!reset_lanes(oven, sockets, socketLane, bake, reason)) {
        return OvenAction::refused;
    }
    const bool masterworked = sockets.plugs[kMasterworkLane] == kMasterworkPlugHash;
    if (combine) {
        // A masterworked oven hides its ingredient and Combine lanes.
        if (masterworked) {
            reason = "oven_combine_masterworked";
            return OvenAction::refused;
        }
        return stage_combine(sockets, requested, bake, reason) ? OvenAction::baked
                                                               : OvenAction::refused;
    }
    // A masterworked oven offers only the discounted recipes, and an unmasterworked one only the
    // ordinary ones; either needs the recipe discovered.
    const bool discounted = requested.definitionHash == recipe->masterworkedPlugHash;
    bool discovered = false;
    if (discounted != masterworked) {
        reason = "oven_recipe_variant";
        return OvenAction::refused;
    }
    if (!recipe_discovered(*recipe, bake.unlockWrites, discovered)) {
        reason = "oven_recipe_flag";
        return OvenAction::refused;
    }
    if (!discovered) {
        reason = "oven_recipe_undiscovered";
        return OvenAction::refused;
    }
    if (!find_cookie(recipe->cookieHash, bake)) {
        reason = "oven_cookie";
        return OvenAction::refused;
    }
    bake.costSetIndex = requested.insertionMaterialRequirementSetIndex;
    return OvenAction::baked;
}

/**
 * Pays the cookie into the charged account, advances the bounties its bake counts and sets a newly
 * discovered recipe's flag. A burnt cookie is no recipe's, so it advances no bounty.
 */
bool pay_oven_bake(const AccountState& uncharged,
                   AccountState& account,
                   std::size_t characterIndex,
                   OvenBake& bake,
                   AccountUnlockWrites& writes,
                   std::int64_t nowSeconds) noexcept {
    build_data::items::Definition cookie{};
    if (!credit_profile_stack(uncharged,
                              account,
                              characterIndex,
                              bake.cookieIndex,
                              1,
                              bake.changes,
                              bake.changeCount)
        || !build_data::find_item_definition_index(bake.cookieIndex, cookie)
        || credit_dawning_bounties(account.characters[characterIndex],
                                   DawningEvent::bake,
                                   cookie.definitionHash,
                                   nowSeconds)
               != RewardPreparation::prepared) {
        return false;
    }
    if (bake.discoveryFlagIndex == build_data::items::kUnavailableFlagIndex) {
        return true;
    }
    std::int32_t discovered = 0;
    return read_account_unlock(
               writes, AccountUnlockBank::flags, bake.discoveryFlagIndex, discovered)
           && (discovered == unlocks::kFlagSet
               || write_account_unlock(writes,
                                       AccountUnlockBank::flags,
                                       bake.discoveryFlagIndex,
                                       unlocks::kFlagSet));
}

/** Credits a pickup to the balance its ingredient plug's quantity expression counts. */
RewardPreparation prepare_ingredient_credit(std::uint32_t pickupHash,
                                            std::int32_t quantity,
                                            AccountUnlockWrite& credit) noexcept {
    credit = {};
    const IngredientPickup* pickup = find_pickup(pickupHash);
    if (pickup == nullptr || quantity <= 0) {
        return RewardPreparation::unresolvable;
    }
    build_data::items::Definition plug{};
    if (!build_data::find_item_definition_hash(pickup->ingredientPlugHash, plug)) {
        return missing_item();
    }
    if (plug.quantityValueIndex == build_data::items::kUnavailableValueIndex) {
        return RewardPreparation::unresolvable;
    }
    std::int32_t balance = 0;
    if (!investment::store::read_unlock(
            investment::store::Bank::objectiveValues, plug.quantityValueIndex, balance)) {
        return RewardPreparation::notReady;
    }
    // Spending the ingredient makes room, so a full balance is refused only for now.
    if (balance > (std::numeric_limits<std::int32_t>::max)() - quantity) {
        return RewardPreparation::deferred;
    }
    credit = {AccountUnlockBank::values, plug.quantityValueIndex, balance, balance + quantity};
    return RewardPreparation::prepared;
}

/** Answers from the authored pickup table alone, without reading the balance. */
bool is_ingredient_pickup(std::uint32_t definitionHash) noexcept {
    return find_pickup(definitionHash) != nullptr;
}

/** A tutorial under way keeps its step, so only an unset row is set to ready. */
RewardPreparation
prepare_chooser_readiness(const AccountState& account,
                          std::optional<AccountUnlockWrite>& readiness) noexcept {
    readiness.reset();
    std::uint16_t row = 0;
    const RewardPreparation resolved = tutorial_quest_row(row);
    if (resolved != RewardPreparation::prepared) {
        return resolved;
    }
    std::int32_t saved = 0;
    if (!investment::store::read_unlock(investment::store::Bank::objectiveValues, row, saved)) {
        return RewardPreparation::notReady;
    }
    if (saved == build_data::items::kUnsetQuestValue && !holds_tutorial_item(account)) {
        readiness = AccountUnlockWrite{AccountUnlockBank::values, row, saved, kChooserReadyValue};
    }
    return RewardPreparation::prepared;
}

/** Answers from the authored oven identity alone. */
bool is_dawning_oven(std::uint32_t definitionHash) noexcept {
    return definitionHash == kOvenHash;
}

} // namespace sunrise::state::runtime::detail
