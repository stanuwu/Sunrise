#include "mission_launch.h"

#include <Windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../state/activity/forced/activity_forced_destination.h"
#include "../../state/activity/runtime.h"
#include "../../state/build_data/activities/activity_catalog.h"
#include "../../state/build_data/runtime.h"
#include "../patterns/image_scan.h"
#include "mission_launch_options.h"

namespace sunrise::client::activity::mission_launch {
namespace {

using patterns::resolve_relative;
using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

// --- Director selection entry points ---

// Matches the member-record getter: the manager call, then the primary-session walk.
constexpr std::string_view kMemberRecordText = "40 53 48 83 EC 20 8B D9 E8 ? ? ? ? 33 D2";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kMemberRecord = signature<signature_length(kMemberRecordText)>(kMemberRecordText);
/** The record getter's first call is a near call to the session-manager getter. */
constexpr std::size_t kManagerCallOffset = 8;
/** A near call is its opcode and a 4-byte displacement. */
constexpr std::size_t kNearCallBytes = 5;
/** Intel's direct near-call opcode guards a decoded target against layout drift. */
constexpr std::byte kNearCallOpcode{0xE8};

// Matches the membership-revision test; the exact field offset is what makes it unique.
constexpr std::string_view kSessionReadyText = "83 B9 6C 08 00 00 00 0F 95 C0 C3";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kSessionReady = signature<signature_length(kSessionReadyText)>(kSessionReadyText);

// Matches the local-member readiness test by its two slot reads and their bound checks.
constexpr std::string_view kMemberReadyText =
    "4C 8B C1 48 63 89 3C E9 00 00 49 63 90 74 08 00 00 83 F9 1F 77 ? 83 FA 1F 77 ?";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kMemberReady = signature<signature_length(kMemberReadyText)>(kMemberReadyText);

// Matches the selection constructor by its argument moves.
constexpr std::string_view kConstructSelectionText =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC 20 41 0F B7 D8 8B FA";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kConstructSelection =
    signature<signature_length(kConstructSelectionText)>(kConstructSelectionText);

// Matches the selection validator. Its field offsets and the no-name sentinel separate it
// from its twin.
constexpr std::string_view kSelectionValidText =
    "0F B6 11 B0 01 80 FA FF 75 ? 66 83 79 02 FF 75 ? 83 79 08 FF 75 ? 66 83 79 04 FF 75 ? "
    "48 8B 41 10 48 FF C8 48 83 F8 FD 76 ? 38 51 3C 75 ? 81 79 40 C5 9D 1C 81";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kSelectionValid =
    signature<signature_length(kSelectionValidText)>(kSelectionValidText);

// Matches the activity name getter. The record's name field at +0x68 separates it from its
// twin.
constexpr std::string_view kActivityNameText =
    "48 89 5C 24 ? 57 48 83 EC 20 0F B7 F9 33 DB E8 ? ? ? ? 84 C0 74 ? E8 ? ? ? ? 0F B7 D7 "
    "48 8B C8 4C 8B 00 41 FF 90 ? ? ? ? 48 8B C8 48 85 C0 74 ? 48 8B 40 68 48 85 C0 74 ? "
    "48 8D 59 68";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kActivityName = signature<signature_length(kActivityNameText)>(kActivityNameText);

// Matches one call site of the clear RPC. The RPC body is byte-identical to its twin.
constexpr std::string_view kClearCallSiteText =
    "E8 ? ? ? ? 48 8D 54 24 ? 33 C9 E8 ? ? ? ? 48 8B 9C 24";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kClearCallSite = signature<signature_length(kClearCallSiteText)>(kClearCallSiteText);

// Matches the set-selection RPC by its 0x160-byte frame and stack cookie.
constexpr std::string_view kSetSelectionText =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 81 EC 60 01 00 00 48 8B 05 ? ? ? ? 48 33 C4 "
    "48 89 84 24 ? ? ? ? 48 8B FA";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kSetSelection = signature<signature_length(kSetSelectionText)>(kSetSelectionText);

// Matches the commit RPC through its four-argument dispatch and the block after its return.
constexpr std::string_view kCommitSelectionText =
    "89 4C 24 ? 48 83 EC 38 E8 ? ? ? ? F6 00 02 74 ? 48 8D 0D ? ? ? ? 41 B9 04 00 00 00 "
    "48 89 4C 24 ? 4C 8D 44 24 ? 48 8B C8 48 8D 15 ? ? ? ? E8 ? ? ? ? 48 83 C4 38 C3 "
    "48 8D 4C 24";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kCommitSelection =
    signature<signature_length(kCommitSelectionText)>(kCommitSelectionText);

// --- Native layouts, as far as the launch path reads them ---

/** Local member slot of one group session. */
constexpr std::size_t kSessionLocalMemberOffset = 0xE93C;
/** Lifecycle state of one group session. */
constexpr std::size_t kSessionStateOffset = 0x1AEF8;
/** One group session's stride inside the manager. */
constexpr std::size_t kSessionStride = 0x1C8A0;

/** One group session. Only the two fields the launch reads are named. */
struct GroupSession {
    std::array<std::byte, kSessionLocalMemberOffset> opaque00{};
    std::int32_t localMember{};
    std::array<std::byte, kSessionStateOffset - kSessionLocalMemberOffset - sizeof(std::int32_t)>
        opaque01{};
    std::int32_t state{};
    std::array<std::byte, kSessionStride - kSessionStateOffset - sizeof(std::int32_t)> opaque02{};
};

static_assert(offsetof(GroupSession, localMember) == kSessionLocalMemberOffset);
static_assert(offsetof(GroupSession, state) == kSessionStateOffset);
static_assert(sizeof(GroupSession) == kSessionStride);

/** Index of the primary session inside the manager. */
constexpr std::size_t kManagerPrimaryOffset = 0x10;
/** First session slot inside the manager. */
constexpr std::size_t kManagerSessionsOffset = 0x18;
/** Session slots the manager carries. */
constexpr std::size_t kSessionSlots = 4;

/** The group session manager, as far as the launch reads it. */
struct SessionManager {
    std::array<std::byte, kManagerPrimaryOffset> opaque00{};
    std::int32_t primary{};
    std::uint32_t opaque01{};
    std::array<GroupSession, kSessionSlots> sessions{};
};

static_assert(offsetof(SessionManager, primary) == kManagerPrimaryOffset);
static_assert(offsetof(SessionManager, sessions) == kManagerSessionsOffset);

/** Launch state byte of one member record. */
constexpr std::size_t kMemberLaunchStateOffset = 0xA33;

/** One session member record, as far as the launch reads it. */
struct MemberRecord {
    std::array<std::byte, kMemberLaunchStateOffset> opaque00{};
    std::uint8_t launchState{};
};

static_assert(offsetof(MemberRecord, launchState) == kMemberLaunchStateOffset);

/** Size of the selection the constructor fills and the RPCs consume. */
constexpr std::size_t kSelectionBytes = 0x120;
/** Selection fields past the destination that the launch never reads. */
constexpr std::size_t kSelectionTailBytes = kSelectionBytes - 6;

/** One activity selection. The constructor writes it whole; the launch reads the head. */
struct alignas(16) Selection {
    std::uint8_t kind{};
    std::uint8_t opaque00{};
    std::int16_t source{};
    std::int16_t destination{};
    std::array<std::byte, kSelectionTailBytes> opaque01{};
};

static_assert(offsetof(Selection, source) == 2);
static_assert(offsetof(Selection, destination) == 4);
static_assert(sizeof(Selection) == kSelectionBytes);

// --- Launch policy ---

/** Session states that accept a launch: joined through in-activity. */
constexpr std::int32_t kSessionStateFirstReady = 4;
constexpr std::int32_t kSessionStateLastReady = 9;
/** Member slots one session addresses. */
constexpr std::int32_t kMemberSlots = 12;
/** Launch states from here on already hold a pending launch. */
constexpr std::uint8_t kLaunchStatePending = 3;
/** Selection kind the constructor writes for an activity pick. */
constexpr std::uint8_t kSelectionKindActivity = 0;
/** Reason the Director passes to the constructor for a plain activity pick. */
constexpr std::uint32_t kSelectionReason = 0;
/** Selection slot the Director fills for an orbit launch. */
constexpr std::uint8_t kSelectionSlot = 0;
/** Commit argument the Director sends for its own launch. */
constexpr std::int32_t kCommitLaunch = 1;
/** Boot-flow step `setup:orbit`. */
constexpr std::int32_t kSetupOrbitStep = 29;
/** Boot-flow step `activity:in_world`. */
constexpr std::int32_t kInWorldStep = 38;
/** An arrival not confirmed inside this window reports as timed out. */
constexpr std::uint64_t kArrivalTimeoutMs = 120'000;

using GetManager = SessionManager*(__fastcall*)() noexcept;
using SessionTest = bool(__fastcall*)(const GroupSession*) noexcept;
using GetMemberRecord = MemberRecord*(__fastcall*)(std::uint32_t) noexcept;
using ConstructSelection = Selection*(__fastcall*)(Selection*,
                                                   std::uint32_t,
                                                   std::int16_t) noexcept;
using SelectionValid = bool(__fastcall*)(const Selection*) noexcept;
using ActivityName = const char*(__fastcall*)(std::int16_t) noexcept;
using ClearSelections = void(__fastcall*)() noexcept;
using SetSelection = void(__fastcall*)(std::uint8_t, const Selection*) noexcept;
using CommitSelection = void(__fastcall*)(std::int32_t) noexcept;

/** Unowned entry points of the Director's selection path. */
struct Natives {
    GetManager manager{};
    SessionTest sessionReady{};
    SessionTest memberReady{};
    GetMemberRecord memberRecord{};
    ConstructSelection construct{};
    SelectionValid valid{};
    ActivityName name{};
    ClearSelections clear{};
    SetSelection select{};
    CommitSelection commit{};
};

Natives g_natives{};
std::atomic_bool g_resolved{false};

SRWLOCK g_lock{SRWLOCK_INIT};
Snapshot g_state{};
std::uint64_t g_requestedAt{};
// Game-thread-only receipt state, scoped to a successfully submitted request.
state::activity::SessionBinding g_previousSession{};
bool g_leftOrbit{};
ManualScratch g_manualScratch{}; // Game-frame owner only, outside the UI arena and native stack.

/** @return The newest joined session binding, or an empty one when none is joined. */
state::activity::SessionBinding newest_joined_session() noexcept {
    std::array<state::activity::SessionRosterRow, state::activity::kSessionCapacity> rows{};
    std::size_t count{};
    state::activity::SessionBinding newest{};
    if (!state::activity::snapshot_session_roster(rows, count)) {
        return newest;
    }
    for (const auto& row : std::span(rows).first(count)) {
        if (row.joined && row.binding.createdRevision > newest.createdRevision) {
            newest = row.binding;
        }
    }
    return newest;
}

/** Records the outcome of the pending request and logs it. @param status The outcome. */
void finish(Status status) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    g_state.status = status;
    g_state.busy = status == Status::queued;
    const auto state = g_state;
    ReleaseSRWLockExclusive(&g_lock);
    std::array<char, 256> line{};
    const int size =
        state.manual
            ? std::snprintf(line.data(),
                            line.size(),
                            "ev=mission_launch activity=%u manual=1 destination=%.*s bubble=%u "
                            "slice=%u spawn=0x%08X status=%u",
                            state.index,
                            static_cast<int>(destination_name(state.destination).size()),
                            state.destination.packageName.data(),
                            state.destination.bubble,
                            state.destination.sliceSet,
                            state.destination.hasSpawnSetHash ? state.destination.spawnSetHash
                                                              : forced::kAbsentSpawnSetHash,
                            static_cast<unsigned>(status))
            : std::snprintf(line.data(),
                            line.size(),
                            "ev=mission_launch activity=%u manual=0 status=%u",
                            state.index,
                            static_cast<unsigned>(status));
    if (size > 0 && static_cast<std::size_t>(size) < line.size()) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::info,
                         {line.data(), static_cast<std::size_t>(size)});
    }
}

