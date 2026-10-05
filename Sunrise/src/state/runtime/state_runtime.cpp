#include <Windows.h>

#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../account/account_token.h"
#include "../account/public_profiles.h"
#include "../account/shared_channel_material.h"
#include "../activity/defaults/activity_defaults_validation.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "../unlocks/unlocks_records.h"
#include "equipment/configured_equipment_identity.h"
#include "runtime.h"
#include "state.h"
#include "state_account_transaction_helpers.h"
#include "storage/internal.h"

namespace sunrise::state {
namespace runtime::storage {

State g_state;
SRWLOCK g_stateLock{SRWLOCK_INIT};

} // namespace runtime::storage

namespace {

/** Network-order IPv4 loopback returned by the in-process SignOn route. */
constexpr std::uint32_t kLoopbackAddress = 0x7F000001;
/** Default one-hour lifetime for generated SignOn session tokens. */
constexpr std::uint32_t kDefaultTokenLifetimeSeconds = 3600;
/** Glimmer item definition, the currency an artifact reset charges. */
constexpr std::uint32_t kGlimmerHash = 3159615086U;

/** @return True when this item is an artifact mod a reset removes and unplugs. */
[[nodiscard]] bool is_artifact_mod(std::uint32_t hash) noexcept {
    std::array<build_data::ArtifactSaleRow, build_data::kArtifactSaleRowCapacity> rows{};
    std::size_t count = 0;
    if (!build_data::artifact_sale_rows(rows, count)) {
        return false;
    }
    for (std::size_t row = 0; row < count; ++row) {
        // The reset row itself sells no mod, and it is the one row that buys no unlock.
        if (rows[row].unlockFlagSlot != build_data::collectibles::kUnavailableFlagSlot
            && rows[row].itemHash == hash) {
            return true;
        }
    }
    return false;
}

[[nodiscard]] bool same_profile_item(const account::inventory::ProfileItem& left,
                                     const account::inventory::ProfileItem& right) noexcept {
    return left.instanceSoid == right.instanceSoid && left.definitionHash == right.definitionHash
           && left.quantity == right.quantity && left.mutationSerial == right.mutationSerial
           && left.seen == right.seen;
}

/** Restores artifact-mod sockets to their manifest-declared initial plugs. */
[[nodiscard]] bool clear_artifact_sockets(account::inventory::Item& item) noexcept {
    if (item.sockets.policy != account::inventory::SocketPolicy::authored) {
        return true;
    }
    build_data::items::Definition base{};
    build_data::items::details::Definition detail{};
    if (!build_data::find_item_definition_hash(item.definitionHash, base)
        || !build_data::find_configured_item_detail(base.definitionIndex, detail)
        || detail.definitionIndex != base.definitionIndex
        || item.sockets.plugCount != detail.ordinarySocketCount) {
        return false;
    }
    for (std::size_t lane = 0; lane < item.sockets.plugCount; ++lane) {
        const auto& plug = item.sockets.plugs[lane];
        if (!plug.has_value() || !is_artifact_mod(*plug)) {
            continue;
        }
        const std::uint16_t initial = detail.initialPlugIndices[lane];
        if (initial == build_data::items::details::kUnavailableItemIndex) {
            item.sockets.plugs[lane].reset();
            continue;
        }
        build_data::items::Definition replacement{};
        if (!build_data::find_item_definition_index(initial, replacement)) {
            return false;
        }
        item.sockets.plugs[lane] = replacement.definitionHash;
    }
    return true;
}

/**
 * Adds one instance soid to the reset result when its socket state changed.
 * @param result Receives the soid; unchanged when the sockets match.
 * @return False when the result's fixed soid list is full.
 */
[[nodiscard]] bool record_changed_item(const account::inventory::Item& prior,
                                       const account::inventory::Item& current,
                                       ArtifactResetResult& result) noexcept {
    if (prior.sockets.policy == current.sockets.policy
        && prior.sockets.plugCount == current.sockets.plugCount
        && prior.sockets.plugs == current.sockets.plugs) {
        return true;
    }
    if (result.instanceCount >= result.instanceSoids.size()) {
        return false;
    }
    result.instanceSoids[result.instanceCount++] = current.instanceSoid;
    return true;
}

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

/** Erases owned payload bytes before releasing their vector storage, then resets valid State. */
void secure_reset(State& state) noexcept {
    SecureZeroMemory(&state.signOn, sizeof state.signOn);
    SecureZeroMemory(&state.bap, sizeof state.bap);
    for (activity::SessionRecord& session : state.activity.sessions) {
        for (activity::mission::PendingIntent& pending : session.mission.pendingIntents) {
            if (!pending.value.authBody.empty()) {
                SecureZeroMemory(pending.value.authBody.data(), pending.value.authBody.size());
            }
        }
        std::vector<activity::mission::PendingIntent>{}.swap(session.mission.pendingIntents);
    }
    // State is too large for a stack temporary, so the reset reconstructs it in place.
    state.~State();
    new (&state) State{};
}

/**
 * Canonicalizes only profile rows which the installed socket UI materializes as action sources.
 * @param accountState Account canonicalized in place.
 * @return True when every profile row canonicalizes.
 */
[[nodiscard]] bool canonicalize_profile_item_identities(AccountState& accountState) noexcept {
    if (!account::valid(accountState)) {
        return false;
    }
    if (accountState.profileItemCount == 0) {
        // An account with no profile stack needs no socket relation.
        return true;
    }
    if (!build_data::socket_plug_rules_ready()) {
        return false;
    }
    std::array<bool, account::inventory::kProfileItemCapacity> actionSources{};
    std::size_t actionSourceCount = 0;
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        const account::inventory::ProfileItem& profileItem = accountState.profileItems[index];
        build_data::items::Definition item{};
        build_data::items::details::Definition detail{};
        build_data::inventory::buckets::Descriptor bucket{};
        if (!build_data::find_item_definition_hash(profileItem.definitionHash, item)
            || item.definitionHash != profileItem.definitionHash
            || !build_data::find_configured_item_detail(item.definitionIndex, detail)
            || detail.definitionIndex != item.definitionIndex
            || detail.definitionHash != item.definitionHash || detail.bucketId != item.bucketId
            || detail.instancedDefinitionState
                   != build_data::items::details::InstancedDefinitionState::stackable
            || !build_data::find_inventory_bucket_descriptor(item.bucketId, bucket)
            || bucket.arraySelector != build_data::inventory::buckets::ArraySelector::profile) {
            return false;
        }
        actionSources[index] =
            build_data::is_profile_action_source(item.definitionIndex, item.bucketId);
        if (actionSources[index]
            && ++actionSourceCount > account::inventory::kProfileActionSourceCapacity) {
            return false;
        }
    }

