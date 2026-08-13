#include "queuez_family_staging.h"

#include <cstddef>
#include <limits>

#include "../queuez_state_validation.h"

namespace sunrise::server::bap::encrypted::queuez {

/** @return True for a logical match between two resident rows. */
bool staging::same_resident(const ResidentObject& left, const ResidentObject& right) noexcept {
    return left.objectSoid == right.objectSoid && left.definitionId == right.definitionId;
}

/**
 * Compares two canonical peer states field by field.
 * @return True when both are valid and every fixed Family-4 field matches.
 */
bool staging::same_state(const SessionState& left, const SessionState& right) noexcept {
    if (!valid(left) || !valid(right) || left.family4RootSoid != right.family4RootSoid
        || left.family4Version != right.family4Version
        || left.family4ResidentCount != right.family4ResidentCount
        || left.family3Phase != right.family3Phase || left.family4Active != right.family4Active) {
        return false;
    }
    for (std::size_t index = 0; index < left.family4Residents.size(); ++index) {
        if (!staging::same_resident(left.family4Residents[index], right.family4Residents[index])) {
            return false;
        }
    }
    return true;
}

namespace {

/**
 * Compares one active resident manifest with a staged full snapshot.
 * @param state Active Family-4 state owned by the peer.
 * @param candidate Possible version-zero snapshot state.
 * @return True only when root, version, count and every resident id match.
 */
[[nodiscard]] bool same_manifest(const SessionState& state,
                                 const SessionState& candidate) noexcept {
    if (!valid(state) || !valid(candidate) || state.family4RootSoid != candidate.family4RootSoid
        || state.family4Version != candidate.family4Version
        || state.family4ResidentCount != candidate.family4ResidentCount) {
        return false;
    }
    for (std::size_t index = 0; index < state.family4ResidentCount; ++index) {
        if (!staging::same_resident(state.family4Residents[index],
                                    candidate.family4Residents[index])) {
            return false;
        }
    }
    return true;
}

} // namespace

/** Stages a first Family-4 manifest, or checks an identical version-zero replay. */
bool stage_family4_snapshot(const SessionState& before,
                            const middleware::queuez::Family& family,
                            SessionState& after) noexcept {
    after = before;
    if (!valid(before) || family.type != kAccountFamilyType || family.rootSoid == 0
        || family.version != kInitialFamilyVersion
        || family.flags != middleware::queuez::kFullSnapshotFlag || family.objects.empty()
        || family.objects.size() > kResidentCapacity
        || family.objects.size()
               > static_cast<std::size_t>((std::numeric_limits<std::uint8_t>::max)())) {
        return false;
    }

    SessionState candidate{};
    candidate.family4RootSoid = family.rootSoid;
    candidate.family4Version = family.version;
    candidate.family4ResidentCount = static_cast<std::uint8_t>(family.objects.size());
    candidate.family3Phase = before.family3Phase;
    candidate.family4Active = true;
    for (std::size_t index = 0; index < family.objects.size(); ++index) {
        const middleware::queuez::Object& object = family.objects[index];
        if (object.id == 0 || object.version == 0) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (family.objects[prior].version == object.version) {
                return false;
            }
        }
        candidate.family4Residents[index] = ResidentObject{object.version, object.id};
    }
    if (candidate.family4Residents.front().objectSoid != family.rootSoid) {
        return false;
    }
    if (before.family4Active) {
        return before.family4Version == kInitialFamilyVersion && same_manifest(before, candidate);
    }
    if (before.family3Phase != Family3Phase::normal) {
        return false;
    }
    after = candidate;
    return true;
}

bool stage_family4_replacement(const SessionState& before,
                               const middleware::queuez::Family& family,
                               SessionState& after) noexcept {
    after = before;
    if (!valid(before) || !before.family4Active || family.type != kAccountFamilyType
        || family.rootSoid != before.family4RootSoid
        || family.version != before.family4Version + 1
        || family.flags != 0 || family.objects.empty()
        || family.objects.size() > kResidentCapacity
        || family.objects.size() > static_cast<std::size_t>((std::numeric_limits<std::uint8_t>::max)())) {
        return false;
    }
    SessionState candidate = before;
    candidate.family4Version = family.version;
    for (const auto& object : family.objects) {
        if (object.id == 0 || object.version == 0 || object.payload.empty()) return false;
        std::size_t resident = 0;
        while (resident < candidate.family4ResidentCount
               && candidate.family4Residents[resident].objectSoid != object.version) {
            ++resident;
        }
        if (resident == candidate.family4ResidentCount) {
            if (candidate.family4ResidentCount >= candidate.family4Residents.size()) return false;
            ++candidate.family4ResidentCount;
        }
        candidate.family4Residents[resident] = {object.version, object.id};
    }
    after = candidate;
    return true;
}

/** Stages the family-zero publication policy. */
bool stage_family0_subscription(const SessionState& before,
                                std::uint64_t selectedCharacter,
                                bool& publish,
                                bool& incremental,
                                SessionState& after) noexcept {
    publish = false;
    incremental = false;
    after = before;
    if (!valid(before) || selectedCharacter == 0) {
        return false;
    }
    if (!before.family0Active) {
        publish = true;
        after.family0Active = true;
        after.family0Character = selectedCharacter;
        after.family0Version = kInitialFamilyVersion;
        return true;
    }
    if (before.family0Character == selectedCharacter) {
        return true;
    }
    publish = true;
    incremental = true;
    after.family0Character = selectedCharacter;
    after.family0Version = before.family0Version + 1;
    return true;
}

/** Stages the measured Family-3 subscription policy: full first, then response-only. */
bool stage_family3_subscription(const SessionState& before,
                                const middleware::queuez::Subscription& subscription,
                                bool& publish,
                                SessionState& after) noexcept {
    publish = false;
    after = before;
    if (!valid(before) || subscription.familyType != kRosterFamilyType
        || subscription.familyRootSoid == 0) {
        return false;
    }
    if (before.family4Active && subscription.familyRootSoid != before.family4RootSoid) {
        return false;
    }
    if (before.family3Phase == Family3Phase::normal) {
        publish = true;
        return true;
    }
    if (!before.family4Active) {
        return false;
    }
    if (before.family3Phase == Family3Phase::publishOnce) {
        publish = true;
        after.family3Phase = Family3Phase::responseOnly;
        return true;
    }
    return before.family3Phase == Family3Phase::responseOnly;
}

void stage_unsubscription(const SessionState& before,
                          std::uint64_t familyRootSoid,
                          SessionState& after) noexcept {
    after = before;
    if (before.family4Active && familyRootSoid == before.family4RootSoid) {
        after = {};
    }
}

} // namespace sunrise::server::bap::encrypted::queuez
