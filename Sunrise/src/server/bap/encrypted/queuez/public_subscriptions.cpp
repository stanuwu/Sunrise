#include "public_subscriptions.h"

#include <algorithm>
#include <limits>

#include "../../../../core/settings/settings.h"
#include "../../../../middleware/bap/family_subscription.h"
#include "../../../../middleware/datagen/definitions.h"
#include "../../../../middleware/secure_channel/runtime.h"
#include "../../../../state/account/public_profiles.h"
#include "../../../../state/activity/fireteam.h"
#include "../../internal.h"
#include "../../proxy/proxy_routing.h"
#include "../internal.h"
#include "../push/queuez/queuez_update_frame.h"
#include "../push/snapshot/snapshot.h"

namespace sunrise::server::bap::encrypted::public_queuez {
namespace {
namespace profiles = state::account::profiles;
namespace snapshot = push::snapshot;
constexpr std::uint64_t kRetryDelayMs = 400;

bool supported(std::uint32_t family) noexcept {
    return family == 0 || family == 1 || family == 2 || family == 3 || family == 4 || family == 6
           || family == 7;
}

Subscription*
find(Subscriptions& subscriptions, std::uint32_t family, std::uint64_t root) noexcept {
    for (auto& entry : subscriptions.entries) {
        if (entry.root == root && entry.family == family) {
            return &entry;
        }
    }
    return nullptr;
}

std::uint32_t generation(const Subscription& subscription) noexcept {
    const auto own = profiles::generation(state::account_for_public_root(subscription.root));
    if (own == 0) {
        return own;
    }
    if (subscription.family == 6) {
        return profiles::public_generation();
    }
    if (subscription.family == 2) {
        // The roster row also carries the served account's seat and fireteam, and neither of
        // those moves the projected profile. Folding them in is what refreshes the row on
        // seating instead of leaving it stale until the owner's profile happens to change.
        const auto mixed = own ^ snapshot::social_roster_revision(subscription.root);
        return mixed != 0 ? mixed : 1U;
    }
    return own;
}

bool prepare(Scratch& scratch,
             const Subscription& before,
             std::int32_t version,
             snapshot::Prepared& prepared,
             std::uint64_t& character) noexcept {
    const auto handle = state::account_for_public_root(before.root);
    const state::ScopedAccount accountScope(handle, true);
    character = profiles::banner_character(handle);
    if (before.family == 0) {
        const auto previous = before.character != character ? before.character : 0;
        if (!snapshot::prepare_banner(scratch, before.root, version, previous, prepared)) {
            prepared.family = {
                before.family, before.root, version, middleware::queuez::kFullSnapshotFlag, {}};
        }
    } else {
        const middleware::queuez::Subscription subscription{before.family, before.root};
        if (!snapshot::prepare_initial(scratch, subscription, {}, prepared)) {
            return false;
        }
        prepared.family.version = version;
    }
    return true;
}
} // namespace

Result consume(Session& session,
               Scratch& scratch,
               const middleware::bap::RequestFrame& request,
               std::span<std::byte> response,
               std::size_t& written,
               std::uint64_t now) noexcept {
    if (!core::settings::hosts_session() || (request.serviceId != 12 && request.serviceId != 14)) {
        return Result::notHandled;
    }
    middleware::queuez::Subscription selector{};
    if (!session.authenticated || session.accountHandle == state::kInvalidAccount
        || request.frameType != middleware::bap::FrameType::encrypted
        || !middleware::bap::family_subscription::parse(request.body, selector)) {
        return Result::failure;
    }
    if (!supported(selector.familyType)) {
        return Result::notHandled;
    }
    // The playing host's own investment remains on the original local SQLite route.
    if (session.accountHandle == state::kLocalAccount
        && proxy::is_local_root(selector.familyRootSoid)
        && (selector.familyType == 0 || selector.familyType == 3 || selector.familyType == 4)) {
        return Result::notHandled;
    }
    written = 0;
    if (selector.familyRootSoid == 0) {
        return Result::failure;
    }
    const bool removing = request.serviceId == 14;
    auto* entry = find(session.publicSubscriptions, selector.familyType, selector.familyRootSoid);
    if (!removing && !entry) {
        const auto count =
            std::count_if(session.publicSubscriptions.entries.begin(),
                          session.publicSubscriptions.entries.end(),
                          [&](const Subscription& value) {
                              return value.root != 0 && value.family == selector.familyType;
                          });
        if (count >= kRootsPerFamily) {
            return Result::failure;
        }
        for (auto& candidate : session.publicSubscriptions.entries) {
            if (candidate.root == 0) {
                entry = &candidate;
                break;
            }
        }
        if (!entry) {
            return Result::failure;
        }
    }
    Subscription staged = entry ? *entry : Subscription{};
    const bool first = !removing && staged.root == 0;
    if (first) {
        staged.root = selector.familyRootSoid;
        staged.family = static_cast<std::uint8_t>(selector.familyType);
    }
    const ServiceRoute route{ResponseMode::reply,
                             removing ? middleware::bap::ResponseService::unsubscribeFamily
                                      : middleware::bap::ResponseService::subscribeFamily,
                             BodyCodec::empty};
    std::size_t size{};
    if (!reply::encode(
            scratch, route, request.taskId, session.sessionKey, session.sendNonce, {}, size)) {
        return Result::failure;
    }
    auto nonce = session.sendNonce;
    middleware::secure_channel::advance_nonce(nonce);
    bool publishedJoin = false;
    if (first) {
        snapshot::Prepared prepared;
        std::uint64_t character{};
        const auto current = generation(staged);
        if (!prepare(scratch, staged, 0, prepared, character)) {
            return Result::failure;
        }
        publishedJoin = staged.family == 7 && prepared.family.objects.size() > 1;
        const bool content = !prepared.family.objects.empty();
        if (!push::queuez_frame::append_prepared_frame(
                scratch, prepared, session.sessionKey, nonce, scratch.framed, size)) {
            return Result::failure;
        }
        staged.hasFrame = true;
        staged.character = content ? character : 0;
        staged.generation = content ? current : 0;
    }
    if (size > response.size()) {
        return Result::failure;
    }
    std::copy_n(scratch.framed.begin(), size, response.begin());
    if (entry) {
        if (removing) {
            *entry = {};
        } else {
            // The native declaration can finish after the first answer. Preserve the established
            // 400-ms replay for banner/account records, with independent obligations per root.
            staged.replayPending = staged.family == 0 || staged.family == 4;
            staged.nextAttemptTick = now + kRetryDelayMs;
            *entry = staged;
        }
    }
    session.sendNonce = nonce;
    written = size;
    if (publishedJoin) {
        const auto target = state::account_for_public_root(selector.familyRootSoid);
        static_cast<void>(state::activity::fireteam::request_join(
            state::account_primary_soid(session.accountHandle),
            state::account_primary_soid(target),
            now));
    }
    return Result::success;
}

bool poll(Session& session,
          Scratch& scratch,
          std::span<std::byte> response,
          std::size_t& written,
          bool& touchesScratch,
          std::uint64_t now) noexcept {
    written = 0;
    if (!core::settings::hosts_session() || !session.authenticated) {
        return false;
    }
    auto& subscriptions = session.publicSubscriptions;
    const auto start = subscriptions.cursor;
    for (std::size_t offset = 0; offset < subscriptions.entries.size(); ++offset) {
        const auto index = (start + offset) % subscriptions.entries.size();
        auto& entry = subscriptions.entries[index];
        if (entry.root == 0 || now < entry.nextAttemptTick) {
            continue;
        }
        const auto current = generation(entry);
        if (current == 0 || (!entry.replayPending && current == entry.generation)) {
            continue;
        }
        subscriptions.cursor =
            static_cast<std::uint8_t>((index + 1) % subscriptions.entries.size());
        entry.nextAttemptTick = now + kRetryDelayMs;
        if (entry.version == (std::numeric_limits<std::int32_t>::max)()) {
            continue;
        }
        const auto version = entry.version + (current != entry.generation ? 1 : 0);
        snapshot::Prepared prepared;
        std::uint64_t character{};
        touchesScratch = true;
        if (!prepare(scratch, entry, version, prepared, character)) {
            return false;
        }
        if (prepared.family.objects.empty()) {
            push::queuez_frame::clear_object_storage(
                scratch, prepared.rawClearSize, prepared.compressedClearSize);
            return false;
        }
        auto nonce = session.sendNonce;
        std::size_t size{};
        if (!push::queuez_frame::append_prepared_frame(
                scratch, prepared, session.sessionKey, nonce, scratch.framed, size)
            || size > response.size()) {
            return false;
        }
        std::copy_n(scratch.framed.begin(), size, response.begin());
        entry.generation = current;
        entry.version = version;
        entry.character = character;
        entry.replayPending = false;
        session.sendNonce = nonce;
        written = size;
        if (entry.family == 7 && prepared.family.objects.size() > 1) {
            const auto target = state::account_for_public_root(entry.root);
            static_cast<void>(state::activity::fireteam::request_join(
                state::account_primary_soid(session.accountHandle),
                state::account_primary_soid(target),
                now));
        }
        return true;
    }
    return false;
}
} // namespace sunrise::server::bap::encrypted::public_queuez
