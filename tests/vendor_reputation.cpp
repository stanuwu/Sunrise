#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "core/logging/log.h"
#include "middleware/datagen/family4/loadout/loadout_resolver.h"
#include "state/build_data/runtime.h"
#include "state/build_data/vendors/vendor_catalog.h"
#include "state/investment/store_internal.h"
#include "state/runtime/runtime.h"
#include "state/runtime/vendor_reward_pool.h"

namespace {
namespace state = sunrise::state;
namespace store = state::investment::store;
using Disposition = state::VendorReputationDisposition;

/** Synthetic account identities isolate every check from player saves. */
constexpr std::uint64_t kAccount = 1001, kCharacter = 1002, kOtherCharacter = 1003;
/** Fixture rows are selectors, not installed vendor or item indices. */
constexpr std::uint16_t kVendor = 1, kSale = 2, kSoldIndex = 3, kCostIndex = 4;
/** Build-86657 Banshee identity, reward placeholder and Gunsmith Material identity. */
constexpr std::uint32_t kBanshee = 672118013, kGunsmithRewards = 3831705402,
                        kGunsmithMaterials = 685157383;
/** Build-86657 Zavala identity, reward placeholder and Vanguard token identity. */
constexpr std::uint32_t kZavala = 69482069, kVanguardRewards = 3987308529,
                        kVanguardToken = 3899548068;
/** Native character progression indices from build 86657. */
constexpr std::uint16_t kGunsmithProgression = 55, kVanguardProgression = 62;
/** A five-material turn-in earns 150 XP from Banshee's 30-XP material rate. */
constexpr std::int32_t kCost = 5, kAward = 150, kInitialQuantity = 10;
/** Build-86657 Vanguard tokens award 100 XP each. */
constexpr std::int32_t kVanguardXpPerToken = 100;
/** Nonzero untouched lanes detect accidental replacement of the whole progression row. */
constexpr state::unlocks::ProgressionLanes kInitialProgression{40, 7, 9};
/** A synthetic bucket holds all fixture profile stacks without instanced item sources. */
constexpr std::uint8_t kBucket = 1;
std::uint32_t g_vendorHash = kBanshee, g_soldHash = kGunsmithRewards,
              g_costHash = kGunsmithMaterials;
std::uint32_t g_cost = kCost;
std::uint16_t g_progression = kGunsmithProgression;
bool g_characterScope = true;
bool g_materialAvailable = true;
/** Build-86657 owned faction engrams, separate from the turn-in placeholders. */
constexpr std::uint32_t kVanguardEngram = 3578462974U, kCrucibleEngram = 1368565477U,
                        kGunsmithEngram = 3531414277U;
/** Synthetic selectors isolate package and gear from the material fixture rows. */
constexpr std::uint16_t kRewardIndex = 5, kGearIndexBase = 100;
/** Build-86657 Vanguard package, sale, interaction, category and evaluated gate bindings. */
constexpr std::uint32_t kPackageHash = 2746484552U;
constexpr std::uint16_t kPackageSale = 93, kClaimInteraction = 40, kClaimCategory = 3,
                        kPackageFlag = 5901, kLevelSlot = 465;
constexpr std::int32_t kMinimumLevel = 20;
/** Build-86657 Gunsmith reward counter and normal claim interaction. */
constexpr std::uint16_t kGunsmithCreditRow = 49, kBansheeClaimInteraction = 35;

/** @return Flat fixture pool in weapon, Titan, Hunter, Warlock order. */
std::uint32_t gear_hash(std::size_t index) {
    for (const auto pool :
         {std::span<const std::uint32_t>(state::vendor_rewards::kVanguardWeapons),
          std::span<const std::uint32_t>(state::vendor_rewards::kVanguardTitan),
          std::span<const std::uint32_t>(state::vendor_rewards::kVanguardHunter),
          std::span<const std::uint32_t>(state::vendor_rewards::kVanguardWarlock)}) {
        if (index < pool.size()) {
            return pool[index];
        }
        index -= pool.size();
    }
    return 0;
}
/** Build-86657 Gunsmith's repeating rank costs 3000 XP. */
constexpr std::int32_t kGunsmithRankCost = 3000;
/** Build-86657 engram bucket has ten character inventory slots. */
constexpr std::size_t kEngramCapacity = 10;
std::size_t g_rewardCapacity = kEngramCapacity;
bool g_rewardAvailable = true;

/** @param passed Check result. @param label Failure description. */
void check(bool passed, const char* label) {
    if (!passed) {
        std::fprintf(stderr, "FAIL: %s\n", label);
        std::abort();
    }
}

/**
 * Reads repository SQL resources, never a player database.
 * @param path Resource path.
 * @return File contents.
 */
std::string read_text(const std::string& path) {
    std::FILE* file = nullptr;
    check(fopen_s(&file, path.c_str(), "rb") == 0 && file != nullptr, "open SQL resource");
    std::string text;
    // SQL resource reads use a fixed 4-KiB buffer.
    std::array<char, 4096> buffer{};
    while (const auto count = std::fread(buffer.data(), 1, buffer.size(), file)) {
        text.append(buffer.data(), count);
    }
    check(std::ferror(file) == 0, "read SQL resource");
    std::fclose(file);
    return text;
}

/** Resets only the disposable fixture and its controlled content catalogue. */
void reset() {
    g_vendorHash = kBanshee;
    g_soldHash = kGunsmithRewards;
    g_costHash = kGunsmithMaterials;
    g_cost = kCost;
    g_progression = kGunsmithProgression;
    g_characterScope = true;
    g_materialAvailable = true;
    g_rewardCapacity = kEngramCapacity;
    g_rewardAvailable = true;
    state::AccountState account{};
    account.primarySoid = kAccount;
    account.characterCount = 2;
    account.characters[0].soid = kCharacter;
    account.characters[0].selected = true;
    account.characters[1].soid = kOtherCharacter;
    account.profileItemCount = 1;
    account.profileItems[0] = {0, kGunsmithMaterials, kInitialQuantity, 1, true};
    check(store::read_settings(account.settings) && store::write_account(account), "seed account");
    check(store::execute("DELETE FROM unlocks"), "clear fixture banks");
    state::unlocks::Table banks{};
    banks.characterProgressions[kGunsmithProgression] = kInitialProgression;
    check(store::write_unlocks(banks, 0), "seed progression");
}

/** @return Character-zero Gunsmith lanes from the disposable database. */
state::unlocks::ProgressionLanes gunsmith() {
    state::unlocks::Table banks{};
    check(store::read_unlocks(banks, 0), "read progression");
    return banks.characterProgressions[kGunsmithProgression];
}

/** Covers successful, stale, insufficient, overflow and rolled-back turn-ins. */
void verify() {
    state::PendingVendorReputation pending{};
    reset();
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared,
          "prepare Banshee");
    check(store::account().profileItems[0].quantity == kInitialQuantity
              && gunsmith() == kInitialProgression,
          "prepare writes nothing");
    auto duplicate = pending;
    check(state::commit_vendor_reputation(pending) && !pending.prepared, "commit and consume");
    const auto expected = state::unlocks::ProgressionLanes{
        kInitialProgression[0] + kAward, kInitialProgression[1], kInitialProgression[2]};
    check(gunsmith() == expected
              && store::account().profileItems[0].quantity == kInitialQuantity - kCost,
          "exact cost and XP, other lanes preserved");
    check(!state::commit_vendor_reputation(pending) && !state::commit_vendor_reputation(duplicate),
          "reused and duplicate plans refused");
    state::unlocks::Table other{};
    check(store::read_unlocks(other, 1)
              && other.characterProgressions[kGunsmithProgression]
                     == state::unlocks::ProgressionLanes{},
          "other character untouched");
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared
              && state::commit_vendor_reputation(pending) && store::account().profileItemCount == 0,
          "exact balance removes empty stack");
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::refused,
          "empty inventory refused");

    reset();
    auto split = store::account();
    split.profileItemCount = 2;
    split.profileItems[0].quantity = kCost - 1;
    split.profileItems[1] = {0, kGunsmithMaterials, kInitialQuantity, 2, true};
    check(store::write_account(split), "seed split material stacks");
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared
              && state::commit_vendor_reputation(pending),
          "charge across material stacks");
    check(store::account().profileItemCount == 1
              && store::account().profileItems[0].quantity == kInitialQuantity - 1
              && store::account().profileItems[0].mutationSerial == 2,
          "dense remainder preserves its ordering serial");

    reset();
    g_cost = kInitialQuantity + 1;
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::refused
              && gunsmith() == kInitialProgression
              && store::account().profileItems[0].quantity == kInitialQuantity,
          "insufficient materials leave both sides unchanged");
    g_cost = (std::numeric_limits<std::uint32_t>::max)();
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::refused,
          "award multiplication overflow refused");
    reset();
    check(store::write_unlock(store::Bank::characterProgressions,
                              kGunsmithProgression,
                              (std::numeric_limits<std::int32_t>::max)()),
          "seed overflow");
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::refused,
          "XP total overflow refused");

    reset();
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared,
          "prepare stale inventory");
    auto account = store::account();
    --account.profileItems[0].quantity;
    check(store::write_account(account) && !state::commit_vendor_reputation(pending)
              && gunsmith() == kInitialProgression,
          "stale inventory refused");
    reset();
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared,
          "prepare stale character");
    account = store::account();
    account.characters[0].selected = false;
    account.characters[1].selected = true;
    check(store::write_account(account) && !state::commit_vendor_reputation(pending),
          "changed selection refused");
    reset();
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared,
          "prepare stale XP");
    check(store::write_unlock(store::Bank::characterProgressions, kGunsmithProgression, 0)
              && !state::commit_vendor_reputation(pending),
          "changed XP refused");
    reset();
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared,
          "prepare stale cost");
    ++g_cost;
    check(!state::commit_vendor_reputation(pending), "changed sale cost refused");
    reset();
    g_cost = kInitialQuantity;
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared,
          "prepare whole-stack charge");
    g_materialAvailable = false;
    check(!state::commit_vendor_reputation(pending)
              && store::account().profileItems[0].quantity == kInitialQuantity,
          "missing material definition cannot be hidden by removing its stack");

    reset();
    {
        store::Transaction outer;
        check(outer.ready()
                  && state::prepare_vendor_reputation(kVendor, kSale, pending)
                         == Disposition::prepared
                  && state::commit_vendor_reputation(pending),
              "stage inside response transaction");
    }
    check(gunsmith() == kInitialProgression
              && store::account().profileItems[0].quantity == kInitialQuantity,
          "abandoned response transaction rolls back both writes");
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared,
          "prepare SQL failure");
    check(store::execute("CREATE TEMP TRIGGER reject_xp BEFORE INSERT ON unlocks "
                         "BEGIN SELECT RAISE(ABORT,'fixture'); END"),
          "inject XP write failure");
    check(!state::commit_vendor_reputation(pending)
              && store::account().profileItems[0].quantity == kInitialQuantity
              && gunsmith() == kInitialProgression,
          "failed XP write rolls back material debit");
    check(store::execute("DROP TRIGGER reject_xp"), "remove fixture fault");

    reset();
    g_soldHash = kVanguardRewards;
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::notApplicable,
          "ordinary purchase using the same material does not award XP");
    g_soldHash = kGunsmithRewards;
    g_costHash = kVanguardToken;
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::refused,
          "known placeholder with wrong payment cannot fall through");
    reset();
    g_characterScope = false;
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::refused,
          "wrong progression scope refused");

    reset();
    g_vendorHash = kZavala;
    g_soldHash = kVanguardRewards;
    g_costHash = kVanguardToken;
    g_progression = kVanguardProgression;
    account = store::account();
    account.profileItems[0].definitionHash = kVanguardToken;
    check(store::write_account(account), "seed Vanguard tokens");
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared
              && state::commit_vendor_reputation(pending),
          "shared transaction handles Zavala");
    check(store::read_unlocks(other, 0)
              && other.characterProgressions[kVanguardProgression][0]
                     == kCost * kVanguardXpPerToken,
          "Vanguard token rate, not Gunsmith rate");
}

