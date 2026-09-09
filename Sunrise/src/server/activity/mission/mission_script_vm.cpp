#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>

#include "mission_script_vm_internal.h"

namespace sunrise::server::activity::mission::lua_vm {
namespace detail {

struct VmAccess final {
    [[nodiscard]] static Impl& get(Vm& vm) noexcept {
        return *std::launder(reinterpret_cast<Impl*>(vm.storage_.data()));
    }

    [[nodiscard]] static const Impl& get(const Vm& vm) noexcept {
        return *std::launder(reinterpret_cast<const Impl*>(vm.storage_.data()));
    }
};

} // namespace detail
namespace {

using detail::CallFrame;
using detail::Candidate;
using detail::Handler;
using detail::Impl;
using detail::impl_from_state;
using detail::raise_lua_error;
using detail::VmAccess;

static_assert(sizeof(Impl) <= kVmStorageByteCapacity);
static_assert(alignof(Impl) <= alignof(std::max_align_t));

/** Reads only an already-interned Lua string; coercing arbitrary errors can allocate. */
[[nodiscard]] const char* existing_error(lua_State* state, const char* fallback) noexcept {
    return lua_type(state, -1) == LUA_TSTRING ? lua_tostring(state, -1) : fallback;
}

/** Instructions between budget checks. Every lua_sethook call must charge this same count. */
constexpr std::uint32_t kHookInterval = 100;

/** Charges the fixed interval against the frame budget and errors once it cannot be paid. */
void instruction_hook(lua_State* state, lua_Debug*) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr || impl->frame == nullptr) {
        raise_lua_error(state, "instruction_budget");
    }
    if (impl->frame->remainingInstructions <= kHookInterval) {
        impl->frame->remainingInstructions = 0;
        impl->frame->instructionExceeded = true;
        luaL_error(state, "instruction_budget");
    }
    impl->frame->remainingInstructions -= kHookInterval;
}