    // Clear stale keys on non-instanced rows first so they cannot reserve the identity namespace.
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        if (!actionSources[index]) {
            accountState.profileItems[index].instanceSoid = 0;
        }
    }

    std::uint64_t nextProfileSoid = account::inventory::kFirstProfileItemInstanceSoid;
    for (std::size_t index = 0; index < accountState.profileItemCount; ++index) {
        account::inventory::ProfileItem& item = accountState.profileItems[index];
        if (!actionSources[index] || item.instanceSoid != 0) {
            continue;
        }
        while (runtime::detail::account_owns_soid(accountState, nextProfileSoid)) {
            if (nextProfileSoid == (std::numeric_limits<std::uint64_t>::max)()) {
                return false;
            }
            ++nextProfileSoid;
        }
        item.instanceSoid = nextProfileSoid;
        if (nextProfileSoid != (std::numeric_limits<std::uint64_t>::max)()) {
            ++nextProfileSoid;
        }
    }
    return account::valid(accountState);
}

} // namespace

/**
 * Loads build data and generates secrets with Sunrise's authored activity defaults.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @return True when the cached data passes its checks and every secret gets random bytes.
 */
bool initialize(void* module) noexcept {
    return initialize(module, activity::defaults::authored());
}

/**
 * Loads build data and publishes fixed activity defaults in one step.
 * @param module Loaded Sunrise module, or null to disable disk persistence.
 * @param activityDefaults Complete local fallback policy from immutable Core settings.
 * @return True when account, defaults, cached data, and generated secrets are valid.
 */
