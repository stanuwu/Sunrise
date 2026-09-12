#include "mission_script_actor_sequence_bridge.h"

#include "../../../state/activity_sdk/actor_sequences.h"
#include "../activity_sdk_actor_sequences.h"

namespace sunrise::server::activity::mission::sdk_bridge {
namespace {
namespace sdk = state::activity_sdk;
using World = sdk::generated_world::GeneratedWorldView;

[[nodiscard]] bool
owner(const void* context, std::uint32_t slotRow, lua_vm::ActorSequenceOwner& output) noexcept {
    return context != nullptr
           && actor_sequences::owner(*static_cast<const World*>(context), slotRow, output);
}

[[nodiscard]] bool current(const void* context,
                           const lua_vm::ActorSequenceOwner& expected) noexcept {
    lua_vm::ActorSequenceOwner actual{};
    return owner(context, expected.slotRow, actual) && actual == expected;
}

[[nodiscard]] std::size_t count(const void* context,
                                const lua_vm::ActorSequenceOwner& expected) noexcept {
    return current(context, expected)
               ? sdk::actor_sequence_count(
                     *static_cast<const World*>(context)->activity_sdk_view().catalog,
                     expected.actorClassRow)
               : 0;
}

/** Metadata keeps the exact table row while playback uses a validated actor-owned handle. */
[[nodiscard]] bool resolve(const void* context,
                           const lua_vm::ActorSequenceOwner& expected,
                           std::uint32_t ordinal,
                           lua_vm::ActorSequenceDefinition& output) noexcept {
    output = {};
    if (!current(context, expected) || ordinal == 0) {
        return false;
    }
    const auto& catalog = *static_cast<const World*>(context)->activity_sdk_view().catalog;
    const auto* const row = sdk::actor_sequence_at(catalog, expected.actorClassRow, ordinal - 1U);
    if (row == nullptr) {
        return false;
    }
    output.id = catalog.string(row->id);
    output.name = catalog.string(row->name);
    output.symbol = catalog.string(row->symbol);
    output.sourcePath = catalog.string(row->sourcePath);
    output.catalogRow = static_cast<std::uint32_t>(row - catalog.actor_sequence_entries().data());
    output.keyHash = row->keyHash;
    output.kind = row->kind;
    output.resourceTag = row->resourceTag;
    output.tableIndex = row->tableIndex;
    output.ordinal = row->ordinal;
    output.sourceOffset = row->sourceOffset;
    output.playable = sdk::actor_sequence_playable(*row);
    return true;
}
} // namespace

void attach_actor_sequences(lua_vm::DefinitionApi& output, const World& world) noexcept {
    output.actorSequences = {&world, &owner, &count, &resolve};
}

} // namespace sunrise::server::activity::mission::sdk_bridge