/**
 * Confirms the client arrived in the requested destination.
 * @param state The queued request.
 * @param rows The published activity catalog.
 * @return True when the newest joined session binds the requested destination.
 */
[[nodiscard]] bool
arrived(const Snapshot& state,
        std::span<const state::build_data::activities::Definition> rows) noexcept {
    const auto session = newest_joined_session();
    const auto& destination = session.destination;
    if (session.sessionId == 0 || state::activity::same_binding(session, g_previousSession)
        || state.index >= rows.size()
        || destination.activityIndex != static_cast<std::int16_t>(state.index)) {
        return false;
    }
    const std::string_view name =
        state.manual ? destination_name(state.destination) : rows[state.index].name();
    if (destination.packageNameLength != name.size()
        || std::memcmp(destination.packageName.data(), name.data(), name.size()) != 0) {
        return false;
    }
    if (!state.manual) {
        return true;
    }
    // A bubble override always publishes a spawn override, the no-name sentinel when none was
    // chosen, so the spawn check follows the bubble one.
    const std::uint32_t spawn = state.destination.hasSpawnSetHash ? state.destination.spawnSetHash
                                                                  : forced::kAbsentSpawnSetHash;
    return (!state.destination.hasBubble
            || (destination.hasArrivalBubbleOverride
                && destination.arrivalBubbleOverride == state.destination.bubble))
           && (!state.destination.hasSliceSet
               || (destination.hasSliceSetOverride
                   && destination.sliceSetOverride == state.destination.sliceSet))
           && (!state.destination.hasBubble
               || (destination.hasSpawnSetOverride && destination.spawnSetOverride == spawn));
}

