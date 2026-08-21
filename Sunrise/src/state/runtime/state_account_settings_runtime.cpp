#include "../account/settings/settings_delta.h"
#include "runtime.h"
#include "storage/internal.h"

namespace sunrise::state {

/** Builds a complete checked settings candidate without changing authoritative State. */
SettingsUpdateDisposition prepare_settings_update(const account::settings::SettingsDelta& delta,
                                                  PendingSettingsUpdate& mutation) noexcept {
    mutation = {};

    const AccountState current = account_snapshot();
    if (!account::valid(current)) {
        return SettingsUpdateDisposition::rejected;
    }

    account::settings::AccountSettings after{};
    bool changed = false;
    if (!account::settings::apply_delta(delta, current.settings, after, changed)) {
        return SettingsUpdateDisposition::rejected;
    }
    if (!changed) {
        return SettingsUpdateDisposition::acceptedNoChange;
    }

    mutation.beforeSettings = current.settings;
    mutation.afterSettings = after;
    mutation.accountSoid = current.primarySoid;
    mutation.prepared = true;
    return SettingsUpdateDisposition::preparedMutation;
}

/** Commits a prepared settings image only while its account and settings view remain current. */
bool commit_settings_update(PendingSettingsUpdate& mutation) noexcept {
    const PendingSettingsUpdate prepared = mutation;
    mutation = {};

    if (!prepared.prepared || prepared.accountSoid == 0
        || prepared.beforeSettings == prepared.afterSettings
        || !account::settings::valid(prepared.beforeSettings)
        || !account::settings::valid(prepared.afterSettings)) {
        return false;
    }

    AcquireSRWLockExclusive(&runtime::storage::g_stateLock);
    AccountState candidate = runtime::storage::g_state.account;
    bool committed = false;

    if (candidate.primarySoid == prepared.accountSoid) {
        if (candidate.settings == prepared.afterSettings) {
            // A second identical transaction may observe the first one's completed after-image.
            committed = account::valid(candidate);
        } else if (candidate.settings == prepared.beforeSettings) {
            candidate.settings = prepared.afterSettings;
            if (account::valid(candidate)) {
                runtime::storage::g_state.account = candidate;
                committed = true;
            }
        }
    }

    ReleaseSRWLockExclusive(&runtime::storage::g_stateLock);
    return committed;
}

} // namespace sunrise::state