bool initialize(void* module,
                const activity::defaults::ActivityDefaults& activityDefaults) noexcept {
    // AccountState and State are multi-megabyte fixed-capacity values. Keeping both as locals
    // exceeds the game's startup-thread stack before this function can execute any code.
    const std::unique_ptr<AccountState> runtimeAccount{new (std::nothrow) AccountState{}};
    const std::unique_ptr<State> initialized{new (std::nothrow) State{}};
    if (!runtimeAccount || !initialized) {
        return false;
    }
    investment::store::Transaction transaction;
    if (!transaction.ready() || !investment::store::read_account(*runtimeAccount)
        || !activity::defaults::valid(activityDefaults)) {
        return false;
    }
    if (!build_data::initialize(module, runtime::equipment::configured_hash(*runtimeAccount))) {
        return false;
    }
    build_data::set_exotic_catalyst_completion_enabled(
        core::settings::get().completeExoticCatalysts);
    // Only a cache hit carries the plug relation here; a first build repeats this after extraction.
    if (build_data::socket_plug_rules_ready()
        && !canonicalize_profile_item_identities(*runtimeAccount)) {
        build_data::shutdown();
        return false;
    }
    {
        // Buffer holds the whole line; a truncated account key would read as a valid one.
        std::array<char, 96> line{};
        const int written =
            std::snprintf(line.data(),
                          line.size(),
                          "ev=account stage=identity primary=0x%016llX characters=%zu",
                          static_cast<unsigned long long>(runtimeAccount->primarySoid),
                          runtimeAccount->characterCount);
        if (written > 0) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::info,
                             {line.data(), static_cast<std::size_t>(written)});
        }
    }
    if (!randomize(initialized->signOn.encryptionKey)
        || !randomize(initialized->signOn.authenticationKey)
        || !randomize(initialized->signOn.sessionToken) || !randomize(initialized->bap.nonce)
        || !randomize(initialized->bap.sessionKey) || !randomize(initialized->bap.envelopeIv)) {
        secure_reset(*initialized);
        build_data::shutdown();
        return false;
    }
    initialized->signOn.relayAddress = kLoopbackAddress;
    if (core::settings::role() != core::settings::Role::embedded
        || core::settings::get().server.upstream.enabled) {
        const auto bridge = account::shared_channel_material();
        initialized->signOn.encryptionKey = bridge.encryptionKey;
        initialized->signOn.authenticationKey = bridge.authenticationKey;
        if (core::settings::role() != core::settings::Role::host) {
            account::signon_token(runtimeAccount->primarySoid, initialized->signOn.sessionToken);
        }
    }
    // The published relay port is the one the listener binds, so both move with one setting.
    initialized->signOn.relayPort = core::settings::get().server.bapPort;
    initialized->signOn.tokenLifetimeSeconds = kDefaultTokenLifetimeSeconds;
    if (!investment::store::write_account(*runtimeAccount)) {
        build_data::shutdown();
        return false;
    }
    initialized->activity.defaults = activityDefaults;
    // Publish one complete State only after every generated secret is valid.
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    secure_reset(runtime::storage::g_state);
    runtime::storage::g_state = std::move(*initialized);
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    secure_reset(*initialized);
    // The seeded banks decide every derived bar, gate and seasonal counter, so both run last.
    (void)seed_seasonal_progression();
    unlocks::records::seed();
    if (!transaction.commit()) {
        return false;
    }
    account::profiles::reset(runtimeAccount->primarySoid);
    return true;
}

/** Securely erases State, including activity destinations and matchmaking descriptors. */
void shutdown() noexcept {
    account::profiles::reset();
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    secure_reset(runtime::storage::g_state);
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    build_data::shutdown();
    investment::store::shutdown();
}

/** @return Immutable generated SignOn session fields. */
const SignOnState& sign_on() noexcept {
    return runtime::storage::g_state.signOn;
}

void publish_bap_port(std::uint16_t port) noexcept {
    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    runtime::storage::g_state.signOn.relayPort = port;
    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
}

/** Ensures every native profile action source has one unique runtime item-instance key. */
bool ensure_profile_item_identities() noexcept {
    investment::store::g_mutex.lock();
    AccountState candidate = investment::store::account();
    const bool ready = canonicalize_profile_item_identities(candidate);
    if (ready) {
        if (!investment::store::write_account(candidate)) {
            investment::store::g_mutex.unlock();
            return false;
        }
    }
    investment::store::g_mutex.unlock();
    return ready;
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

/** Records when the account signed in, on every character the account owns. */
void publish_sign_in_time(std::uint64_t seconds) noexcept {
    investment::store::set_sign_in_time(seconds);
}

/** @return Immutable generated BAP session fields. */
const BapState& bap() noexcept {
    return runtime::storage::g_state.bap;
}

/** Generates one connection's own secure-channel material. */
bool new_bap_session(BapState& output) noexcept {
    output = {};
    if (!randomize(output.nonce) || !randomize(output.sessionKey)
        || !randomize(output.envelopeIv)) {
        output = {};
        return false;
    }
    return true;
}

/** Copies one complete evaluated content state with build-derived catalyst overrides. */
bool investment_snapshot(InvestmentState& output) noexcept {
    investment::store::g_mutex.lock();
    InvestmentState snapshot;
    const bool loaded = investment::store::read_family5(snapshot.family5);
    investment::store::g_mutex.unlock();
    if (!loaded || !build_data::complete_exotic_catalyst_investment(snapshot.family5)) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         "ev=investment stage=snapshot result=fail reason=catalyst");
        return false;
    }
    output = snapshot;
    return true;
}