/** Takes a registry reference to one named entry; nil is allowed, a non-function is refused. */
[[nodiscard]] bool capture_handler(lua_State* state, int table, const char* key, int& output) {
    lua_pushstring(state, key);
    lua_rawget(state, table);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        output = LUA_NOREF;
        return true;
    }
    if (!lua_isfunction(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    output = luaL_ref(state, LUA_REGISTRYINDEX);
    return output != LUA_NOREF && output != LUA_REFNIL;
}

/** Captures the optional generated mission-state declaration without retaining its Lua table. */
[[nodiscard]] bool capture_initial_state(lua_State* state, int program, Impl& impl) noexcept {
    impl.initialStateRegion = -1;
    impl.hasInitialState = false;
    lua_pushliteral(state, "initial_state");
    lua_rawget(state, program);
    if (lua_isnil(state, -1)) {
        lua_pop(state, 1);
        return true;
    }
    if (!lua_istable(state, -1)) {
        lua_pop(state, 1);
        return false;
    }
    lua_pushliteral(state, "region_index");
    lua_rawget(state, -2);
    if (!lua_isinteger(state, -1)) {
        lua_pop(state, 2);
        return false;
    }
    const lua_Integer region = lua_tointeger(state, -1);
    if (region < 0
        || static_cast<std::uint64_t>(region)
               > static_cast<std::uint64_t>((std::numeric_limits<std::int32_t>::max)())) {
        lua_pop(state, 2);
        return false;
    }
    impl.initialStateRegion = static_cast<std::int32_t>(region);
    impl.hasInitialState = true;
    lua_pop(state, 2);
    return true;
}

// One callback name per EventKind, in EventKind order. The index is the kind.
inline constexpr std::array<const char*, host::kEventKindCount> kEventHandlerNames{{
    "on_event_sensor_sense_updated",
    "on_event_client_state_changed",
    "on_event_client_message_received",
    "on_event_auth_state_committed",
    "on_event_auth_state_transport_staged",
    "on_event_auth_state_canceled",
    "on_event_incident_received",
    "on_event_incident_queued",
    "on_event_incident_transport_staged",
    "on_event_incident_canceled",
    "on_event_incident_refused",
    "on_event_scriptable_override_committed",
    "on_event_scriptable_override_transport_staged",
    "on_event_scriptable_override_canceled",
    "on_event_operator_refused",
    "on_event_timer_elapsed",
    "on_event_effect_result",
    "on_event_phase_entered",
    "on_event_trigger_entered",
    "on_event_trigger_exited",
    "on_event_squad_state",
    "on_event_entity_spawned",
    "on_event_entity_died",
    "on_event_scene_finished",
    "on_event_objective_progress",
    "on_event_entity_slots_requested",
    "on_event_session_joined",
    "on_event_session_left",
    "on_event_player_trigger",
    "on_event_cinematic_started",
    "on_event_cinematic_terminated",
    "on_event_ghost_link_state",
    "on_event_actor_path_state",
    "on_event_object_interacted",
    "on_event_cinematic_skip_requested",
    "on_event_fireteam_state",
    "on_event_object_state",
    "on_event_damage_state",
}};

static_assert([] {
    for (const auto* name : kEventHandlerNames)
        if (name == nullptr) return false;
    return true;
}());

/** Captures immutable callback references while table access remains inside lua_pcall. */
[[nodiscard]] int capture_program(lua_State* state) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr || lua_type(state, 1) != LUA_TTABLE) {
        return luaL_error(state, "mission chunk must return one program table");
    }
    lua_pushliteral(state, "on_event");
    lua_rawget(state, 1);
    const bool genericEventHandler = !lua_isnil(state, -1);
    lua_pop(state, 1);
    if (genericEventHandler) {
        return luaL_error(state, "on_event is unsupported; use one on_event_<type> callback");
    }
    if (!capture_initial_state(state, 1, *impl)) {
        return luaL_error(state,
                          "initial_state must be a generated state with an integer region_index");
    }
    if (!capture_handler(state, 1, "on_start", impl->startReference)
        || !capture_handler(state, 1, "on_load", impl->loadReference)) {
        return luaL_error(state, "mission entries must be functions or nil");
    }
    for (std::size_t index = 0; index < kEventHandlerNames.size(); ++index) {
        if (!capture_handler(state, 1, kEventHandlerNames[index], impl->eventReferences[index])) {
            return luaL_error(state, "mission event entries must be functions or nil");
        }
    }
    return 0;
}

void preserve_failure_and_close(Impl& impl) noexcept {
    const std::array<char, 256> error = impl.lastError;
    detail::destroy_state(impl);
    impl.lastError = error;
    impl.faulted = true;
}

[[nodiscard]] bool contains_error(const Impl& impl, std::string_view marker) noexcept {
    const std::string_view error(impl.lastError.data());
    return error.find(marker) != std::string_view::npos;
}

[[nodiscard]] CallStatus
failed_call_status(const Impl& impl, const CallFrame& frame, int luaStatus) noexcept {
    if (luaStatus == LUA_ERRMEM || frame.memoryExceeded) {
        return CallStatus::outOfMemory;
    }
    return contains_error(impl, "instruction_budget") ? CallStatus::instructionBudget
                                                      : CallStatus::scriptError;
}

// Past three quarters of the arena a step cannot keep up, so a full collection runs; below
// that the step is charged 64 KiB at a time.
constexpr std::size_t kCollectionPressureBytes = (kArenaByteCapacity * 3U) / 4U;
constexpr int kCollectionStepKilobytes = 64;

/** Advances collection only after Lua has left a protected transaction. */
void collect_after_transaction(Impl& impl) noexcept {
    if (impl.state != nullptr) {
        const int operation =
            impl.arena.used >= kCollectionPressureBytes ? LUA_GCCOLLECT : LUA_GCSTEP;
        const int amount = operation == LUA_GCSTEP ? kCollectionStepKilobytes : 0;
        static_cast<void>(lua_gc(impl.state, operation, amount));
        static_cast<void>(lua_gc(impl.state, LUA_GCSTOP, 0));
        ++impl.collections;
    }
}

[[nodiscard]] bool same_key(const StateKey& left, const StateKey& right) noexcept {
    return left.length == right.length && left.bytes == right.bytes;
}