/** @return One selected-character claim counter from the disposable database. */
std::int32_t credits(std::uint16_t row) {
    std::int32_t value = 0;
    check(store::read_unlock(store::Bank::characterObjectValues, row, value), "read claim credits");
    return value;
}

/** Covers exact thresholds, multiple ranks, no backfill, overflow and transaction rollback. */
void verify_rank_rewards() {
    /** Build-86657 vendor, progression and saved claim-counter mappings. */
    struct RankCase {
        std::uint32_t vendor, placeholder, material;
        std::uint16_t progression, rewardRow;
        std::int32_t rankCost, rate;
    };
    constexpr std::array<RankCase, 3> cases{{
        {kBanshee, kGunsmithRewards, kGunsmithMaterials, 55, 49, 3000, 30},
        {kZavala, kVanguardRewards, kVanguardToken, 62, 55, 2000, 100},
        {3603221665U, 265113466U, 183980811U, 49, 45, 2000, 100},
    }};
    state::PendingVendorReputation pending{};
    for (const auto& entry : cases) {
        reset();
        g_vendorHash = entry.vendor;
        g_soldHash = entry.placeholder;
        g_costHash = entry.material;
        g_progression = entry.progression;
        auto account = store::account();
        account.profileItems[0].definitionHash = entry.material;
        check(store::write_account(account)
                  && store::write_unlock(store::Bank::characterProgressions,
                                         entry.progression,
                                         entry.rankCost - kCost * entry.rate),
              "seed exact-threshold turn-in");
        g_rewardCapacity = 0;
        g_rewardAvailable = false;
        check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared
                  && credits(entry.rewardRow) == 0,
              "credit preparation needs no inventory capacity or reward item");
        auto duplicate = pending;
        check(state::commit_vendor_reputation(pending) && credits(entry.rewardRow) == 1
                  && store::account().characters[0].inventory.count == 0,
              "one credit, no owned engram");
        check(!state::commit_vendor_reputation(duplicate) && credits(entry.rewardRow) == 1,
              "duplicate cannot credit again");
        check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared
                  && state::commit_vendor_reputation(pending) && credits(entry.rewardRow) == 1,
              "below next threshold adds no credit");
        state::unlocks::Table other{};
        check(store::read_unlocks(other, 1) && other.characterObjectValues[entry.rewardRow] == 0,
              "other character receives no credit");
    }
    reset();
    check(store::write_unlock(
              store::Bank::characterProgressions, kGunsmithProgression, kGunsmithRankCost - kAward),
          "seed crossing");
    {
        store::Transaction outer;
        check(outer.ready()
                  && state::prepare_vendor_reputation(kVendor, kSale, pending)
                         == Disposition::prepared
                  && state::commit_vendor_reputation(pending),
              "stage credit inside response");
    }
    check(credits(kGunsmithCreditRow) == 0
              && store::account().profileItems[0].quantity == kInitialQuantity
              && gunsmith()[0] == kGunsmithRankCost - kAward,
          "abandoned response rolls back credit, XP and payment");
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared
              && store::execute("CREATE TEMP TRIGGER reject_rank BEFORE INSERT ON unlocks "
                                "BEGIN SELECT RAISE(ABORT,'fixture'); END"),
          "inject unlock write failure");
    check(!state::commit_vendor_reputation(pending) && credits(kGunsmithCreditRow) == 0
              && store::account().profileItems[0].quantity == kInitialQuantity,
          "failed unlock write rolls back payment");
    check(store::execute("DROP TRIGGER reject_rank"), "remove rank fault");
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared
              && store::write_unlock(store::Bank::characterObjectValues, kGunsmithCreditRow, 1)
              && !state::commit_vendor_reputation(pending),
          "concurrent credit change refuses stale turn-in");
    check(store::write_unlock(store::Bank::characterObjectValues,
                              kGunsmithCreditRow,
                              (std::numeric_limits<std::int32_t>::max)())
              && state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::refused,
          "credit overflow refused");
    check(store::write_unlock(store::Bank::characterObjectValues, kGunsmithCreditRow, -1)
              && state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::refused,
          "negative credit refused");
    reset();
    check(store::write_unlock(
              store::Bank::characterProgressions, kGunsmithProgression, kGunsmithRankCost * 2)
              && state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared
              && state::commit_vendor_reputation(pending) && credits(kGunsmithCreditRow) == 0,
          "historical ranks never backfilled");
    reset();
    auto account = store::account();
    // Two complete Gunsmith ranks in one payment exercise multi-credit accounting.
    g_cost = static_cast<std::uint32_t>(kGunsmithRankCost * 2 / 30);
    account.profileItems[0].quantity = static_cast<std::int32_t>(g_cost);
    check(store::write_account(account)
              && state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared
              && state::commit_vendor_reputation(pending) && credits(kGunsmithCreditRow) == 2
              && store::account().characters[0].inventory.count == 0,
          "two thresholds award two credits, no items");
}