/**
 * Charges Glimmer, removes every artifact mod, and refunds the spent unlock points.
 * @param result Receives the instances whose sockets changed; empty unless the commit lands.
 * @return False when the cost cannot be paid or the account moved since the snapshot.
 */
bool reset_artifact(std::int32_t glimmerCost, ArtifactResetResult& result) noexcept {
    result = {};
    investment::store::Transaction transaction;
    if (!transaction.ready()) {
        return false;
    }
    if (glimmerCost <= 0) {
        return false;
    }

    const std::uint32_t previousMods = artifact_mod_mask();
    const AccountState before = bound_account_snapshot();
    if (previousMods == 0 || !account::valid(before)
        || !runtime::detail::valid_profile_inventory(before)) {
        return false;
    }

    std::int32_t remainingCost = glimmerCost;
    auto compacted = before.profileItems;
    std::size_t compactedCount = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        auto item = before.profileItems[index];
        if (item.definitionHash == kGlimmerHash && remainingCost > 0) {
            const std::int32_t spent = (std::min)(item.quantity, remainingCost);
            item.quantity -= spent;
            remainingCost -= spent;
        }
        if (item.quantity > 0 && !is_artifact_mod(item.definitionHash)) {
            compacted[compactedCount++] = item;
        }
    }
    if (remainingCost != 0) {
        return false;
    }
    std::fill(compacted.begin() + static_cast<std::ptrdiff_t>(compactedCount),
              compacted.end(),
              account::inventory::ProfileItem{});

    std::int32_t serial = 0;
    for (std::size_t index = 0; index < before.profileItemCount; ++index) {
        serial = (std::max)(serial, before.profileItems[index].mutationSerial);
    }
    std::size_t changedRows = 0;
    for (std::size_t index = 0; index < compactedCount; ++index) {
        changedRows += static_cast<std::size_t>(
            index >= before.profileItemCount
            || !same_profile_item(compacted[index], before.profileItems[index]));
    }
    if (changedRows
        > static_cast<std::size_t>((std::numeric_limits<std::int32_t>::max)() - serial)) {
        return false;
    }
    for (std::size_t index = 0; index < compactedCount; ++index) {
        if (index >= before.profileItemCount
            || !same_profile_item(compacted[index], before.profileItems[index])) {
            compacted[index].mutationSerial = ++serial;
        }
    }

    AccountState candidate = before;
    candidate.profileItems = compacted;
    candidate.profileItemCount = compactedCount;
    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount;
         ++characterIndex) {
        auto& character = candidate.characters[characterIndex];
        for (auto& item : character.equipment.slots) {
            if (item.has_value() && !clear_artifact_sockets(*item)) {
                return false;
            }
        }
        for (std::size_t itemIndex = 0; itemIndex < character.inventory.count; ++itemIndex) {
            if (!clear_artifact_sockets(character.inventory.values[itemIndex])) {
                return false;
            }
        }
    }
    ArtifactResetResult changed{};
    for (std::size_t characterIndex = 0; characterIndex < candidate.characterCount;
         ++characterIndex) {
        if (!before.characters[characterIndex].selected) {
            continue;
        }
        const auto& prior = before.characters[characterIndex];
        const auto& current = candidate.characters[characterIndex];
        for (std::size_t slot = 0; slot < current.equipment.slots.size(); ++slot) {
            if (current.equipment.slots[slot].has_value()
                && (!prior.equipment.slots[slot].has_value()
                    || !record_changed_item(
                        *prior.equipment.slots[slot], *current.equipment.slots[slot], changed))) {
                return false;
            }
        }
        for (std::size_t index = 0; index < current.inventory.count; ++index) {
            if (!record_changed_item(
                    prior.inventory.values[index], current.inventory.values[index], changed)) {
                return false;
            }
        }
    }
    if (!account::valid(candidate) || !runtime::detail::valid_profile_inventory(candidate)
        || !replace_artifact_mod_mask(previousMods, 0)) {
        return false;
    }

    const AccountState live = investment::store::account();
    bool current =
        runtime::detail::same_profile_inventory(live, before.profileItems, before.profileItemCount)
        && live.characterCount == before.characterCount;
    for (std::size_t index = 0; current && index < before.characterCount; ++index) {
        current = runtime::detail::same_character(live.characters[index], before.characters[index]);
    }
    if (!current || !investment::store::write_account(candidate) || !transaction.commit()) {
        return false;
    }
    result = changed;
    return true;
}

} // namespace sunrise::state