[[nodiscard]] bool same_value(const VariableValue& left, const VariableValue& right) noexcept {
    return left.stringValue == right.stringValue && left.integerValue == right.integerValue
           && left.realValue == right.realValue && left.stringLength == right.stringLength
           && left.kind == right.kind && left.booleanValue == right.booleanValue;
}

[[nodiscard]] bool same_variable(const ScriptVariable& left, const ScriptVariable& right) noexcept {
    return same_key(left.key, right.key) && same_value(left.value, right.value);
}

[[nodiscard]] bool same_timer(const MissionTimer& left, const MissionTimer& right) noexcept {
    return same_key(left.key, right.key) && left.deadlineTick == right.deadlineTick
           && left.sequence == right.sequence;
}

/** True when the candidate differs from committed durable state in any field it may change. */
[[nodiscard]] bool durable_delta(const Impl& impl, const Candidate& candidate) noexcept {
    if ((candidate.phaseChanged && candidate.phase != impl.phase)
        || candidate.variableCount != impl.variableCount || candidate.timerCount != impl.timerCount
        || candidate.nextTimerSequence != impl.nextTimerSequence
        || candidate.nextIntentKey != impl.nextIntentKey) {
        return true;
    }
    for (std::size_t index = 0; index < candidate.variableCount; ++index) {
        if (!same_variable(candidate.variables[index], impl.variables[index])) {
            return true;
        }
    }
    for (std::size_t index = 0; index < candidate.timerCount; ++index) {
        if (!same_timer(candidate.timers[index], impl.timers[index])) {
            return true;
        }
    }
    return false;
}

void initialize_candidate(const Impl& impl, Candidate& candidate) noexcept {
    candidate.phase = impl.phase;
    candidate.variables = impl.variables;
    candidate.timers = impl.timers;
    candidate.variableCount = impl.variableCount;
    candidate.timerCount = impl.timerCount;
    candidate.nextTimerSequence = impl.nextTimerSequence;
    candidate.nextIntentKey = impl.nextIntentKey;
}

/** Consumes only the exact active timer represented by an internal elapsed event. */
[[nodiscard]] bool consume_elapsed_timer(Candidate& candidate, const host::Event& event) noexcept {
    if (event.kind != host::EventKind::timerElapsed) {
        return true;
    }
    for (std::size_t index = 0; index < candidate.timerCount; ++index) {
        const MissionTimer& timer = candidate.timers[index];
        if (timer.sequence != event.timerSequence || timer.deadlineTick != event.timerDeadlineTick
            || !same_key(timer.key, event.timerName)) {
            continue;
        }
        for (std::size_t move = index + 1; move < candidate.timerCount; ++move) {
            candidate.timers[move - 1] = candidate.timers[move];
        }
        --candidate.timerCount;
        candidate.timers[candidate.timerCount] = {};
        return true;
    }
    return false;
}

/** Publishes one callback candidate without allowing its revision to wrap. */
[[nodiscard]] CallStatus commit_candidate(Impl& impl, Candidate& candidate) noexcept {
    const bool phaseChanged = candidate.phaseChanged && candidate.phase != impl.phase;
    const bool stateChanged = durable_delta(impl, candidate);
    if ((stateChanged || !candidate.intents.empty())
        && impl.stateRevision == (std::numeric_limits<std::uint64_t>::max)()) {
        detail::set_error(impl, "mission state revision is exhausted");
        ++impl.refusedCallbacks;
        impl.faulted = true;
        return CallStatus::scriptError;
    }
    if (impl.outboxRead != 0) {
        impl.outbox.erase(impl.outbox.begin(),
                          impl.outbox.begin() + static_cast<std::ptrdiff_t>(impl.outboxRead));
        impl.outboxRead = 0;
    }
    if (candidate.intents.size() > impl.outbox.max_size() - impl.outbox.size()) {
        detail::set_error(impl, "mission outbox allocation failed");
        ++impl.refusedCallbacks;
        impl.faulted = true;
        return CallStatus::outOfMemory;
    }
    try {
        impl.outbox.reserve(impl.outbox.size() + candidate.intents.size());
    } catch (const std::bad_alloc&) {
        detail::set_error(impl, "mission outbox allocation failed");
        ++impl.refusedCallbacks;
        impl.faulted = true;
        return CallStatus::outOfMemory;
    }
    for (Intent& intent : candidate.intents) {
        impl.outbox.push_back(std::move(intent));
    }
    if (phaseChanged) {
        impl.phase = candidate.phase;
    }
    if (stateChanged) {
        impl.variables = candidate.variables;
        impl.timers = candidate.timers;
        impl.variableCount = candidate.variableCount;
        impl.timerCount = candidate.timerCount;
        impl.nextTimerSequence = candidate.nextTimerSequence;
        impl.nextIntentKey = candidate.nextIntentKey;
    }
    if (stateChanged || !candidate.intents.empty()) {
        ++impl.stateRevision;
    }
    impl.lastError = {};
    ++impl.committedCallbacks;
    return CallStatus::committed;
}

