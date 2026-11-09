/** Dawning oven bakes: a Combine or recipe plug charges a recipe and pays its cookie. */

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>

#include "../build_data/runtime.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state::runtime::detail {

namespace {

namespace item_details = build_data::items::details;
namespace material_requirements = build_data::material_requirements;

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

// TODO: extract what an oven bake pays; no extractor relates a recipe plug to its cookie or its
// masterworked plug, an unmatched Combine to its cookie, or a pickup to the plug it fills.

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

// The bake's charge and its own writes must fit the transaction's unlock writes together.
static_assert(kSocketUnlockWriteCapacity
              == material_requirements::kRequirementCapacity + kIngredientLanes.size());

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
        || !read_account_unlock(writes, true, plug.plugSetFlagIndex, flag)) {
        return false;
    }
    discovered = flag == unlocks::kFlagSet;
    return true;
}

/** @return True when every recipe is discovered, which the Masterwork plug requires. */
[[nodiscard]] bool all_recipes_discovered() noexcept {
    const AccountUnlockWrites unstaged{};
    for (const OvenRecipe& recipe : kOvenRecipes) {
        bool discovered = false;
        if (!recipe_discovered(recipe, unstaged, discovered) || !discovered) {
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
    material_requirements::Definition cost{};
    if (!build_data::find_material_requirement_set(
            recipePlug.insertionMaterialRequirementSetIndex, cost)) {
        return false;
    }
    std::size_t count = 0;
    for (const auto& requirement : std::span(cost.requirements).first(cost.requirementCount)) {
        if (requirement.condition == material_requirements::kUnconditionalRequirement) {
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
 * @return False when a lane holds no ingredient, a balance does not cover it, or no recipe
 * resolves.
 */
[[nodiscard]] bool stage_combine(const account::inventory::Sockets& sockets,
                                 const build_data::items::Definition& combine,
                                 OvenBake& bake) noexcept {
    std::array<build_data::items::Definition, kIngredientLanes.size()> ingredients{};
    for (std::size_t index = 0; index < ingredients.size(); ++index) {
        const auto& plug = sockets.plugs[kIngredientLanes[index]];
        if (!plug.has_value()
            || !build_data::find_item_definition_hash(*plug, ingredients[index])) {
            return false;
        }
    }
    const OvenRecipe* recipe = nullptr;
    build_data::items::Definition recipePlug{};
    if (!match_recipe(
            ingredients[0].definitionIndex, ingredients[1].definitionIndex, recipe, recipePlug)) {
        return false;
    }
    if (recipe != nullptr) {
        bake.costSetIndex = recipePlug.insertionMaterialRequirementSetIndex;
        bake.discoveryFlagIndex = recipePlug.plugSetFlagIndex;
        return bake.discoveryFlagIndex != build_data::items::kUnavailableFlagIndex
               && find_cookie(recipe->cookieHash, bake);
    }
    // Each ingredient plug counts its own balance, which is the one a burnt bake spends.
    for (const auto& ingredient : ingredients) {
        if (ingredient.quantityValueIndex == build_data::items::kUnavailableValueIndex
            || !debit_account_value(
                bake.unlockWrites, ingredient.quantityValueIndex, kBurntIngredientCost)) {
            return false;
        }
    }
    bake.costSetIndex = combine.insertionMaterialRequirementSetIndex;
    return find_cookie(kBurntCookieHash, bake);
}

/**
 * Returns every lane before the Masterwork lane to its initial plug.
 * @return False when a lane has no initial plug or the requested lane is not reset.
 */
[[nodiscard]] bool reset_lanes(const item_details::Definition& oven,
                               const account::inventory::Sockets& sockets,
                               std::uint8_t socketLane,
                               OvenBake& bake) noexcept {
    bake.sockets = sockets;
    for (std::uint8_t lane = 0; lane < kMasterworkLane; ++lane) {
        const std::uint16_t initialPlugIndex = oven.initialPlugIndices[lane];
        build_data::items::Definition plug{};
        if (initialPlugIndex == item_details::kUnavailableItemIndex
            || !build_data::find_item_definition_index(initialPlugIndex, plug)) {
            return false;
        }
        bake.sockets.plugs[lane] = plug.definitionHash;
        if (lane == socketLane) {
            bake.lanePlug = plug;
        }
    }
    return socketLane < kMasterworkLane;
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

} // namespace

/** Chooses what an oven plug bakes, its cost and the lanes it resets, or gates the Masterwork. */
OvenAction stage_oven_bake(const item_details::Definition& oven,
                           const account::inventory::Sockets& sockets,
                           std::uint8_t socketLane,
                           const build_data::items::Definition& requested,
                           OvenBake& bake) noexcept {
    bake = {};
    if (requested.definitionHash == kMasterworkPlugHash) {
        return all_recipes_discovered() ? OvenAction::none : OvenAction::refused;
    }
    const OvenRecipe* recipe = find_recipe(requested.definitionHash);
    const bool combine = requested.definitionHash == kCombinePlugHash;
    if (recipe == nullptr && !combine) {
        return OvenAction::none;
    }
    if (oven.ordinarySocketCount <= kMasterworkLane
        || !reset_lanes(oven, sockets, socketLane, bake)) {
        return OvenAction::refused;
    }
    const bool masterworked = sockets.plugs[kMasterworkLane] == kMasterworkPlugHash;
    if (combine) {
        // A masterworked oven hides its ingredient and Combine lanes.
        return !masterworked && stage_combine(sockets, requested, bake) ? OvenAction::baked
                                                                         : OvenAction::refused;
    }
    // A masterworked oven offers only the discounted recipes, and an unmasterworked one only the
    // ordinary ones; either needs the recipe discovered.
    const bool discounted = requested.definitionHash == recipe->masterworkedPlugHash;
    bool discovered = false;
    if (discounted != masterworked || !recipe_discovered(*recipe, bake.unlockWrites, discovered)
        || !discovered || !find_cookie(recipe->cookieHash, bake)) {
        return OvenAction::refused;
    }
    bake.costSetIndex = requested.insertionMaterialRequirementSetIndex;
    return OvenAction::baked;
}

/** Pays the cookie into the charged account and sets a newly discovered recipe's flag. */
bool pay_oven_bake(AccountState& account,
                   std::size_t characterIndex,
                   OvenBake& bake,
                   AccountUnlockWrites& writes) noexcept {
    if (!credit_profile_stack(
            account, characterIndex, bake.cookieIndex, 1, bake.changes, bake.changeCount)) {
        return false;
    }
    if (bake.discoveryFlagIndex == build_data::items::kUnavailableFlagIndex) {
        return true;
    }
    std::int32_t discovered = 0;
    return read_account_unlock(writes, true, bake.discoveryFlagIndex, discovered)
           && (discovered == unlocks::kFlagSet
               || write_account_unlock(writes, true, bake.discoveryFlagIndex, unlocks::kFlagSet));
}

} // namespace sunrise::state::runtime::detail

namespace sunrise::state {

/** Credits a pickup to the balance its ingredient plug's quantity expression counts. */
bool prepare_ingredient_credit(std::uint32_t pickupHash,
                               std::int32_t quantity,
                               AccountUnlockWrite& credit) noexcept {
    credit = {};
    const runtime::detail::IngredientPickup* pickup = runtime::detail::find_pickup(pickupHash);
    build_data::items::Definition plug{};
    std::int32_t balance = 0;
    if (pickup == nullptr || quantity <= 0
        || !build_data::find_item_definition_hash(pickup->ingredientPlugHash, plug)
        || plug.quantityValueIndex == build_data::items::kUnavailableValueIndex
        || !investment::store::read_unlock(
            investment::store::Bank::objectiveValues, plug.quantityValueIndex, balance)
        || balance > (std::numeric_limits<std::int32_t>::max)() - quantity) {
        return false;
    }
    credit = {false, plug.quantityValueIndex, balance, balance + quantity};
    return true;
}

} // namespace sunrise::state
