#pragma once

#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "lauxlib.h"
#include "lua.h"
#include "lualib.h"
#include "mission_script_vm.h"

namespace sunrise::server::activity::mission::lua_vm::detail {

/** Named only by the shadow below; the type name is what the compiler prints at a refused call. */
struct MissionUserdataIsBuiltOnlyByPushHandle final {};

/**
 * Shadows the raw Lua userdata constructor across this namespace and every namespace nested in it.
 * It is an object, not a function, so unqualified lookup stops here and argument-dependent lookup
 * never reaches the real one. push_handle calls it as ::lua_newuserdatauv and stays the only maker.
 */
inline constexpr MissionUserdataIsBuiltOnlyByPushHandle lua_newuserdatauv{};

/** Block header. `previous` is the predecessor's payload size, so a free merges both ways. */
struct alignas(std::max_align_t) ArenaBlock final {
    std::size_t size{};
    std::size_t previous{};
    bool free{};
};

/** Lua's only allocator. Only an open program holds a block. */
struct Arena final {
    std::unique_ptr<std::byte[]> bytes{};
    std::size_t capacity{};
    std::size_t used{};
    std::size_t highWater{};
    /** Byte offset the next search starts from, so an allocation does not rescan the whole span. */
    std::size_t rover{};
    bool initialized{};
};

/** Candidate state and actions are invisible until a protected callback returns. */
struct Candidate final {
    std::vector<SlotDefinition> objectBatch{};
    std::vector<Intent> intents{};
    std::array<ScriptVariable, kVariableCapacity> variables{};
    std::array<MissionTimer, kTimerCapacity> timers{};
    std::size_t variableCount{};
    std::size_t timerCount{};
    std::uint64_t nextTimerSequence{state::activity::mission::kFirstTimerSequence};
    std::uint64_t nextIntentKey{state::activity::mission::kFirstIntentKey};
    std::uint32_t phase{};
    bool phaseChanged{};
};

/**
 * Reserves one request key on the pending candidate.
 * The key advances the candidate copy, so a discarded callback returns every key it took.
 * @return The reserved key, or the absent key when the per-program supply is exhausted.
 */
[[nodiscard]] inline std::uint64_t mint_intent_key(Candidate& candidate) noexcept {
    const std::uint64_t key = candidate.nextIntentKey;
    if (key == (std::numeric_limits<std::uint64_t>::max)()) {
        return state::activity::mission::kAbsentIntentKey;
    }
    candidate.nextIntentKey = key + 1;
    return key;
}

enum class Handler : std::uint8_t {
    start,
    event,
    /** Runs once per attach that restored an already-started program, in place of start. */
    load,
};

/** Native values available only while one callback is inside lua_pcall. */
struct CallFrame final {
    Candidate candidate{};
    const host::Event* event{};
    const host::ClientMessageSnapshot* clientMessage{};
    std::uint64_t now{};
    std::uint32_t remainingInstructions{kInstructionBudget};
    Handler handler{Handler::event};
    bool instructionExceeded{};
    bool memoryExceeded{};
    bool intentAllocationFailed{};
    bool moduleOwnershipFailed{};
};

/** @return One LUA_NOREF per event kind; a zero would read as a captured callback. */
[[nodiscard]] constexpr std::array<int, host::kEventKindCount> make_unset_references() noexcept {
    std::array<int, host::kEventKindCount> references{};
    references.fill(LUA_NOREF);
    return references;
}

/** Complete per-activity VM state; no allocation is shared with another instance. */
struct Impl final {
    state::activity::mission::AttemptState attempt{};
    std::vector<state::activity::mission::DeviceRequestReport> deviceRequests{};
    /** Links whose device requests read as applied: the route and every joined private link. */
    std::vector<std::uint64_t> deviceRequestSources{};
    std::vector<state::activity::mission::SquadPopulation> squadPopulations{};
    /** Ghost-link levels the runtime last published, readable outside their own callback. */
    std::array<GhostLinkRow, kGhostLinkCapacity> ghostLevels{};
    std::uint8_t ghostLevelCount{};
    Arena arena{};
    ProgramIdentity identity{};
    DefinitionApi definitions{};
    /** The peer set the host last published. Read by context.peers, never by an event. */
    std::array<PeerSession, kPeerCapacity> peers{};
    std::uint8_t peerCount{};
    std::array<std::array<char, state::mods::kIdCapacity>, state::mods::kModCapacity>
        moduleOwners{};
    std::size_t moduleOwnerCount{};
    std::vector<Intent> outbox{};
    std::array<ScriptVariable, kVariableCapacity> variables{};
    std::array<MissionTimer, kTimerCapacity> timers{};
    std::size_t outboxRead{};
    std::size_t variableCount{};
    std::size_t timerCount{};
    std::uint64_t nextTimerSequence{state::activity::mission::kFirstTimerSequence};
    std::uint64_t nextIntentKey{state::activity::mission::kFirstIntentKey};
    std::uint64_t stateRevision{};
    std::uint64_t callbacks{};
    std::uint64_t committedCallbacks{};
    std::uint64_t refusedCallbacks{};
    std::uint64_t collections{};
    std::uint32_t phase{};
    /** Effective authored region declared by program.initial_state, when present. */
    std::int32_t initialStateRegion{-1};
    /** Spawn set program.initial_state.spawn_set_hash names; zero when the program names none. */
    std::uint32_t initialStateSpawnSet{};
    /** Objects program.initial_state.omit keeps out of every seed the lease materializes. */
    std::array<state::activity::mission::MissionSeedOmission,
               state::activity::mission::kMissionSeedOmitCapacity>
        initialStateOmissions{};
    std::uint8_t initialStateOmissionCount{};
    std::size_t arenaBytesAfterClose{};
    std::array<char, 256> lastError{};
    lua_State* state{};
    CallFrame* frame{};
    int startReference{LUA_NOREF};
    std::array<int, host::kEventKindCount> eventReferences{make_unset_references()};
    int loadReference{LUA_NOREF};
    bool hasInitialState{};
    bool active{};
    bool faulted{};
};

struct VmAccess final {
    [[nodiscard]] static Impl& get(Vm& vm) noexcept {
        return *std::launder(reinterpret_cast<Impl*>(vm.storage_.data()));
    }
    [[nodiscard]] static const Impl& get(const Vm& vm) noexcept {
        return *std::launder(reinterpret_cast<const Impl*>(vm.storage_.data()));
    }
};

/** @return True once the arena owns a block laid out as one free span. */
[[nodiscard]] bool arena_initialize(Arena& arena) noexcept;
/** Frees the block. Every Lua object in it is already gone by then. */
void arena_release(Arena& arena) noexcept;
void* arena_allocate(void* context,
                     void* pointer,
                     std::size_t oldSize,
                     std::size_t newSize) noexcept;

/** @return The VM instance carried in the Lua state's extra space, or null before attach. */
[[nodiscard]] inline Impl* impl_from_state(lua_State* state) noexcept {
    Impl* impl = nullptr;
    static_assert(LUA_EXTRASPACE >= sizeof(Impl*));
    std::memcpy(&impl, lua_getextraspace(state), sizeof(Impl*));
    return impl;
}

/** Stores the VM instance in the Lua state's extra space. */
inline void attach_impl(lua_State* state, Impl& impl) noexcept {
    Impl* const pointer = &impl;
    std::memcpy(lua_getextraspace(state), &pointer, sizeof(Impl*));
}

/** @return The captured non-event entry, or LUA_NOREF when the program declares none. */
[[nodiscard]] inline int handler_reference(const Impl& impl, Handler handler) noexcept {
    switch (handler) {
    case Handler::start:
        return impl.startReference;
    case Handler::load:
        return impl.loadReference;
    case Handler::event:
        return LUA_NOREF;
    }
    return LUA_NOREF;
}

/** @return The callback for one native event kind, or LUA_NOREF when it is absent. */
[[nodiscard]] inline int event_reference(const Impl& impl, host::EventKind kind) noexcept {
    const std::size_t index = static_cast<std::size_t>(kind);
    return index < impl.eventReferences.size() ? impl.eventReferences[index] : LUA_NOREF;
}

/** Lua errors must leave the protected call. */
[[noreturn]] inline void raise_lua_error(lua_State* state, const char* message) {
    luaL_error(state, message);
    __assume(0);
}

/** Raises a Lua error when no mission callback is running. */
[[nodiscard]] inline CallFrame& active_frame(lua_State* state) {
    Impl* const impl = impl_from_state(state);
    if (impl == nullptr || impl->frame == nullptr) {
        raise_lua_error(state, "mission callback context is inactive");
    }
    return *impl->frame;
}

/** Borrows one checked Lua string argument without copying it. */
[[nodiscard]] inline std::string_view lua_string_view(lua_State* state, int index) {
    std::size_t length = 0;
    const char* const text = luaL_checklstring(state, index, &length);
    return {text, length};
}

/** Pushes one 64-bit identity as a decimal string; Lua integers cannot carry the full range. */
inline void push_u64_string(lua_State* state, std::uint64_t value) {
    std::array<char, 32> text{};
    const auto converted = std::to_chars(text.data(), text.data() + text.size(), value);
    if (converted.ec != std::errc{}) {
        luaL_error(state, "identity conversion failed");
    }
    lua_pushlstring(state, text.data(), static_cast<std::size_t>(converted.ptr - text.data()));
}

/** Pushes one 32-byte digest as 64 lowercase hex characters. */
inline void push_digest(lua_State* state, const std::array<std::byte, 32>& value) {
    // Lowercase hex digits; every digest and byte field is spelled this way.
    constexpr char digits[] = "0123456789abcdef";
    std::array<char, 64> text{};
    for (std::size_t index = 0; index < value.size(); ++index) {
        const auto byte = static_cast<std::uint8_t>(value[index]);
        text[index * 2U] = digits[byte >> 4U];
        text[index * 2U + 1U] = digits[byte & 0xFU];
    }
    lua_pushlstring(state, text.data(), text.size());
}

/** Pushes the text, or nil when it is empty. */
inline void push_optional_name(lua_State* state, std::string_view value) {
    if (value.empty()) {
        lua_pushnil(state);
    } else {
        lua_pushlstring(state, value.data(), value.size());
    }
}

inline void set_integer(lua_State* state, const char* key, lua_Integer value) {
    lua_pushinteger(state, value);
    lua_setfield(state, -2, key);
}

inline void set_boolean(lua_State* state, const char* key, bool value) {
    lua_pushboolean(state, value ? 1 : 0);
    lua_setfield(state, -2, key);
}

inline void set_string(lua_State* state, const char* key, std::string_view value) {
    lua_pushlstring(state, value.data(), value.size());
    lua_setfield(state, -2, key);
}

/** Installs the allowed libraries, locked handle types, and no other globals. */
[[nodiscard]] bool initialize_sandbox(Impl& impl) noexcept;
/** Pushes and executes the selected script callback inside the current protected frame. */
int protected_callback(lua_State* state);
/** Records a bounded single-line diagnostic. */
void set_error(Impl& impl, std::string_view error) noexcept;
/** Removes every Lua-owned value while retaining reusable fixed storage. */
void destroy_state(Impl& impl) noexcept;

} // namespace sunrise::server::activity::mission::lua_vm::detail
