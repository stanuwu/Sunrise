#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "../build_data/scriptables/inline_name_evidence.h"
#include "runtime.h"

namespace sunrise::state::activity_sdk {
namespace sequence_detail {

/** Duplicate declarations may share one table pair; distinct owner choices remain ambiguous. */
[[nodiscard]] inline const format::ActorSequenceBinding*
binding(const Catalog& catalog, std::uint32_t actorClass) noexcept {
    if (actorClass >= catalog.actor_classes().size()) {
        return nullptr;
    }
    const auto rows = catalog.actor_sequence_bindings();
    const auto first =
        std::lower_bound(rows.begin(), rows.end(), actorClass, [](const auto& row, auto actor) {
            return row.actorClassIndex < actor;
        });
    const format::ActorSequenceBinding* found = nullptr;
    for (auto row = first; row != rows.end() && row->actorClassIndex == actorClass; ++row) {
        if (found != nullptr
            && (found->globalTableIndex != row->globalTableIndex
                || found->localTableIndex != row->localTableIndex)) {
            return nullptr;
        }
        found = &*row;
    }
    return found;
}

/** A missing table is an empty scope; malformed ranges are never exposed. */
[[nodiscard]] inline std::span<const format::ActorSequenceEntry>
entries(const Catalog& catalog, std::uint32_t tableIndex) noexcept {
    const auto tables = catalog.actor_sequence_tables();
    const auto rows = catalog.actor_sequence_entries();
    if (tableIndex >= tables.size()) {
        return {};
    }
    const auto range = tables[tableIndex].entries;
    return range.first <= rows.size() && range.count <= rows.size() - range.first
               ? rows.subspan(range.first, range.count)
               : std::span<const format::ActorSequenceEntry>{};
}

struct Match final {
    const format::ActorSequenceEntry* row{};
    bool present{};
};

/** Duplicate keys shadow the local scope but cannot authorize a typed playback request. */
[[nodiscard]] inline Match
lookup(const Catalog& catalog, std::uint32_t table, std::uint32_t key) noexcept {
    const auto rows = entries(catalog, table);
    const auto first =
        std::lower_bound(rows.begin(), rows.end(), key, [](const auto& row, auto value) {
            return row.keyHash < value;
        });
    if (first == rows.end() || first->keyHash != key) {
        return {};
    }
    const auto next = first + 1;
    return {next != rows.end() && next->keyHash == key ? nullptr : &*first, true};
}

} // namespace sequence_detail

/** Native sequence actions use kind two; other extracted kinds stay inspectable. */
[[nodiscard]] inline bool actor_sequence_playable(const format::ActorSequenceEntry& row) noexcept {
    return (row.flags & format::kActorSequenceExact) != 0 && row.kind == 2 && row.keyHash != 0
           && row.keyHash != 0x811C9DC5U && row.keyHash != format::kAbsentIndex
           && row.resourceTag != 0 && row.resourceTag != format::kAbsentIndex;
}

/** Global keys win before the actor-local fallback is considered. */
[[nodiscard]] inline const format::ActorSequenceEntry* actor_sequence_by_key(
    const Catalog& catalog, std::uint32_t actorClass, std::uint32_t key) noexcept {
    const auto* owner = sequence_detail::binding(catalog, actorClass);
    if (owner == nullptr) {
        return nullptr;
    }
    const auto global = sequence_detail::lookup(catalog, owner->globalTableIndex, key);
    return global.present ? global.row
                          : sequence_detail::lookup(catalog, owner->localTableIndex, key).row;
}

/** An entry row is usable only when this actor's native lookup selects that exact row. */
[[nodiscard]] inline const format::ActorSequenceEntry* resolve_actor_sequence(
    const Catalog& catalog, std::uint32_t actorClass, std::uint32_t entryRow) noexcept {
    const auto entries = catalog.actor_sequence_entries();
    if (entryRow >= entries.size()) {
        return nullptr;
    }
    const auto* row = &entries[entryRow];
    return actor_sequence_by_key(catalog, actorClass, row->keyHash) == row ? row : nullptr;
}

/** Effective order is global key order, then unshadowed local key order. */
[[nodiscard]] inline const format::ActorSequenceEntry*
actor_sequence_at(const Catalog& catalog, std::uint32_t actorClass, std::size_t ordinal) noexcept {
    const auto* owner = sequence_detail::binding(catalog, actorClass);
    if (owner == nullptr) {
        return nullptr;
    }
    for (const bool global : {true, false}) {
        if (!global && owner->globalTableIndex == owner->localTableIndex) {
            continue;
        }
        const auto table = global ? owner->globalTableIndex : owner->localTableIndex;
        for (const auto& row : sequence_detail::entries(catalog, table)) {
            if (actor_sequence_by_key(catalog, actorClass, row.keyHash) != &row) {
                continue;
            }
            if (ordinal == 0) {
                return &row;
            }
            --ordinal;
        }
    }
    return nullptr;
}

/** Counts the same effective rows that actor_sequence_at exposes. */
[[nodiscard]] inline std::size_t actor_sequence_count(const Catalog& catalog,
                                                      std::uint32_t actorClass) noexcept {
    const auto* owner = sequence_detail::binding(catalog, actorClass);
    if (owner == nullptr) {
        return 0;
    }
    std::size_t count = 0;
    for (const bool global : {true, false}) {
        if (!global && owner->globalTableIndex == owner->localTableIndex) {
            continue;
        }
        const auto table = global ? owner->globalTableIndex : owner->localTableIndex;
        for (const auto& row : sequence_detail::entries(catalog, table)) {
            if (actor_sequence_by_key(catalog, actorClass, row.keyHash) == &row) {
                ++count;
            }
        }
    }
    return count;
}

/** Verifies the immutable sequence corpus and every exact actor-source binding. */
[[nodiscard]] inline bool valid_actor_sequences(const Catalog& catalog) noexcept {
    const auto tables = catalog.actor_sequence_tables();
    const auto entries = catalog.actor_sequence_entries();
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < tables.size(); ++index) {
        const auto& table = tables[index];
        const bool global = table.definitionClass == format::kActorSequenceGlobalDefinitionClass;
        if (table.flags != format::kActorSequenceExact || table.definitionTag == 0
            || table.definitionTag == format::kAbsentIndex
            || (!global && table.definitionClass != format::kActorSequenceLocalDefinitionClass)
            || table.arrayOffset
                   != (global ? format::kActorSequenceGlobalArrayOffset
                              : format::kActorSequenceLocalArrayOffset)
            || table.entries.first != cursor || table.entries.count > entries.size() - cursor) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (tables[prior].definitionTag == table.definitionTag) {
                return false;
            }
        }
        for (std::uint32_t ordinal = 0; ordinal < table.entries.count; ++ordinal) {
            const auto& row = entries[cursor + ordinal];
            const auto name = catalog.string(row.name), symbol = catalog.string(row.symbol);
            const auto path = catalog.string(row.sourcePath), id = catalog.string(row.id);
            for (const auto text : {row.id, row.name, row.symbol, row.sourcePath}) {
                if (text.offset == format::kAbsentIndex && text.length == 0) {
                    continue;
                }
                if (text.offset > catalog.string_bytes().size()
                    || text.length > catalog.string_bytes().size() - text.offset) {
                    return false;
                }
            }
            if (row.tableIndex != index || row.ordinal != ordinal || row.kind > 255
                || row.sourceOffset < table.arrayOffset + 16U || row.flags == 0
                || (row.flags & ~format::kActorSequenceEntryFlagMask) != 0
                || (row.flags & format::kActorSequenceExact) == 0 || row.reserved != 0 || id.empty()
                || symbol.empty() || id.find('\0') != std::string_view::npos
                || path.find('\0') != std::string_view::npos
                || name.find('\0') != std::string_view::npos
                || (ordinal != 0
                    && (entries[cursor + ordinal - 1].keyHash > row.keyHash
                        || entries[cursor + ordinal - 1].sourceOffset > format::kAbsentIndex - 24U
                        || entries[cursor + ordinal - 1].sourceOffset + 24U != row.sourceOffset))) {
                return false;
            }
            if (((row.flags & format::kActorSequenceNameVerified) != 0) != !name.empty()) {
                return false;
            }
            if (!name.empty()
                && build_data::scriptables::inline_name_evidence::hash(
                       std::as_bytes(std::span(name)))
                       != row.keyHash) {
                return false;
            }
            if ((row.flags & format::kActorSequenceSymbolFromPath) != 0 && path.empty()) {
                return false;
            }
            for (const char c : symbol) {
                if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '_') {
                    return false;
                }
            }
        }
        cursor += table.entries.count;
    }
    if (cursor != entries.size()) {
        return false;
    }
    const auto bindings = catalog.actor_sequence_bindings();
    for (std::size_t index = 0; index < bindings.size(); ++index) {
        const auto& row = bindings[index];
        if (row.actorClassIndex >= catalog.actor_classes().size()
            || row.flags != format::kActorSequenceExact || row.configTag == 0
            || row.configTag == format::kAbsentIndex
            || row.sourceClass != format::kActorSequenceOwnerSourceClass
            || (index != 0
                && (bindings[index - 1].actorClassIndex > row.actorClassIndex
                    || (bindings[index - 1].actorClassIndex == row.actorClassIndex
                        && bindings[index - 1].componentOrdinal >= row.componentOrdinal)))) {
            return false;
        }
        for (const bool global : {true, false}) {
            const auto table = global ? row.globalTableIndex : row.localTableIndex;
            if (table != format::kAbsentIndex
                && (table >= tables.size()
                    || tables[table].definitionClass
                           != (global ? format::kActorSequenceGlobalDefinitionClass
                                      : format::kActorSequenceLocalDefinitionClass))) {
                return false;
            }
        }
    }
    return true;
}

} // namespace sunrise::state::activity_sdk