/**
 * Submits one validated request through the Director's own selection path.
 * @param state The request to submit.
 * @param rows The published activity catalog.
 * @return The outcome; `queued` when the commit was sent.
 */
[[nodiscard]] Status
submit(const Snapshot& state,
       std::span<const state::build_data::activities::Definition> rows) noexcept {
    const Natives natives = g_natives;
    SessionManager* const manager = natives.manager();
    if (manager == nullptr || manager->primary < 0
        || manager->primary >= static_cast<std::int32_t>(kSessionSlots)) {
        return Status::notReady;
    }
    const GroupSession& session = manager->sessions[static_cast<std::size_t>(manager->primary)];
    if (session.state < kSessionStateFirstReady || session.state > kSessionStateLastReady
        || !natives.sessionReady(&session) || !natives.memberReady(&session)
        || session.localMember < 0 || session.localMember >= kMemberSlots) {
        return Status::notReady;
    }
    const MemberRecord* const record =
        natives.memberRecord(static_cast<std::uint32_t>(session.localMember));
    if (record == nullptr || record->launchState >= kLaunchStatePending) {
        return Status::notReady;
    }
    const auto index = static_cast<std::int16_t>(state.index);
    const char* const package = natives.name(index);
    const auto& expected = rows[state.index].package;
    if (package == nullptr || std::strncmp(package, expected.data(), expected.size()) != 0) {
        return Status::descriptorRejected;
    }
    Selection selection{};
    if (natives.construct(&selection, kSelectionReason, index) != &selection
        || selection.kind != kSelectionKindActivity || selection.source != index
        || selection.destination != index || !natives.valid(&selection)) {
        return Status::descriptorRejected;
    }
    g_previousSession = newest_joined_session();
    g_leftOrbit = false;
    // Reuse the standalone override service only after all native readiness/descriptor checks.
    if (state.manual && !forced::publish(state.destination)) {
        return Status::manualRejected;
    }
    natives.clear();
    natives.select(kSelectionSlot, &selection);
    natives.commit(kCommitLaunch);
    return Status::queued;
}

} // namespace