/** Runs one callback under the instruction budget, then commits its candidate or faults the vm. */
[[nodiscard]] CallStatus invoke(Impl& impl,
                                Handler handler,
                                const host::Event* event,
                                const host::ClientMessageSnapshot* clientMessage,
                                std::uint64_t now) noexcept {
    if (!impl.active || impl.state == nullptr || impl.faulted) {
        return CallStatus::inactive;
    }
    CallFrame frame{};
    initialize_candidate(impl, frame.candidate);
    if (event != nullptr && !consume_elapsed_timer(frame.candidate, *event)) {
        detail::set_error(impl, "mission timer event no longer matches authoritative state");
        ++impl.refusedCallbacks;
        impl.faulted = true;
        return CallStatus::scriptError;
    }
    frame.handler = handler;
    frame.event = event;
    frame.clientMessage = clientMessage;
    frame.now = now;
    impl.frame = &frame;
    ++impl.callbacks;

    lua_sethook(impl.state, &instruction_hook, LUA_MASKCOUNT, kHookInterval);
    lua_pushcfunction(impl.state, &detail::protected_callback);
    const int result = lua_pcall(impl.state, 0, 1, 0);
    lua_sethook(impl.state, nullptr, 0, 0);
    impl.frame = nullptr;

    if (result != LUA_OK) {
        detail::set_error(
            impl, existing_error(impl.state, "mission callback failed with a non-string error"));
        lua_settop(impl.state, 0);
        collect_after_transaction(impl);
        ++impl.refusedCallbacks;
        impl.faulted = true;
        return failed_call_status(impl, frame, result);
    }
    const bool handled = lua_toboolean(impl.state, -1) != 0;
    lua_settop(impl.state, 0);
    collect_after_transaction(impl);
    if (!handled) {
        if (durable_delta(impl, frame.candidate)) {
            const CallStatus committed = commit_candidate(impl, frame.candidate);
            return committed == CallStatus::committed ? CallStatus::noHandler : committed;
        }
        return CallStatus::noHandler;
    }
    return commit_candidate(impl, frame.candidate);
}

} // namespace

Vm::Vm() noexcept {
    // No arena block yet. An activity slot with no open program holds none.
    static_cast<void>(::new (storage_.data()) detail::Impl{});
}

Vm::~Vm() noexcept {
    detail::Impl& impl = detail::VmAccess::get(*this);
    detail::destroy_state(impl);
    impl.~Impl();
}

/** Replaces the bound view of a program that is already open and running. */
bool rebind(Vm& vm, const ProgramIdentity& identity, const DefinitionApi& definitions) noexcept {
    Impl& impl = VmAccess::get(vm);
    if (impl.state == nullptr || !impl.active || impl.faulted) {
        return false;
    }
    impl.identity = identity;
    impl.definitions = definitions;
    return true;
}

