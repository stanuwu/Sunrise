#include <array>
#include <charconv>
#include <limits>

#include "mission_script_lua_internal.h"

namespace sunrise::server::activity::mission::lua_vm::detail {
namespace {

constexpr const char* kSequencesMetatable = "SunriseActorSequences";
constexpr const char* kSequenceMetatable = "SunriseActorSequence";

struct SequenceHandle final {
    ActorSequenceOwner owner{};
    std::uint32_t ordinal{};
};

/** An actor value cannot cross a slot or SDK/client generation boundary. */
[[nodiscard]] bool current_owner(lua_State* state, const ActorSequenceOwner& owner) {
    const Impl* const impl = impl_from_state(state);
    ActorSequenceOwner current{};
    return impl != nullptr && impl->definitions.actorSequences.owner != nullptr
           && impl->definitions.actorSequences.owner(
               impl->definitions.actorSequences.context, owner.slotRow, current)
           && current == owner;
}

/** Rechecks ownership before resolving the same authored ordinal. */
[[nodiscard]] bool
current_sequence(lua_State* state, const SequenceHandle& handle, ActorSequenceDefinition& output) {
    const Impl* const impl = impl_from_state(state);
    return current_owner(state, handle.owner) && impl->definitions.actorSequences.resolve != nullptr
           && impl->definitions.actorSequences.resolve(
               impl->definitions.actorSequences.context, handle.owner, handle.ordinal, output);
}

/** Exposes all extracted kinds without making unsupported entries executable. */
[[nodiscard]] int sequence_index(lua_State* state) {
    const auto* const handle =
        static_cast<const SequenceHandle*>(luaL_checkudata(state, 1, kSequenceMetatable));
    ActorSequenceDefinition row{};
    if (!current_sequence(state, *handle, row)) {
        return luaL_error(state, "actor sequence owner is stale");
    }
    const std::string_view key = luaL_checkstring(state, 2);
    std::string_view text{};
    if (key == "id") {
        text = row.id;
    } else if (key == "name") {
        text = row.name;
    } else if (key == "symbol") {
        text = row.symbol;
    } else if (key == "source_path") {
        text = row.sourcePath;
    } else if (key == "key") {
        lua_pushinteger(state, row.keyHash);
        return 1;
    } else if (key == "kind") {
        lua_pushinteger(state, row.kind);
        return 1;
    } else if (key == "resource_tag") {
        lua_pushinteger(state, row.resourceTag);
        return 1;
    } else if (key == "table_index") {
        lua_pushinteger(state, row.tableIndex);
        return 1;
    } else if (key == "ordinal") {
        lua_pushinteger(state, row.ordinal);
        return 1;
    } else if (key == "source_offset") {
        lua_pushinteger(state, row.sourceOffset);
        return 1;
    } else if (key == "playable") {
        lua_pushboolean(state, row.playable);
        return 1;
    } else {
        lua_pushnil(state);
        return 1;
    }
    if (text.empty()) {
        lua_pushnil(state);
    } else {
        lua_pushlstring(state, text.data(), text.size());
    }
    return 1;
}

/** Collection rows are one-based; an absent ordinal returns nil. */
[[nodiscard]] int sequences_at(lua_State* state) {
    const auto* const owner =
        static_cast<const ActorSequenceOwner*>(luaL_checkudata(state, 1, kSequencesMetatable));
    if (!current_owner(state, *owner)) {
        return luaL_error(state, "actor sequence owner is stale");
    }
    const lua_Integer ordinal = luaL_checkinteger(state, 2);
    ActorSequenceDefinition row{};
    const SequenceHandle handle{*owner, static_cast<std::uint32_t>(ordinal)};
    if (ordinal <= 0 || ordinal > (std::numeric_limits<std::uint32_t>::max)()
        || !current_sequence(state, handle, row)) {
        lua_pushnil(state);
    } else {
        push_handle(state, kSequenceMetatable, handle);
    }
    return 1;
}

/** Named members are the extractor's collision-safe symbols. */
[[nodiscard]] int sequences_index(lua_State* state) {
    const auto* const owner =
        static_cast<const ActorSequenceOwner*>(luaL_checkudata(state, 1, kSequencesMetatable));
    if (!current_owner(state, *owner)) {
        return luaL_error(state, "actor sequence owner is stale");
    }
    const auto& api = impl_from_state(state)->definitions.actorSequences;
    const std::size_t count = api.count != nullptr ? api.count(api.context, *owner) : 0;
    const std::string_view key = luaL_checkstring(state, 2);
    if (key == "count") {
        lua_pushinteger(state, static_cast<lua_Integer>(count));
    } else if (key == "at") {
        lua_pushcfunction(state, &sequences_at);
    } else {
        std::uint32_t keyHash = 0;
        bool alias = false;
        if (key.size() == 12 && key.starts_with("KEY_")) {
            const auto parsed =
                std::from_chars(key.data() + 4, key.data() + key.size(), keyHash, 16);
            alias = parsed.ec == std::errc{} && parsed.ptr == key.data() + key.size();
        }
        for (std::size_t ordinal = 1; ordinal <= count; ++ordinal) {
            ActorSequenceDefinition row{};
            if (!api.resolve(api.context, *owner, static_cast<std::uint32_t>(ordinal), row)) {
                continue;
            }
            if (alias ? row.keyHash == keyHash : row.symbol == key) {
                push_handle(state,
                            kSequenceMetatable,
                            SequenceHandle{*owner, static_cast<std::uint32_t>(ordinal)});
                return 1;
            }
        }
        lua_pushnil(state);
    }
    return 1;
}
} // namespace

/** Binds the catalog to the selected combatant before any sequence handle is minted. */
int slot_actor_sequences(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition slot{};
    ActorSequenceOwner owner{};
    const Impl* const impl = impl_from_state(state);
    if (!current_slot(state, *handle, slot) || !exact_combatant_slot(slot)
        || impl->definitions.actorSequences.owner == nullptr
        || impl->definitions.actorSequences.count == nullptr
        || impl->definitions.actorSequences.resolve == nullptr
        || !impl->definitions.actorSequences.owner(
            impl->definitions.actorSequences.context, slot.nativeRow, owner)) {
        return luaL_error(state, "combatant has no exact authored actor owner");
    }
    push_handle(state, kSequencesMetatable, owner);
    return 1;
}

/** Only an extracted value owned by this exact combatant can become a playback intent. */
int slot_play_actor_sequence(lua_State* state) {
    const auto* const handle =
        static_cast<const SlotHandle*>(luaL_checkudata(state, 1, kSlotMetatable));
    SlotDefinition slot{};
    if (!current_slot(state, *handle, slot) || !exact_combatant_slot(slot)) {
        return luaL_error(state, "activity slot is not an exact combatant");
    }
    static constexpr std::array<std::string_view, 1> kArguments{"sequence"};
    refuse_unknown_arguments(state, kArguments);
    static_cast<void>(push_argument(state, "sequence"));
    const SequenceHandle sequence =
        *static_cast<const SequenceHandle*>(luaL_checkudata(state, -1, kSequenceMetatable));
    lua_pop(state, 1);
    ActorSequenceDefinition row{};
    if (sequence.owner.slotRow != slot.nativeRow || !current_sequence(state, sequence, row)) {
        return luaL_error(state, "sequence belongs to another combatant or generation");
    }
    if (!row.playable) {
        return luaL_error(state, "extracted sequence kind is not playable");
    }
    Intent intent{};
    intent.kind = IntentKind::playActorSequence;
    intent.firstRow = slot.nativeRow;
    intent.secondRow = row.catalogRow;
    intent.sequenceOwner = sequence.owner;
    return queue_intent(state, active_frame(state), intent);
}

void register_actor_sequence_metatables(lua_State* state) {
    register_metatable(state, kSequencesMetatable, &sequences_index);
    register_metatable(state, kSequenceMetatable, &sequence_index);
}

} // namespace sunrise::server::activity::mission::lua_vm::detail
