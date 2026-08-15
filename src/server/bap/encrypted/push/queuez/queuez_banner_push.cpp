#include <array>
#include <cstdio>
#include <span>

#include "../../../../../core/logging/log.h"
#include "../../../../../middleware/secure_channel/runtime.h"
#include "../../../../../state/account/account_state.h"
#include "../../../../../state/runtime/runtime.h"
#include "../../queuez/queuez_state_validation.h"
#include "../snapshot/snapshot.h"
#include "queuez_push_reporting.h"
#include "queuez_update_frame.h"

namespace sunrise::server::bap::encrypted::push {
namespace {

/** One line carries the refusal key and nothing else. */
constexpr std::size_t kSkipLineCapacity = 96;

/**
 * Reports the banner move declining to build a frame.
 * The move has 4 separate refusals and all leave the emblem where it is, so the key is the only
 * way to tell a pick owing nothing from a ladder that was never seeded.
 * @param reason Key naming the refusal.
 */
void report_skip(const char* reason) noexcept {
    std::array<char, kSkipLineCapacity> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=queuez stage=banner_move result=skip reason=%s", reason);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/**
 * Appends the unsolicited family-zero banner pair that follows a family-three subscription.
 * Sent twice per boot at the same version: family zero hits the state-1 race with no svc-12
 * re-push behind it, so a rejected first pair would blank the banner for the run.
 * @param scratch Lock-owned transform buffers.
 * @param familyRootSoid Root the Client subscribed for Family 3.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce, advanced only by a complete frame.
 * @param response Caller-owned output containing prior frames.
 * @param written Existing byte count, updated by a complete frame.
 * @return True when the banner frame is appended.
 */
bool append_banner_notification(Scratch& scratch,
                                const queuez::SessionState& before,
                                std::uint64_t familyRootSoid,
                                std::span<const std::byte, state::kAesKeySize> key,
                                std::array<std::byte, state::kBapNonceSize>& nonce,
                                std::span<std::byte> response,
                                std::size_t& written,
                                queuez::SessionState& after) noexcept {
    after = before;
    // The pair names one character, so with none selected there is nothing to publish yet and the
    // first pick delivers it. That is an ordinary boot state, not a failure.
    if (state::account::selected_character_soid(state::account_snapshot()) == 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=queuez stage=banner result=skip reason=unselected");
        return false;
    }
    snapshot::Prepared prepared{};
    // The unsolicited pair is the family's first delivery, so it carries the full-snapshot flag.
    if (!snapshot::prepare_banner(
            scratch, familyRootSoid, queuez::kInitialFamilyVersion, 0, prepared)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner result=fail reason=prepare");
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    if (!queuez_frame::append(scratch,
                              prepared.family,
                              prepared.rawClearSize,
                              prepared.compressedClearSize,
                              key,
                              nonce,
                              response,
                              written)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner result=fail reason=frame");
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    // The Client now holds this pair, so the ladder owns it. Without this an unsubscribe leaves
    // family zero unrecorded and the next pick has no previous record to release.
    const std::uint64_t delivered =
        state::account::selected_character_soid(state::account_snapshot());
    if (!after.family0Active && delivered != 0) {
        after.family0Active = true;
        after.family0Character = delivered;
        after.family0Version = queuez::kInitialFamilyVersion;
    }
    queuez_report::push("banner",
                        prepared.family.type,
                        objectCount,
                        written - beforeBytes,
                        queuez_report::kNoRecordOutcome);
    return true;
}

/**
 * Appends the family-zero move that follows an opcode-504 pick.
 * The Client holds the objIdx-1 buffer for one character at a time, allocated from the character
 * the anchor names, so the pair moves with the pick or the banner keeps the old emblem.
 * @param scratch Lock-owned transform buffers.
 * @param before Queuez state after the family-four move.
 * @param selectedCharacter Character the pick named.
 * @param key Active AES-GCM session key.
 * @param nonce Push-direction nonce, advanced only by a complete frame.
 * @param response Caller-owned output containing prior frames.
 * @param written Existing byte count, updated by a complete frame.
 * @param after Receives the state published once the frame is copied.
 * @return True when a frame went out and `after` carries the advanced ladder.
 */
bool append_banner_move_notification(Scratch& scratch,
                                     const queuez::SessionState& before,
                                     std::uint64_t selectedCharacter,
                                     std::span<const std::byte, state::kAesKeySize> key,
                                     std::array<std::byte, state::kBapNonceSize>& nonce,
                                     std::span<std::byte> response,
                                     std::size_t& written,
                                     queuez::SessionState& after) noexcept {
    bool publish = false;
    bool incremental = false;
    after = before;
    // A pick that names the character the pair already holds owes nothing, and so does a family
    // zero that has not had its first delivery yet.
    const char* reason = nullptr;
    if (!queuez::stage_family0_subscription(
            before, selectedCharacter, publish, incremental, after)) {
        reason = "stage";
    } else if (!publish) {
        reason = "unchanged";
    } else if (before.family4RootSoid == 0) {
        reason = "no_root";
    }
    if (reason != nullptr) {
        report_skip(reason);
        after = before;
        return false;
    }
    snapshot::Prepared prepared{};
    // A first delivery releases nothing: the Client holds no record for this family yet, so the
    // pair goes out as its own full snapshot instead of as a move off a previous character.
    if (!snapshot::prepare_banner(scratch,
                                  before.family4RootSoid,
                                  after.family0Version,
                                  incremental ? before.family0Character : 0,
                                  prepared)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner_move result=fail reason=prepare");
        after = before;
        return false;
    }
    const std::size_t objectCount = prepared.family.objects.size();
    const std::size_t beforeBytes = written;
    if (!queuez_frame::append(scratch,
                              prepared.family,
                              prepared.rawClearSize,
                              prepared.compressedClearSize,
                              key,
                              nonce,
                              response,
                              written)) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         "ev=queuez stage=banner_move result=fail reason=frame");
        after = before;
        return false;
    }
    middleware::secure_channel::advance_nonce(nonce);
    queuez_report::push("banner_move",
                        prepared.family.type,
                        objectCount,
                        written - beforeBytes,
                        queuez_report::kNoRecordOutcome);
    return true;
}

} // namespace sunrise::server::bap::encrypted::push