/** Configures a disposable eligible Zavala claim without changing any player save. */
void reset_claim() {
    reset();
    g_vendorHash = kZavala;
    state::Family5State family{};
    family.flagCount = 1;
    family.flags[0] = {kPackageFlag, state::unlocks::kFlagSet};
    family.valueCount = 1;
    family.values[0] = {kLevelSlot, kMinimumLevel};
    check(store::write_family5(family)
              && store::write_unlock(
                  store::Bank::characterObjectValues, state::kVanguardRewardValueRow, 2),
          "seed claim fixture");
}

/** Covers claim publication, exact debit, refusal, class filtering and atomic persistence. */
void verify_claims() {
    state::PendingItemAcquisition grant{};
    reset_claim();
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
              == Disposition::prepared,
          "prepare claim");
    auto duplicate = grant;
    state::AccountState after{};
    state::unlocks::Table banks{};
    check(state::preview_item_acquisition(grant, after, banks)
              && after.characters[0].inventory.count == 1
              && banks.characterObjectValues[state::kVanguardRewardValueRow] == 1
              && credits(state::kVanguardRewardValueRow) == 2
              && store::account().characters[0].inventory.count == 0,
          "preview publishes item and decremented credit without saving");
    check(state::commit_item_acquisition(grant) && credits(state::kVanguardRewardValueRow) == 1
              && store::account().characters[0].inventory.count == 1
              && store::account().characters[0].inventory.values[0].definitionHash
                     == state::vendor_rewards::kVanguardWeapons.front(),
          "one item and one credit commit together");
    check(!state::commit_item_acquisition(duplicate) && !state::commit_item_acquisition(grant),
          "duplicate and reused claims refused");
    check(store::read_unlocks(banks, 1)
              && banks.characterObjectValues[state::kVanguardRewardValueRow] == 0
              && store::account().characters[1].inventory.count == 0,
          "other character unchanged");
    reset_claim();
    {
        store::Transaction outer;
        check(outer.ready()
                  && state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
                         == Disposition::prepared
                  && state::commit_item_acquisition(grant),
              "claim inside outer response");
    }
    check(credits(state::kVanguardRewardValueRow) == 2
              && store::account().characters[0].inventory.count == 0,
          "response failure rolls back item and credit");
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
                  == Disposition::prepared
              && store::execute("CREATE TEMP TRIGGER reject_claim BEFORE INSERT ON unlocks "
                                "BEGIN SELECT RAISE(ABORT,'fixture'); END"),
          "inject claim write failure");
    check(!state::commit_item_acquisition(grant) && credits(state::kVanguardRewardValueRow) == 2
              && store::account().characters[0].inventory.count == 0,
          "credit write failure rolls back item");
    check(store::execute("DROP TRIGGER reject_claim"), "remove claim fault");
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 1, 0, grant)
              == Disposition::refused,
          "wrong reply refused");
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction + 1, 0, 0, grant)
              == Disposition::notApplicable,
          "other interactions left alone");
    check(state::is_vendor_reward_category(kVendor, kClaimCategory)
              && !state::is_vendor_reward_category(kVendor, kClaimCategory + 1),
          "reward category blocks sale bypass only");
    g_vendorHash = kBanshee;
    check(state::prepare_vendor_reward(kVendor, kBansheeClaimInteraction, 0, 0, grant)
              == Disposition::refused,
          "unsupported faction payout refused");

    reset_claim();
    g_rewardCapacity = 0;
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
                  == Disposition::refused
              && credits(state::kVanguardRewardValueRow) == 2,
          "full inventory preserves credit");
    g_rewardCapacity = kEngramCapacity;
    g_rewardAvailable = false;
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
              == Disposition::refused,
          "missing gear refuses payout");
    reset_claim();
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
                  == Disposition::prepared
              && store::write_unlock(
                  store::Bank::characterObjectValues, state::kVanguardRewardValueRow, 1)
              && !state::commit_item_acquisition(grant),
          "stale credit refuses claim");
    check(store::write_unlock(store::Bank::characterObjectValues, state::kVanguardRewardValueRow, 0)
              && state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
                     == Disposition::refused,
          "no credit refuses claim");
    reset_claim();
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
              == Disposition::prepared,
          "prepare before gate change");
    check(store::write_family5({}) && !state::commit_item_acquisition(grant)
              && credits(state::kVanguardRewardValueRow) == 2,
          "changed or absent package gates cannot consume credit");
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
              == Disposition::refused,
          "missing evaluated gates refused");

    reset_claim();
    state::Family5State gates{};
    check(store::read_family5(gates), "read fixture gates");
    gates.values[0].value = kMinimumLevel - 1;
    check(store::write_family5(gates)
              && state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
                     == Disposition::refused,
          "low-level claim refused");
    reset_claim();
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
              == Disposition::prepared,
          "prepare before capacity loss");
    g_rewardCapacity = 0;
    check(!state::commit_item_acquisition(grant) && credits(state::kVanguardRewardValueRow) == 2,
          "capacity lost after prepare preserves credit");
    reset_claim();
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
              == Disposition::prepared,
          "prepare before character change");
    auto changed = store::account();
    changed.characters[0].selected = false;
    changed.characters[1].selected = true;
    check(store::write_account(changed) && !state::commit_item_acquisition(grant),
          "changed selected character refuses claim");

    reset_claim();
    auto existing = store::account();
    auto& inventory = existing.characters[0].inventory;
    inventory.count = 1;
    inventory.values[0].definitionHash = kVanguardEngram;
    inventory.values[0].instanceSoid = kAccount + kCharacter;
    inventory.values[0].quantity = 1;
    const auto savedSoid = inventory.values[0].instanceSoid;
    check(store::write_account(existing)
              && state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, grant)
                     == Disposition::prepared
              && state::commit_item_acquisition(grant),
          "claim alongside a previously saved experimental engram");
    const auto saved = store::account();
    check(saved.characters[0].inventory.count == 2
              && saved.characters[0].inventory.values[0].definitionHash == kVanguardEngram
              && saved.characters[0].inventory.values[0].instanceSoid == savedSoid,
          "existing experimental item preserved without conversion");

    for (auto characterClass : {state::CharacterClass::titan,
                                state::CharacterClass::hunter,
                                state::CharacterClass::warlock}) {
        reset_claim();
        auto account = store::account();
        account.characters[0].characterClass = characterClass;
        check(store::write_account(account), "set fixture class");
        const auto armour = state::vendor_rewards::armour(characterClass);
        for (std::size_t slot = 0; slot < armour.size(); ++slot) {
            const auto roll =
                static_cast<std::uint32_t>(state::vendor_rewards::kVanguardWeapons.size() + slot);
            check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, roll, grant)
                          == Disposition::prepared
                      && grant.acquiredDefinitionHash == armour[slot],
                  "armour selection matches class");
        }
    }
}

} // namespace