/** Finds every selection entry point, or none. */
bool install() noexcept {
    std::byte* const record = scan_main_image_unique(kMemberRecord, "mission_launch_member_record");
    std::byte* const sessionReady =
        scan_main_image_unique(kSessionReady, "mission_launch_session_ready");
    std::byte* const memberReady =
        scan_main_image_unique(kMemberReady, "mission_launch_member_ready");
    std::byte* const construct =
        scan_main_image_unique(kConstructSelection, "mission_launch_construct_selection");
    std::byte* const valid =
        scan_main_image_unique(kSelectionValid, "mission_launch_selection_valid");
    std::byte* const name = scan_main_image_unique(kActivityName, "mission_launch_activity_name");
    std::byte* const clearSite =
        scan_main_image_unique(kClearCallSite, "mission_launch_clear_call_site");
    std::byte* const select = scan_main_image_unique(kSetSelection, "mission_launch_set_selection");
    std::byte* const commit =
        scan_main_image_unique(kCommitSelection, "mission_launch_commit_selection");
    if (record == nullptr || sessionReady == nullptr || memberReady == nullptr
        || construct == nullptr || valid == nullptr || name == nullptr || clearSite == nullptr
        || select == nullptr || commit == nullptr || record[kManagerCallOffset] != kNearCallOpcode
        || clearSite[0] != kNearCallOpcode) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=mission_launch stage=install result=fail reason=target");
        return false;
    }
    Natives found{};
    found.manager = reinterpret_cast<GetManager>(resolve_relative(
        record + kManagerCallOffset + 1, record + kManagerCallOffset + kNearCallBytes));
    found.sessionReady = reinterpret_cast<SessionTest>(sessionReady);
    found.memberReady = reinterpret_cast<SessionTest>(memberReady);
    found.memberRecord = reinterpret_cast<GetMemberRecord>(record);
    found.construct = reinterpret_cast<ConstructSelection>(construct);
    found.valid = reinterpret_cast<SelectionValid>(valid);
    found.name = reinterpret_cast<ActivityName>(name);
    found.clear = reinterpret_cast<ClearSelections>(
        resolve_relative(clearSite + 1, clearSite + kNearCallBytes));
    found.select = reinterpret_cast<SetSelection>(select);
    found.commit = reinterpret_cast<CommitSelection>(commit);
    g_natives = found;
    g_resolved.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=mission_launch stage=install result=ok");
    return true;
}