/** Compiles and validates one build-bound text chunk in a fresh arena. */
OpenStatus open(Vm& vm,
                const ProgramIdentity& identity,
                const DefinitionApi& definitions,
                std::span<const char> source) noexcept {
    Impl& impl = VmAccess::get(vm);
    detail::destroy_state(impl);
    impl.arenaBytesAfterClose = 0;
    if (source.empty() || source.size() > kSourceByteCapacity) {
        detail::set_error(impl, "mission source is empty or exceeds 128 KiB");
        impl.faulted = true;
        return OpenStatus::sourceTooLarge;
    }
    // The build id is the 7-byte "sha256:" prefix plus 64 hex digits, 71 characters total.
    constexpr std::string_view kSdkBuildIdPrefix = "sha256:";
    constexpr std::size_t kSdkBuildIdLength = kSdkBuildIdPrefix.size() + 64;
    if (identity.sdkBuildId.back() != '\0'
        || std::strlen(identity.sdkBuildId.data()) != kSdkBuildIdLength
        || std::string_view(identity.sdkBuildId.data(), kSdkBuildIdPrefix.size())
               != kSdkBuildIdPrefix
        || identity.activityId.back() != '\0' || identity.activityId.front() == '\0'
        || definitions.resolveSquadRow == nullptr || definitions.resolveSquadId == nullptr
        || definitions.resolveSceneRow == nullptr || definitions.resolveSceneId == nullptr
        || definitions.resolveSlotRow == nullptr || definitions.resolveSlotId == nullptr
        || definitions.resolveSenseSlot == nullptr
        || definitions.resolveActivityMessageRow == nullptr
        || definitions.resolveActivityMessageId == nullptr
        || definitions.resolveActivityMessageName == nullptr
        || definitions.resolveActivityMessageFieldRow == nullptr
        || definitions.resolveActivityMessageFieldIndex == nullptr
        || definitions.resolveActivityBinding == nullptr
        || definitions.activityBindingTagCount == nullptr
        || definitions.resolveActivityBindingTag == nullptr
        || definitions.activityBindingLocatorCount == nullptr
        || definitions.resolveActivityBindingLocator == nullptr || definitions.squadCount == nullptr
        || definitions.sceneCount == nullptr || definitions.slotCount == nullptr
        || definitions.activityMessageCount == nullptr) {
        detail::set_error(impl, "mission identity or SDK definition bridge is invalid");
        impl.faulted = true;
        return OpenStatus::invalidProgram;
    }
    impl.identity = identity;
    impl.definitions = definitions;
    if (!detail::arena_initialize(impl.arena)) {
        detail::set_error(impl, "mission arena block could not be taken from the heap");
        impl.faulted = true;
        return OpenStatus::outOfMemory;
    }
    impl.state = lua_newstate(&detail::arena_allocate, &impl.arena);
    if (impl.state == nullptr) {
        detail::set_error(impl, "Lua state exceeded its fixed arena");
        preserve_failure_and_close(impl);
        return OpenStatus::outOfMemory;
    }
    static_cast<void>(lua_gc(impl.state, LUA_GCSTOP, 0));
    if (!detail::initialize_sandbox(impl)) {
        const OpenStatus status =
            contains_error(impl, "memory") ? OpenStatus::outOfMemory : OpenStatus::runtimeError;
        preserve_failure_and_close(impl);
        return status;
    }
    const int compiled =
        luaL_loadbufferx(impl.state, source.data(), source.size(), identity.activityId.data(), "t");
    if (compiled != LUA_OK) {
        detail::set_error(
            impl, existing_error(impl.state, "mission compile failed with a non-string error"));
        const OpenStatus status =
            compiled == LUA_ERRMEM ? OpenStatus::outOfMemory : OpenStatus::compileError;
        preserve_failure_and_close(impl);
        return status;
    }

    CallFrame loadFrame{};
    loadFrame.remainingInstructions = kInitializationInstructionBudget;
    impl.frame = &loadFrame;
    lua_sethook(impl.state, &instruction_hook, LUA_MASKCOUNT, kHookInterval);
    const int executed = lua_pcall(impl.state, 0, 1, 0);
    lua_sethook(impl.state, nullptr, 0, 0);
    impl.frame = nullptr;
    if (executed != LUA_OK) {
        detail::set_error(
            impl,
            existing_error(impl.state, "mission initialization failed with a non-string error"));
        const OpenStatus status = executed == LUA_ERRMEM || loadFrame.memoryExceeded
                                      ? OpenStatus::outOfMemory
                                      : OpenStatus::runtimeError;
        preserve_failure_and_close(impl);
        return status;
    }
    lua_pushcfunction(impl.state, &capture_program);
    lua_insert(impl.state, -2);
    const int captured = lua_pcall(impl.state, 1, 0, 0);
    if (captured != LUA_OK) {
        detail::set_error(
            impl, existing_error(impl.state, "mission declaration failed with a non-string error"));
        const OpenStatus status =
            captured == LUA_ERRMEM ? OpenStatus::outOfMemory : OpenStatus::invalidProgram;
        preserve_failure_and_close(impl);
        return status;
    }
    lua_settop(impl.state, 0);
    impl.active = true;
    impl.faulted = false;
    return OpenStatus::ready;
}

