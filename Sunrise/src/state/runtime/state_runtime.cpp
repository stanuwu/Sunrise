#include <Windows.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>

#include "../../core/filesystem/path.h"
#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../activity/defaults/activity_defaults_validation.h"
#include "../build_data/runtime.h"
#include "equipment/configured_equipment_identity.h"
#include "runtime.h"
#include "state.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace runtime::storage {

State g_state;
SRWLOCK g_stateLock{SRWLOCK_INIT};

} // namespace runtime::storage

namespace {

core::path::Buffer g_inventoryStatePath{};
constexpr std::uint32_t kInventoryStateMagic = 0x564E4953U; // SINV
constexpr std::uint32_t kInventoryStateVersion = 1;
struct InventoryStateHeader {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t recordSize{};
    std::uint32_t characterCount{};
};
struct InventoryCharacterRecord {
    std::uint64_t characterSoid{};
    account::inventory::Equipment equipment{};
    account::inventory::CharacterInventory inventory{};
};

void configure_inventory_state_path(void* module) noexcept {
    g_inventoryStatePath = {};
    if (module != nullptr && core::path::artifact_directory(module, g_inventoryStatePath)) {
        (void)core::path::append(g_inventoryStatePath, L"\\inventory_state.bin");
    }
}

void overlay_persisted_inventory(AccountState& accountState) noexcept {
    if (g_inventoryStatePath.length == 0) return;
    const AccountState baseline = accountState;
    const HANDLE file = CreateFileW(g_inventoryStatePath.chars.data(), GENERIC_READ, FILE_SHARE_READ,
                                    nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    InventoryStateHeader header{};
    DWORD read = 0;
    bool complete = ReadFile(file, &header, sizeof header, &read, nullptr) != FALSE
                    && read == sizeof header && header.magic == kInventoryStateMagic
                    && header.version == kInventoryStateVersion
                    && header.recordSize == sizeof(InventoryCharacterRecord)
                    && header.characterCount <= kCharacterCapacity;
    for (std::size_t recordIndex = 0; complete && recordIndex < header.characterCount;
         ++recordIndex) {
        InventoryCharacterRecord record{};
        complete = ReadFile(file, &record, sizeof record, &read, nullptr) != FALSE
                   && read == sizeof record;
        for (std::size_t characterIndex = 0; complete && characterIndex < accountState.characterCount;
             ++characterIndex) {
            if (accountState.characters[characterIndex].soid == record.characterSoid) {
                accountState.characters[characterIndex].equipment = record.equipment;
                accountState.characters[characterIndex].inventory = record.inventory;
            }
        }
    }
    CloseHandle(file);
    if (!complete || !account::valid(accountState)) {
        accountState = baseline;
    }
}

/** Network-order IPv4 loopback returned by the in-process SignOn route. */
constexpr std::uint32_t kLoopbackAddress = 0x7F000001;
/** BAP relay port used by the generated SignOn response. */
constexpr std::uint16_t kDefaultRelayPort = 30974;
/** Default one-hour lifetime for generated SignOn session tokens. */
constexpr std::uint32_t kDefaultTokenLifetimeSeconds = 3600;
/** Family 5 uses the largest signed 64-bit value as its process-global object key. */
constexpr std::uint64_t kGlobalFamily5Soid =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
/**
 * Fills fixed secret storage with Windows system randomness.
 * @tparam Size Required secret byte count.
 * @param output Secret storage to overwrite.
 * @return True when Windows generates every byte.
 */
template <std::size_t Size>
[[nodiscard]] bool randomize(std::array<std::byte, Size>& output) noexcept {
    return BCryptGenRandom(nullptr,
                           reinterpret_cast<PUCHAR>(output.data()),
                           static_cast<ULONG>(output.size()),
                           BCRYPT_USE_SYSTEM_PREFERRED_RNG)
           >= 0;
}

} // namespace

/**
 * Loads build data and generates secrets with Sunrise's authored activity defaults.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param initialAccount Empty State, or a complete checked account from Core settings.
 * @return True when the cached data passes its checks and every secret gets random bytes.
 */
bool initialize(void* module, const AccountState& initialAccount) noexcept {
    return initialize(module, initialAccount, activity::defaults::authored());
}

/**
 * Loads build data and publishes fixed activity defaults in one step.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param initialAccount Empty State, or a complete checked account from Core settings.
 * @param activityDefaults Complete local fallback policy from immutable Core settings.
 * @return True when account, defaults, cached data, and generated secrets are valid.
 */
bool initialize(void* module,
                const AccountState& initialAccount,
                const activity::defaults::ActivityDefaults& activityDefaults) noexcept {
    if (!account::valid(initialAccount) || !activity::defaults::valid(activityDefaults)) {
        return false;
    }
    configure_inventory_state_path(module);
    AccountState restoredAccount = initialAccount;
    overlay_persisted_inventory(restoredAccount);
    if (!account::valid(restoredAccount)
        || !build_data::initialize(module, runtime::equipment::configured_hash(restoredAccount))) {
        return false;
    }
    {
        // The account key is authored, and a truncated one is consistent enough to go unnoticed.
        std::array<char, 96> line{};
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=account stage=identity primary=0x%016llX characters=%zu",
                          static_cast<unsigned long long>(restoredAccount.primarySoid),
                          restoredAccount.characterCount);
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    State initialized{};
    if (!randomize(initialized.signOn.encryptionKey)
        || !randomize(initialized.signOn.authenticationKey)
        || !randomize(initialized.signOn.sessionToken) || !randomize(initialized.bap.nonce)
        || !randomize(initialized.bap.sessionKey) || !randomize(initialized.bap.envelopeIv)) {
        SecureZeroMemory(&initialized, sizeof initialized);
        build_data::shutdown();
        return false;
    }
    initialized.signOn.relayAddress = kLoopbackAddress;
    initialized.signOn.relayPort = kDefaultRelayPort;
    initialized.signOn.tokenLifetimeSeconds = kDefaultTokenLifetimeSeconds;
    initialized.account = restoredAccount;
    initialized.activity.defaults = activityDefaults;
    initialized.investment.family5.objectSoid = kGlobalFamily5Soid;
    // Only the override lists come from settings. Identity and gate stay owned by State.
    const Family5State& authored = core::settings::get().initialFamily5;
    initialized.investment.family5.flags = authored.flags;
    initialized.investment.family5.flagCount = authored.flagCount;
    initialized.investment.family5.values = authored.values;
    initialized.investment.family5.valueCount = authored.valueCount;
    // The arm is account-wide and rides the first ws-503, which goes out before any pick. Nothing
    // is selected at boot, so it is armed when any authored character carries the bypass. The
    // per-character objB byte is the other half, and it still decides which character it opens.
    for (std::size_t index = 0; index < restoredAccount.characterCount; ++index) {
        if (restoredAccount.characters[index].contentBypass) {
            initialized.investment.family5.contentGateArm = true;
            break;
        }
    }

    // Publish one complete State only after every generated secret is valid.
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    runtime::storage::g_state = initialized;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    SecureZeroMemory(&initialized, sizeof initialized);
    return true;
}

/** Securely erases State, including activity destinations and matchmaking descriptors. */
void shutdown() noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    SecureZeroMemory(&runtime::storage::g_state, sizeof runtime::storage::g_state);
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    build_data::shutdown();
}

/** @return Immutable generated SignOn session fields. */
const SignOnState& sign_on() noexcept {
    return runtime::storage::g_state.signOn;
}

/**
 * Publishes the bootstrap content-id token read from the installed client.
 * @param token Exactly 16 native bytes.
 * @return True when the complete token is kept for this process.
 */
bool publish_bootstrap_token(std::span<const std::byte> token) noexcept {
    SignOnState& signOn = runtime::storage::g_state.signOn;
    if (token.size() != signOn.bootstrapToken.size()) {
        return false;
    }
    std::copy(token.begin(), token.end(), signOn.bootstrapToken.begin());
    signOn.bootstrapTokenPresent = true;
    return true;
}

/** @return Immutable generated BAP session fields. */
const BapState& bap() noexcept {
    return runtime::storage::g_state.bap;
}

/** @return A copy of the evaluated content state, read under the lock. */
InvestmentState investment_snapshot() noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    const InvestmentState snapshot = runtime::storage::g_state.investment;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
    return snapshot;
}

} // namespace sunrise::state