namespace sunrise::core::log {
/** Logging is not an assertion; state checks use the real store and transaction helpers. */
void write(Channel, Level, std::string_view) noexcept {}
} // namespace sunrise::core::log

namespace sunrise::state::build_data::vendors {
/** Catalogue doubles expose one controlled vendor and sale, not runtime inventory. */
bool find_index(std::uint16_t index, IndexEntry& entry) noexcept {
    entry = {};
    entry.definitionHash = g_vendorHash;
    return index == kVendor;
}
bool find(std::uint32_t hash, Definition& definition) noexcept {
    definition = {};
    return hash == g_vendorHash;
}
bool sale_row(const Definition&, std::size_t row, SaleRow& output) noexcept {
    output = {};
    output.itemIndex = kSoldIndex;
    output.costItemIndex = kCostIndex;
    output.costQuantity = g_cost;
    if (row == kPackageSale) {
        output.itemIndex = kRewardIndex;
        output.categoryIndex = kClaimCategory;
        output.costQuantity = 0;
        output.costItemIndex = kAbsentCostItem;
        return true;
    }
    return row == kSale;
}
} // namespace sunrise::state::build_data::vendors

namespace sunrise::state::build_data {
/** Season bundles are not part of the faction grant under test. */
bool find_season_pass_package(std::uint32_t, season_pass::Package&) noexcept {
    return false;
}
/** Unrelated acquisition code shares the profile helper translation unit. */
bool find_collectible_definition(std::uint16_t, collectibles::Definition&) noexcept {
    return false;
}
/** Item catalogue doubles retain production profile-shape validation. */
bool find_item_definition_index(std::uint16_t definitionIndex,
                                items::Definition& definition) noexcept {
    definition = {};
    definition.definitionIndex = definitionIndex;
    definition.definitionHash = definitionIndex == kSoldIndex ? g_soldHash : g_costHash;
    definition.bucketId = kBucket;
    if (definitionIndex == kRewardIndex) {
        definition.definitionHash = kPackageHash;
        return true;
    }
    if (definitionIndex >= kGearIndexBase) {
        definition.definitionHash = gear_hash(definitionIndex - kGearIndexBase);
        return g_rewardAvailable && definition.definitionHash != 0;
    }
    return definitionIndex == kSoldIndex || definitionIndex == kCostIndex;
}
bool find_item_definition_hash(std::uint32_t definitionHash,
                               items::Definition& definition) noexcept {
    if (definitionHash == kVanguardEngram || definitionHash == kCrucibleEngram
        || definitionHash == kGunsmithEngram) {
        definition = {};
        definition.definitionHash = definitionHash;
        definition.definitionIndex = kRewardIndex;
        return g_rewardAvailable;
    }
    for (std::size_t index = 0; gear_hash(index) != 0; ++index) {
        if (definitionHash == gear_hash(index)) {
            return find_item_definition_index(static_cast<std::uint16_t>(kGearIndexBase + index),
                                              definition);
        }
    }
    return definitionHash == g_costHash && find_item_definition_index(kCostIndex, definition);
}
bool find_configured_item_detail(std::uint16_t definitionIndex,
                                 items::details::Definition& definition) noexcept {
    definition = {};
    definition.definitionIndex = definitionIndex;
    definition.definitionHash = g_costHash;
    definition.bucketId = kBucket;
    definition.instancedDefinitionState = items::details::InstancedDefinitionState::stackable;
    if (definitionIndex >= kGearIndexBase) {
        definition.definitionHash = gear_hash(definitionIndex - kGearIndexBase);
        definition.equipmentSlot = 0;
        definition.instancedDefinitionState = items::details::InstancedDefinitionState::instanced;
        return g_rewardAvailable && definition.definitionHash != 0;
    }
    return g_materialAvailable && definitionIndex == kCostIndex;
}
bool find_inventory_bucket_descriptor(std::uint8_t bucketId,
                                      inventory::buckets::Descriptor& descriptor) noexcept {
    descriptor = {};
    descriptor.arraySelector = inventory::buckets::ArraySelector::profile;
    descriptor.slotCount = inventory::buckets::kProfileSlotCapacity;
    return bucketId == kBucket;
}
bool is_profile_action_source(std::uint16_t, std::uint8_t) noexcept {
    return false;
}
/**
 * Exposes only the fixture's selected progression in the requested object scope.
 * @param scope Requested object scope.
 * @param output Receives the one fixture index.
 * @param count Receives one on success, zero on failure.
 * @return False for missing storage or a scope the fixture does not publish.
 */
bool find_progression_slots(progressions::Scope scope,
                            std::span<std::uint16_t> output,
                            std::size_t& count) noexcept {
    count = 0;
    if (scope != progressions::Scope::character || !g_characterScope || output.empty()) {
        return false;
    }
    output[0] = g_progression;
    count = 1;
    return true;
}
} // namespace sunrise::state::build_data