/**
 * Publishes the current peer set.
 * It replaces the previous set outright, so a peer that left is gone on the next read.
 */
void publish_peers(Vm& vm, std::span<const PeerSession> peers) noexcept {
    Impl& impl = VmAccess::get(vm);
    const std::size_t count = (std::min)(peers.size(), impl.peers.size());
    impl.peers = {};
    std::copy_n(peers.begin(), count, impl.peers.begin());
    impl.peerCount = static_cast<std::uint8_t>(count);
}

/** Restores state and typed actions only before any callback or action transaction. */
bool restore_state(Vm& vm,
                   std::uint32_t phase,
                   std::uint64_t revision,
                   std::span<const ScriptVariable> variables,
                   std::span<const MissionTimer> timers,
                   std::uint64_t nextTimerSequence,
                   std::uint64_t nextIntentKey,
                   std::span<const Intent> pendingIntents) noexcept {
    Impl& impl = VmAccess::get(vm);
    if (!impl.active || impl.state == nullptr || impl.faulted || impl.frame != nullptr
        || impl.callbacks != 0 || !impl.outbox.empty() || variables.size() > impl.variables.size()
        || timers.size() > impl.timers.size()) {
        return false;
    }
    impl.phase = phase;
    impl.stateRevision = revision;
    std::copy(variables.begin(), variables.end(), impl.variables.begin());
    std::fill(impl.variables.begin() + static_cast<std::ptrdiff_t>(variables.size()),
              impl.variables.end(),
              ScriptVariable{});
    std::copy(timers.begin(), timers.end(), impl.timers.begin());
    std::fill(impl.timers.begin() + static_cast<std::ptrdiff_t>(timers.size()),
              impl.timers.end(),
              MissionTimer{});
    impl.variableCount = variables.size();
    impl.timerCount = timers.size();
    impl.nextTimerSequence = nextTimerSequence;
    impl.nextIntentKey = nextIntentKey;
    try {
        impl.outbox.assign(pendingIntents.begin(), pendingIntents.end());
    } catch (const std::bad_alloc&) {
        return false;
    }
    impl.outboxRead = 0;
    return true;
}

/** Restores state when no typed actions are pending. */
bool restore_state(Vm& vm, std::uint32_t phase, std::uint64_t revision) noexcept {
    return restore_state(vm,
                         phase,
                         revision,
                         {},
                         {},
                         state::activity::mission::kFirstTimerSequence,
                         state::activity::mission::kFirstIntentKey,
                         {});
}

/** Runs start and reserves one revision for the durable started transition. */
CallStatus start(Vm& vm, std::uint64_t now) noexcept {
    Impl& impl = VmAccess::get(vm);
    const std::uint64_t before = impl.stateRevision;
    const CallStatus status = invoke(impl, Handler::start, nullptr, nullptr, now);
    if (status != CallStatus::committed && status != CallStatus::noHandler) {
        return status;
    }
    if (impl.stateRevision != before) {
        return status;
    }
    if (impl.stateRevision == (std::numeric_limits<std::uint64_t>::max)()) {
        detail::set_error(impl, "mission start revision is exhausted");
        impl.faulted = true;
        return CallStatus::scriptError;
    }
    ++impl.stateRevision;
    return status;
}

