#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>

#include "state/build_data/runtime.h"
#include "state/build_data/vendors/vendor_catalog.h"
#include "state/investment/store_internal.h"
#include "state/runtime/state_vendor_reputation_runtime.h"

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
} // namespace

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
    return row == kSale;
}
} // namespace sunrise::state::build_data::vendors

namespace sunrise::state::build_data {
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
    return definitionIndex == kSoldIndex || definitionIndex == kCostIndex;
}
bool find_item_definition_hash(std::uint32_t definitionHash,
                               items::Definition& definition) noexcept {
    return definitionHash == g_costHash && find_item_definition_index(kCostIndex, definition);
}
bool find_configured_item_detail(std::uint16_t definitionIndex,
                                 items::details::Definition& definition) noexcept {
    definition = {};
    definition.definitionIndex = definitionIndex;
    definition.definitionHash = g_costHash;
    definition.bucketId = kBucket;
    definition.instancedDefinitionState = items::details::InstancedDefinitionState::stackable;
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

namespace sunrise::state::runtime::detail {
/** Isolates character selection from the unrelated item acquisition implementation. */
std::size_t selected_character_index(const AccountState& account) noexcept {
    for (std::size_t index = 0; index < account.characterCount; ++index) {
        if (account.characters[index].selected) {
            return index;
        }
    }
    return account.characterCount;
}
} // namespace sunrise::state::runtime::detail

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
    store::shutdown();
    check(store::open(argv[2],
                      read_text(root + "/investment_schema.sql"),
                      "INSERT INTO account VALUES (1,1001,0);",
                      read_text(root + "/account_settings_schema.sql"),
                      read_text(root + "/account_settings_defaults.sql")),
          "open disposable disk store");
    reset();
    state::PendingVendorReputation pending{};
    check(state::prepare_vendor_reputation(kVendor, kSale, pending) == Disposition::prepared
              && state::commit_vendor_reputation(pending),
          "commit before closing store");
    store::shutdown();
    check(store::open(argv[2], {}, {}, {}, {}), "reopen saved store without seeding");
    check(gunsmith()[0] == kInitialProgression[0] + kAward
              && store::account().profileItems[0].quantity == kInitialQuantity - kCost,
          "material debit and XP survive reopening database");
    store::shutdown();
    std::puts("PASS: reputation transaction, exact debit/credit, refusal, staleness and rollback");
}