void uninstall() noexcept {
    g_resolved.store(false, std::memory_order_release);
    g_natives = {};
}

Snapshot snapshot() noexcept {
    AcquireSRWLockShared(&g_lock);
    const auto result = g_state;
    ReleaseSRWLockShared(&g_lock);
    return result;
}

/** Queues an orbit launch of one catalog row. @return False while a request is pending. */
bool request(std::uint16_t index) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_state.busy) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_state = {Status::requested, index, true};
    g_requestedAt = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Queues a launch with its arrival override. @return False while a request is pending. */
bool request_manual(std::uint16_t index, const forced::ForcedDestination& destination) noexcept {
    AcquireSRWLockExclusive(&g_lock);
    if (g_state.busy) {
        ReleaseSRWLockExclusive(&g_lock);
        return false;
    }
    g_state = {Status::requested, index, true, true, destination};
    g_requestedAt = GetTickCount64();
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

/** Validates the pending request against the catalog and the client, then submits it. */
void poll(std::int32_t step) noexcept {
    const auto state = snapshot();
    if (!state.busy) {
        return;
    }
    if (!g_resolved.load(std::memory_order_acquire)) {
        finish(Status::nativeUnavailable);
        return;
    }
    const auto rows = state::build_data::activities::entries();
    if (state.status == Status::queued) {
        // Requests are immutable until this owner completes them; the timestamp follows that lock.
        AcquireSRWLockShared(&g_lock);
        const auto started = g_requestedAt;
        ReleaseSRWLockShared(&g_lock);
        g_leftOrbit = g_leftOrbit || step != kSetupOrbitStep;
        if (g_leftOrbit && step == kInWorldStep && arrived(state, rows)) {
            finish(Status::arrived);
        } else if (GetTickCount64() - started > kArrivalTimeoutMs) {
            finish(Status::timedOut);
        }
        return;
    }
    if (rows.empty()) {
        finish(Status::catalogUnavailable);
        return;
    }
    if (state.index >= rows.size() || rows[state.index].name().empty()) {
        finish(Status::entryUnavailable);
        return;
    }
    state::build_data::scenarios::Definition layout{};
    if (!state::build_data::find_scenario_layout(rows[state.index].name(), layout)) {
        finish(Status::entryUnavailable);
        return;
    }
    if (state.manual) {
        if (!manual_transport_valid(state.index, state.destination, rows)
            || validate_manual(state.destination, g_manualScratch) != ManualError::none) {
            finish(Status::manualRejected);
            return;
        }
    } else {
        forced::ForcedDestination effective{};
        forced::snapshot(effective);
        if (forced::active(effective) || forced::override_active()) {
            finish(Status::overrideActive);
            return;
        }
    }
    // A launch is only accepted from orbit. Leaving a world belongs to the native lifecycle.
    if (step != kSetupOrbitStep) {
        finish(Status::returnToOrbit);
        return;
    }
    finish(submit(state, rows));
}

/** @return A static UI message for one status. */
const char* description(Status status) noexcept {
    switch (status) {
    case Status::idle:
        return "Choose an activity, return to orbit, then launch.";
    case Status::requested:
        return "Checking the native launch request...";
    case Status::queued:
        return "Submitted to the Director. Waiting for the native activity transition.";
    case Status::arrived:
        return "Native activity arrival confirmed.";
    case Status::catalogUnavailable:
        return "Activity catalog is still being extracted.";
    case Status::entryUnavailable:
        return "This entry has no available direct-launch scenario.";
    case Status::overrideActive:
        return "An Activity override is active. Disable it before launching this selection.";
    case Status::returnToOrbit:
        return "Return to orbit through the Director, then click Launch again.";
    case Status::nativeUnavailable:
        return "Native launch entry points are unavailable in this client build.";
    case Status::notReady:
        return "The fireteam is not ready to launch. Wait in orbit and try again.";
    case Status::descriptorRejected:
        return "The native client rejected this activity selection.";
    case Status::timedOut:
        return "No arrival confirmation was received. Check the Director before retrying.";
    case Status::manualRejected:
        return "The manual destination, bubble, slice, spawn or native launch route is no longer "
               "valid. Review the selection.";
    }
    return "Launch status unavailable.";
}

} // namespace sunrise::client::activity::mission_launch