/**
 * Runs load for a program whose durable state was restored.
 * It reserves no revision. A restore is not a state transition, so the revision is already correct.
 */
CallStatus load(Vm& vm, std::uint64_t now) noexcept {
    Impl& impl = VmAccess::get(vm);
    // A program that declares no on_load must cost a reattach nothing at all.
    if (impl.loadReference == LUA_NOREF && impl.active && impl.state != nullptr && !impl.faulted) {
        return CallStatus::noHandler;
    }
    return invoke(impl, Handler::load, nullptr, nullptr, now);
}

CallStatus dispatch(Vm& vm,
                    const host::Event& event,
                    const host::ClientMessageSnapshot* clientMessage,
                    std::uint64_t now) noexcept {
    return invoke(VmAccess::get(vm), Handler::event, &event, clientMessage, now);
}

bool handles_event(const Vm& vm, host::EventKind kind) noexcept {
    const Impl& impl = VmAccess::get(vm);
    return impl.active && !impl.faulted && detail::event_reference(impl, kind) != LUA_NOREF;
}

bool initial_state_region(const Vm& vm, std::int32_t& output) noexcept {
    const Impl& impl = VmAccess::get(vm);
    output = -1;
    if (!impl.active || impl.faulted || !impl.hasInitialState || impl.initialStateRegion < 0) {
        return false;
    }
    output = impl.initialStateRegion;
    return true;
}

/** Copies the next unread outbox action without consuming it; false when none is pending. */
bool pending_intent(const Vm& vm, Intent& output) noexcept {
    const Impl& impl = VmAccess::get(vm);
    output = {};
    if (!impl.active || impl.outboxRead >= impl.outbox.size()) {
        return false;
    }
    try {
        output = impl.outbox[impl.outboxRead];
    } catch (const std::bad_alloc&) {
        output = {};
        return false;
    }
    return true;
}

/** Copies the complete ordered outbox without consuming an action. */
bool snapshot_intents(const Vm& vm, std::vector<Intent>& output) noexcept {
    const Impl& impl = VmAccess::get(vm);
    output.clear();
    if (!impl.active) {
        return false;
    }
    try {
        output.assign(impl.outbox.begin() + static_cast<std::ptrdiff_t>(impl.outboxRead),
                      impl.outbox.end());
    } catch (const std::bad_alloc&) {
        output.clear();
        return false;
    }
    return true;
}

/** Copies complete durable script state without changing its revision. */
bool snapshot_durable_state(const Vm& vm,
                            std::span<ScriptVariable> variables,
                            std::size_t& variableCount,
                            std::span<MissionTimer> timers,
                            std::size_t& timerCount,
                            std::uint64_t& nextTimerSequence,
                            std::uint64_t& nextIntentKey) noexcept {
    const Impl& impl = VmAccess::get(vm);
    variableCount = 0;
    timerCount = 0;
    nextTimerSequence = state::activity::mission::kAbsentTimerSequence;
    nextIntentKey = state::activity::mission::kAbsentIntentKey;
    if (!impl.active || variables.size() < impl.variableCount || timers.size() < impl.timerCount) {
        return false;
    }
    std::copy_n(impl.variables.begin(), impl.variableCount, variables.begin());
    std::copy_n(impl.timers.begin(), impl.timerCount, timers.begin());
    variableCount = impl.variableCount;
    timerCount = impl.timerCount;
    nextTimerSequence = impl.nextTimerSequence;
    nextIntentKey = impl.nextIntentKey;
    return true;
}