namespace sunrise::state {
/** Unrelated profile exchange code reads the same disposable store. */
AccountState account_snapshot() noexcept {
    return investment::store::account();
}
} // namespace sunrise::state

namespace sunrise::middleware::datagen::family4::loadout {
/**
 * Models bucket capacity without installed game content; acquisition and persistence are real.
 * @param account Candidate account.
 * @param selectedCharacterIndex Character whose items must fit.
 * @param output Receives synthetic inventory positions.
 * @return False when the fixture bucket has insufficient space.
 */
bool resolve(const state::AccountState& account,
             std::size_t selectedCharacterIndex,
             ResolvedLoadout& output) noexcept {
    output = {};
    const auto& inventory = account.characters[selectedCharacterIndex].inventory;
    if (inventory.count > g_rewardCapacity) {
        return false;
    }
    for (std::size_t index = 0; index < inventory.count; ++index) {
        auto& item = output.items[output.itemCount++];
        item.inventoryRow = static_cast<std::uint16_t>(index);
        item.instance.instanceSoid = inventory.values[index].instanceSoid;
    }
    return true;
}
} // namespace sunrise::middleware::datagen::family4::loadout

/**
 * Runs against disposable SQLite only, then checks persistence by closing and reopening it.
 * @param argc Argument count.
 * @param argv Executable, repository SQL directory and new disposable database path.
 * @return Zero when every check passes; failed checks abort.
 */
