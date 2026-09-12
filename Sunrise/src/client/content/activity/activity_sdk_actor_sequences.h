#pragma once

#include <string>
#include <unordered_map>

#include "../../../state/build_data/scriptables/inline_name_evidence.h"
#include "activity_sdk_actor_rsat_inventory_internal.h"

namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory {
namespace sequence_inventory {

/** Declared component references and action entries retain their native package classes. */
inline constexpr std::uint32_t kComponentClass = 0x80809C04U;
inline constexpr std::uint32_t kConfigClass = 0x80809C36U;
inline constexpr std::uint32_t kActorGroupClass = 0x80807EE1U;
inline constexpr std::uint32_t kActionEntryClass = 0x808081AEU;
inline constexpr std::size_t kComponentArrayOffset = 16, kComponentStride = 12;
inline constexpr std::size_t kNativeRootRelativeOffset = 16;
inline constexpr std::size_t kLocalTableOffset = 760, kGlobalTableOffset = 768;
inline constexpr std::size_t kActionStride = 24, kPathOffset = 8, kResourceOffset = 16;
/** Diagnostic paths are bounded while searching for their terminator. */
inline constexpr std::size_t kMaximumPathBytes = 4096;

/** Hashes exact candidate bytes using the same FNV-1 rule as native authored keys. */
inline std::uint32_t name_hash(std::string_view value) noexcept {
    return state::build_data::scriptables::inline_name_evidence::hash(
        std::as_bytes(std::span(value)));
}

/** Retains the source path without treating its basename as the authored lookup key. */
inline bool source_path(std::span<const std::byte> blob, std::size_t field, std::string& output) {
    std::int64_t relative = 0;
    if (!read_value(blob, field, relative)) {
        return false;
    }
    output.clear();
    if (relative == 0) {
        return true;
    }
    std::size_t start = 0;
    if (!relative_offset(field, relative, start) || start >= blob.size()) {
        return false;
    }
    std::size_t length = 0;
    while (start + length < blob.size() && length <= kMaximumPathBytes
           && blob[start + length] != std::byte{0}) {
        ++length;
    }
    if (length > kMaximumPathBytes || start + length == blob.size()) {
        return false;
    }
    output.assign(reinterpret_cast<const char*>(blob.data() + start), length);
    return output.empty()
           || state::build_data::scriptables::inline_name_evidence::valid_utf8(
               std::as_bytes(std::span(output)));
}

/** Only the filename stem supplies a generated presentation symbol. */
inline std::string_view basename(std::string_view path) noexcept {
    const auto slash = path.find_last_of("/\\");
    if (slash != std::string_view::npos) {
        path.remove_prefix(slash + 1);
    }
    const auto dot = path.find('.');
    return path.substr(0, dot);
}

/** Symbols are identifiers; their bytes are never substituted for the extracted native key. */
inline std::string symbol(std::string_view value) {
    std::string result;
    for (const unsigned char c : value) {
        if (c >= 'a' && c <= 'z') {
            result.push_back(static_cast<char>(c - 'a' + 'A'));
        } else if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
            result.push_back(static_cast<char>(c));
        } else if (result.empty() || result.back() != '_') {
            result.push_back('_');
        }
    }
    while (!result.empty() && result.back() == '_') {
        result.pop_back();
    }
    if (!result.empty() && result.front() >= '0' && result.front() <= '9') {
        result.insert(0, "VALUE_");
    }
    return result;
}

/** One physical definition is read once even when many actors share it. */
inline bool table(BuildState& state, SequenceTableSource source, std::uint32_t& output) {
    output = format::kAbsentIndex;
    if (is_absent_tag(source.definitionTag)) {
        return true;
    }
    const auto found = state.sequenceTableIndexes.find(source.definitionTag);
    if (found != state.sequenceTableIndexes.end()) {
        output = found->second;
        return state.snapshot.sequenceTables[output].definitionClass == source.definitionClass;
    }
    const bool global = source.definitionClass == format::kActorSequenceGlobalDefinitionClass;
    if (!global && source.definitionClass != format::kActorSequenceLocalDefinitionClass) {
        return false;
    }
    std::vector<std::byte> blob;
    TypedArray array{};
    const auto arrayOffset =
        global ? format::kActorSequenceGlobalArrayOffset : format::kActorSequenceLocalArrayOffset;
    if (!state.readTag(state.readContext, source.definitionTag, source.definitionClass, blob)
        || !typed_array(blob, arrayOffset, kActionEntryClass, kActionStride, array)
        || state.snapshot.sequenceEntries.size() > (std::numeric_limits<std::uint32_t>::max)()
        || state.snapshot.sequenceTables.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    output = static_cast<std::uint32_t>(state.snapshot.sequenceTables.size());
    format::ActorSequenceTable row{
        source.definitionTag,
        source.definitionClass,
        arrayOffset,
        {static_cast<std::uint32_t>(state.snapshot.sequenceEntries.size()), array.count},
        format::kActorSequenceExact};
    std::uint32_t previousKey = 0;
    for (std::uint32_t ordinal = 0; ordinal < array.count; ++ordinal) {
        if (is_cancelled(state.cancel, state.cancelContext)) {
            return false;
        }
        const auto offset = static_cast<std::size_t>(array.dataOffset) + ordinal * kActionStride;
        SequenceEntry entry{};
        std::uint8_t kind = 0;
        if (!read_value(blob, offset, entry.row.keyHash) || !read_value(blob, offset + 4, kind)
            || !read_value(blob, offset + kResourceOffset, entry.row.resourceTag)
            || !source_path(blob, offset + kPathOffset, entry.sourcePath)
            || !to_u32(offset, entry.row.sourceOffset)
            || (ordinal != 0 && entry.row.keyHash < previousKey)) {
            return false;
        }
        previousKey = entry.row.keyHash;
        entry.row.tableIndex = output;
        entry.row.ordinal = ordinal;
        entry.row.kind = kind;
        entry.row.flags = format::kActorSequenceExact;
        std::array<char, 64> id{};
        const auto length = std::snprintf(id.data(),
                                          id.size(),
                                          "actor-sequence/%08x/%u/%u",
                                          source.definitionTag,
                                          arrayOffset,
                                          ordinal);
        if (length <= 0 || static_cast<std::size_t>(length) >= id.size()) {
            return false;
        }
        entry.id.assign(id.data(), static_cast<std::size_t>(length));
        state.snapshot.sequenceEntries.push_back(std::move(entry));
    }
    state.snapshot.sequenceTables.push_back(row);
    state.sequenceTableIndexes.emplace(source.definitionTag, output);
    return true;
}

/** The native ActorGroup source names its local definition, program and global definition. */
inline bool owner(BuildState& state, std::uint32_t configTag, SequenceOwner& output) {
    const auto cached = state.sequenceOwners.find(configTag);
    if (cached != state.sequenceOwners.end()) {
        output = cached->second;
        return true;
    }
    output = {};
    std::vector<std::byte> config;
    if (state.readTag(state.readContext, configTag, kConfigClass, config)) {
        std::int64_t relative = 0;
        std::size_t native = 0;
        std::uint32_t nativeClass = 0;
        if (!read_value(config, kNativeRootRelativeOffset, relative)
            || !relative_offset(kNativeRootRelativeOffset, relative, native) || native < 4
            || !read_value(config, native - 4, nativeClass)) {
            return false;
        }
        if (nativeClass == kActorGroupClass) {
            std::uint32_t ownTag = 0, sourceClass = 0;
            std::uint64_t source = 0;
            if (!read_value(config, native, ownTag) || ownTag != configTag
                || !read_value(config, native + 4, sourceClass)
                || sourceClass != format::kActorSequenceOwnerSourceClass
                || !read_value(config, native + 8, source)
                || source > (std::numeric_limits<std::uint32_t>::max)()
                || !contains(config, static_cast<std::size_t>(source), kGlobalTableOffset + 4)) {
                return false;
            }
            std::uint32_t sourceOwn = 0, sourceNativeClass = 0;
            std::uint64_t sourceNative = 0;
            if (!read_value(config, static_cast<std::size_t>(source), sourceOwn)
                || sourceOwn != configTag
                || !read_value(config, static_cast<std::size_t>(source) + 4U, sourceNativeClass)
                || sourceNativeClass != kActorGroupClass
                || !read_value(config, static_cast<std::size_t>(source) + 8U, sourceNative)
                || sourceNative != native) {
                return false;
            }
            output.sourceOffset = static_cast<std::uint32_t>(source);
            if (!read_value(config, output.sourceOffset + kLocalTableOffset, output.localTag)
                || !read_value(
                    config, output.sourceOffset + kGlobalTableOffset, output.globalTag)) {
                return false;
            }
            output.present = true;
        }
    }
    state.sequenceOwners.emplace(configTag, output);
    return true;
}

/** Traverses only declared components; tag-shaped incidental words cannot create ownership. */
inline bool
actor(BuildState& state, std::uint32_t actorIndex, std::span<const std::byte> actorBlob) {
    TypedArray components{};
    if (!typed_array(
            actorBlob, kComponentArrayOffset, kComponentClass, kComponentStride, components)) {
        return false;
    }
    for (std::uint32_t ordinal = 0; ordinal < components.count; ++ordinal) {
        if (is_cancelled(state.cancel, state.cancelContext)) {
            return false;
        }
        const auto offset =
            static_cast<std::size_t>(components.dataOffset) + ordinal * kComponentStride;
        std::uint32_t configTag = 0;
        SequenceOwner source{};
        if (!read_value(actorBlob, offset, configTag) || !owner(state, configTag, source)) {
            return false;
        }
        if (!source.present) {
            continue;
        }
        format::ActorSequenceBinding binding{};
        binding.actorClassIndex = actorIndex;
        binding.componentOrdinal = ordinal;
        binding.configTag = configTag;
        binding.sourceOffset = source.sourceOffset;
        binding.sourceClass = format::kActorSequenceOwnerSourceClass;
        binding.flags = format::kActorSequenceExact;
        if (!table(state,
                   {source.globalTag, format::kActorSequenceGlobalDefinitionClass},
                   binding.globalTableIndex)
            || !table(state,
                      {source.localTag, format::kActorSequenceLocalDefinitionClass},
                      binding.localTableIndex)) {
            return false;
        }
        state.snapshot.sequenceBindings.push_back(binding);
    }
    return true;
}

/** FNV-verified names and path-derived symbols have distinct provenance. */
inline bool finish_names(Snapshot& snapshot) {
    std::unordered_map<std::uint32_t, std::string> names;
    for (const auto& entry : snapshot.sequenceEntries) {
        const auto candidate = basename(entry.sourcePath);
        if (candidate.empty()) {
            continue;
        }
        const auto key = name_hash(candidate);
        const auto [found, inserted] = names.emplace(key, candidate);
        if (!inserted && found->second != candidate) {
            found->second.clear();
        }
    }
    std::unordered_map<std::string, std::uint32_t> symbols;
    std::unordered_set<std::string> collisions;
    for (auto& entry : snapshot.sequenceEntries) {
        const auto named = names.find(entry.row.keyHash);
        if (named != names.end() && !named->second.empty()) {
            entry.name = named->second;
            entry.row.flags |= format::kActorSequenceNameVerified;
        }
        entry.symbol = symbol(basename(entry.sourcePath));
        if (!entry.symbol.empty()) {
            entry.row.flags |= format::kActorSequenceSymbolFromPath;
        } else {
            std::array<char, 32> fallback{};
            std::snprintf(fallback.data(), fallback.size(), "KEY_%08X", entry.row.keyHash);
            entry.symbol = fallback.data();
        }
        const auto [found, inserted] = symbols.emplace(entry.symbol, entry.row.keyHash);
        if (!inserted && found->second != entry.row.keyHash) {
            collisions.emplace(entry.symbol);
        }
    }
    for (auto& entry : snapshot.sequenceEntries) {
        if (collisions.contains(entry.symbol)) {
            std::array<char, 16> suffix{};
            std::snprintf(suffix.data(), suffix.size(), "_%08X", entry.row.keyHash);
            entry.symbol += suffix.data();
        }
    }
    symbols.clear();
    collisions.clear();
    for (const auto& entry : snapshot.sequenceEntries) {
        const auto [found, inserted] = symbols.emplace(entry.symbol, entry.row.keyHash);
        if (!inserted && found->second != entry.row.keyHash) {
            collisions.emplace(entry.symbol);
        }
    }
    for (auto& entry : snapshot.sequenceEntries) {
        if (collisions.contains(entry.symbol) || entry.symbol.starts_with("KEY_")) {
            std::array<char, 32> fallback{};
            std::snprintf(fallback.data(), fallback.size(), "KEY_%08X", entry.row.keyHash);
            entry.symbol = fallback.data();
            entry.row.flags &= ~format::kActorSequenceSymbolFromPath;
        }
    }
    return true;
}

/** Validates retained ownership and names before any strings are linked into the pack. */
inline bool validate(const Snapshot& snapshot) noexcept {
    std::size_t cursor = 0;
    for (std::size_t index = 0; index < snapshot.sequenceTables.size(); ++index) {
        const auto& table = snapshot.sequenceTables[index];
        const bool global = table.definitionClass == format::kActorSequenceGlobalDefinitionClass;
        if (table.flags != format::kActorSequenceExact || is_absent_tag(table.definitionTag)
            || (!global && table.definitionClass != format::kActorSequenceLocalDefinitionClass)
            || table.arrayOffset
                   != (global ? format::kActorSequenceGlobalArrayOffset
                              : format::kActorSequenceLocalArrayOffset)
            || table.entries.first != cursor || cursor > snapshot.sequenceEntries.size()
            || table.entries.count > snapshot.sequenceEntries.size() - cursor) {
            return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
            if (snapshot.sequenceTables[prior].definitionTag == table.definitionTag) {
                return false;
            }
        }
        for (std::uint32_t ordinal = 0; ordinal < table.entries.count; ++ordinal) {
            const auto& entry = snapshot.sequenceEntries[cursor + ordinal];
            const auto& row = entry.row;
            if (row.tableIndex != index || row.ordinal != ordinal || row.kind > 255
                || row.reserved != 0 || (row.flags & format::kActorSequenceExact) == 0
                || (row.flags & ~format::kActorSequenceEntryFlagMask) != 0 || entry.id.empty()
                || entry.symbol.empty() || entry.id.find('\0') != std::string::npos
                || entry.name.find('\0') != std::string::npos
                || entry.sourcePath.find('\0') != std::string::npos
                || (((row.flags & format::kActorSequenceNameVerified) != 0) != !entry.name.empty())
                || (!entry.name.empty() && name_hash(entry.name) != row.keyHash)
                || ((row.flags & format::kActorSequenceSymbolFromPath) != 0
                    && entry.sourcePath.empty())
                || (ordinal != 0
                    && snapshot.sequenceEntries[cursor + ordinal - 1].row.keyHash > row.keyHash)) {
                return false;
            }
            for (const char c : entry.symbol) {
                if (!(c >= 'A' && c <= 'Z') && !(c >= '0' && c <= '9') && c != '_') {
                    return false;
                }
            }
        }
        cursor += table.entries.count;
    }
    if (cursor != snapshot.sequenceEntries.size()) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.sequenceBindings.size(); ++index) {
        const auto& row = snapshot.sequenceBindings[index];
        if (row.actorClassIndex >= snapshot.actorClasses.size() || is_absent_tag(row.configTag)
            || row.flags != format::kActorSequenceExact
            || row.sourceClass != format::kActorSequenceOwnerSourceClass
            || (index != 0
                && (snapshot.sequenceBindings[index - 1].actorClassIndex > row.actorClassIndex
                    || (snapshot.sequenceBindings[index - 1].actorClassIndex == row.actorClassIndex
                        && snapshot.sequenceBindings[index - 1].componentOrdinal
                               >= row.componentOrdinal)))) {
            return false;
        }
        for (const bool global : {true, false}) {
            const auto table = global ? row.globalTableIndex : row.localTableIndex;
            if (table != format::kAbsentIndex
                && (table >= snapshot.sequenceTables.size()
                    || snapshot.sequenceTables[table].definitionClass
                           != (global ? format::kActorSequenceGlobalDefinitionClass
                                      : format::kActorSequenceLocalDefinitionClass))) {
                return false;
            }
        }
    }
    return true;
}

struct ScanContext final {
    std::vector<SequenceTableSource>* rows{};
    std::uint32_t definitionClass{};
};

/** The class scan records all tables, including definitions no current actor selects. */
inline bool visit_table(void* raw, std::uint32_t tag) noexcept {
    auto& context = *static_cast<ScanContext*>(raw);
    try {
        context.rows->push_back({tag, context.definitionClass});
        return true;
    } catch (...) {
        return false;
    }
}

/** Scans both physical definition classes without limiting them to currently bound actors. */
inline bool scan_tables(const reader::Source& source, std::vector<SequenceTableSource>& output) {
    output.clear();
    for (const auto cls : {format::kActorSequenceGlobalDefinitionClass,
                           format::kActorSequenceLocalDefinitionClass}) {
        ScanContext context{&output, cls};
        reader::ScanResult result{};
        if (!reader::scan_class(source.directory, cls, &visit_table, &context, result)) {
            return false;
        }
    }
    std::sort(output.begin(), output.end(), [](const auto& a, const auto& b) {
        return a.definitionTag < b.definitionTag;
    });
    return true;
}

} // namespace sequence_inventory
} // namespace sunrise::client::content::activity::sdk_generation::actor_rsat_inventory