/** Probes a mission script source for its declared initial_state region without a full VM attach. */
std::int32_t probe_initial_state_region(std::span<const char> source,
                                        std::string_view sdkLuaSearchPath) noexcept {
    if (source.empty()) {
        return -1;
    }
    lua_State* L = luaL_newstate();
    if (L == nullptr) {
        return -1;
    }
    luaL_requiref(L, "_G", luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_LOADLIBNAME, luaopen_package, 1);
    lua_pop(L, 1);
    if (!sdkLuaSearchPath.empty()) {
        lua_getglobal(L, "package");
        if (lua_istable(L, -1)) {
            lua_pushlstring(L, sdkLuaSearchPath.data(), sdkLuaSearchPath.size());
            lua_setfield(L, -2, "path");
        }
        lua_pop(L, 1);
    }
    if (luaL_loadbuffer(L, source.data(), source.size(), "mission_script_probe") != LUA_OK) {
        lua_close(L);
        return -1;
    }
    if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
        lua_close(L);
        return -1;
    }
    if (!lua_istable(L, -1)) {
        lua_close(L);
        return -1;
    }
    lua_getfield(L, -1, "initial_state");
    if (!lua_istable(L, -1)) {
        lua_close(L);
        return -1;
    }
    lua_getfield(L, -1, "region_index");
    std::int32_t region = -1;
    if (lua_isinteger(L, -1)) {
        region = static_cast<std::int32_t>(lua_tointeger(L, -1));
    }
    lua_close(L);
    return region;
}

/** Drops the oldest unread action, and empties the outbox once every action has been read. */
void consume_intent(Vm& vm) noexcept {
    Impl& impl = VmAccess::get(vm);
    if (impl.outboxRead >= impl.outbox.size()) {
        return;
    }
    impl.outbox[impl.outboxRead] = {};
    ++impl.outboxRead;
    if (impl.outboxRead == impl.outbox.size()) {
        impl.outbox.clear();
        impl.outboxRead = 0;
    }
}

void discard_intents(Vm& vm) noexcept {
    Impl& impl = VmAccess::get(vm);
    std::vector<Intent>{}.swap(impl.outbox);
    impl.outboxRead = 0;
}

void fault(Vm& vm, std::string_view reason) noexcept {
    Impl& impl = VmAccess::get(vm);
    detail::set_error(impl, reason);
    impl.faulted = true;
}

/** Copies the counters, durable sizes and arena figures a caller reads for diagnostics. */
void snapshot(const Vm& vm, Snapshot& output) noexcept {
    const Impl& impl = VmAccess::get(vm);
    output = {};
    output.lastError = impl.lastError;
    output.stateRevision = impl.stateRevision;
    output.callbacks = impl.callbacks;
    output.committedCallbacks = impl.committedCallbacks;
    output.refusedCallbacks = impl.refusedCallbacks;
    output.collections = impl.collections;
    output.phase = impl.phase;
    output.pendingIntents = impl.outbox.size() - impl.outboxRead;
    output.variableCount = impl.variableCount;
    output.timerCount = impl.timerCount;
    output.nextTimerSequence = impl.nextTimerSequence;
    output.nextIntentKey = impl.nextIntentKey;
    output.initialStateRegion = impl.initialStateRegion;
    output.arenaBytes = impl.arena.used;
    output.arenaHighWater = impl.arena.highWater;
    output.arenaBytesAfterClose = impl.arenaBytesAfterClose;
    output.active = impl.active;
    output.faulted = impl.faulted;
    output.hasInitialState = impl.hasInitialState;
}

void close(Vm& vm) noexcept {
    detail::destroy_state(VmAccess::get(vm));
}

/** Stable lowercase log token for one open status. */
const char* status_name(OpenStatus status) noexcept {
    switch (status) {
    case OpenStatus::ready:
        return "ready";
    case OpenStatus::sourceTooLarge:
        return "source_too_large";
    case OpenStatus::outOfMemory:
        return "out_of_memory";
    case OpenStatus::compileError:
        return "compile_error";
    case OpenStatus::runtimeError:
        return "runtime_error";
    case OpenStatus::invalidProgram:
        return "invalid_program";
    }
    return "unknown";
}

/** Stable lowercase log token for one call status. */
const char* status_name(CallStatus status) noexcept {
    switch (status) {
    case CallStatus::committed:
        return "committed";
    case CallStatus::noHandler:
        return "no_handler";
    case CallStatus::inactive:
        return "inactive";
    case CallStatus::scriptError:
        return "script_error";
    case CallStatus::instructionBudget:
        return "instruction_budget";
    case CallStatus::outOfMemory:
        return "out_of_memory";
    }
    return "unknown";
}

} // namespace sunrise::server::activity::mission::lua_vm