int main(int argc, char** argv) {
    check(argc == 3, "resource directory and disposable database arguments");
    const std::string root = argv[1];
    check(store::open(":memory:",
                      read_text(root + "/investment_schema.sql"),
                      "INSERT INTO account VALUES (1,1001,0);",
                      read_text(root + "/account_settings_schema.sql"),
                      read_text(root + "/account_settings_defaults.sql")),
          "open disposable store");
    verify();
    verify_rank_rewards();
    verify_claims();
    store::shutdown();
    check(store::open(argv[2],
                      read_text(root + "/investment_schema.sql"),
                      "INSERT INTO account VALUES (1,1001,0);",
                      read_text(root + "/account_settings_schema.sql"),
                      read_text(root + "/account_settings_defaults.sql")),
          "open disposable disk store");
    reset();
    check(store::write_unlock(
              store::Bank::characterProgressions, kGunsmithProgression, kGunsmithRankCost - kAward),
          "seed persistent rank crossing");
    state::PendingVendorReputation pending{};
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared
              && state::commit_vendor_reputation(pending),
          "commit before closing store");
    store::shutdown();
    check(store::open(argv[2], {}, {}, {}, {}), "reopen saved store without seeding");
    check(gunsmith()[0] == kGunsmithRankCost
              && store::account().profileItems[0].quantity == kInitialQuantity - kCost
              && store::account().characters[0].inventory.count == 0,
          "material debit and XP survive reopening database");
    state::unlocks::Table reopened{};
    check(store::read_unlocks(reopened, 0)
              && reopened.characterObjectValues[kGunsmithCreditRow] == 1,
          "claim credit survives reopening database");
    reset_claim();
    state::PendingItemAcquisition claim{};
    check(state::prepare_vendor_reward(kVendor, kClaimInteraction, 0, 0, claim)
                  == Disposition::prepared
              && state::commit_item_acquisition(claim),
          "commit persistent claim");
    store::shutdown();
    check(store::open(argv[2], {}, {}, {}, {}), "reopen claimed save");
    check(store::read_unlocks(reopened, 0)
              && reopened.characterObjectValues[state::kVanguardRewardValueRow] == 1
              && store::account().characters[0].inventory.count == 1,
          "claim debit and item persist together");
    store::shutdown();
    std::puts("PASS: reputation transaction, exact debit/credit, refusal, staleness and rollback");
}
