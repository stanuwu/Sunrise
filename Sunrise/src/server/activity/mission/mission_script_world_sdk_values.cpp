#include "mission_script_world_sdk_values.h"

#include <cstdint>
#include <string_view>

namespace sunrise::server::activity::mission::sdk_bridge {
namespace {

namespace sdk = state::activity_sdk;
namespace generated_world = state::activity_sdk::generated_world;
namespace catalog = state::build_data::scriptables;

/** Every digest, generation, and row identity of a bound view must still match. */
[[nodiscard]] bool same_generation(const generated_world::GeneratedWorldView& world,
                                   const lua_vm::WorldGenerationIdentity& expected) noexcept {
    const sdk::BoundView& activity = world.activity_sdk_view();
    const generated_world::GenerationIdentity& identity = world.generation_identity();
    return world.snapshot() != nullptr && activity.catalog != nullptr
           && identity.sdkBuildSha256 == expected.sdkBuildSha256
           && identity.sdkPayloadSha256 == expected.sdkPayloadSha256
           && identity.sourceFingerprint == expected.sourceFingerprint
           && identity.manifestPayloadSha256 == expected.manifestPayloadSha256
           && identity.shardPayloadSha256 == expected.shardPayloadSha256
           && activity.activityClientGeneration == expected.activityClientGeneration
           && activity.activityRow == expected.activityRow
           && world.scenario_tag() == expected.scenarioTag;
}

} // namespace

bool field_u64(lua_vm::WorldFieldDefinition& output,
               std::uint64_t value,
               lua_vm::WorldFieldKind kind) noexcept {
    output = {};
    output.kind = kind;
    output.unsignedValue = value;
    return true;
}

bool field_i64(lua_vm::WorldFieldDefinition& output,
               std::int64_t value,
               lua_vm::WorldFieldKind kind) noexcept {
    output = {};
    output.kind = kind;
    output.signedValue = value;
    return true;
}

bool field_bool(lua_vm::WorldFieldDefinition& output, bool value) noexcept {
    return field_u64(output, value ? 1U : 0U, lua_vm::WorldFieldKind::boolean);
}

bool field_string(lua_vm::WorldFieldDefinition& output,
                  std::string_view value,
                  bool optional) noexcept {
    output = {};
    output.kind =
        optional ? lua_vm::WorldFieldKind::optionalString : lua_vm::WorldFieldKind::string;
    output.stringValue = value;
    return true;
}

bool field_row(lua_vm::WorldFieldDefinition& output, std::uint32_t zeroBased) noexcept {
    return field_u64(output, one_based(zeroBased), lua_vm::WorldFieldKind::optionalRow);
}

bool field_first_row(lua_vm::WorldFieldDefinition& output,
                     std::uint32_t zeroBased,
                     std::uint32_t count) noexcept {
    return field_u64(output, count == 0 ? 0U : zeroBased + 1U, lua_vm::WorldFieldKind::optionalRow);
}

std::uint32_t one_based(std::uint32_t row) noexcept {
    return row == catalog::kNoRow ? 0U : row + 1U;
}

/** Maps one name provenance to its Lua spelling. */
const char* provenance_text(std::uint32_t value) noexcept {
    switch (static_cast<catalog::NameProvenance>(value)) {
    case catalog::NameProvenance::unresolved:
        return "unresolved";
    case catalog::NameProvenance::packageInline:
        return "package_inline";
    case catalog::NameProvenance::packagePath:
        return "package_path";
    case catalog::NameProvenance::packageIdentifierCandidate:
        return "package_identifier_candidate";
    }
    return nullptr;
}

/** Maps one group safety verdict to its Lua spelling. */
const char* safety_text(catalog::GroupSafety value) noexcept {
    switch (value) {
    case catalog::GroupSafety::notApplicable:
        return "not_applicable";
    case catalog::GroupSafety::destinationSafe:
        return "destination_safe";
    case catalog::GroupSafety::bubbleSafe:
        return "bubble_safe";
    case catalog::GroupSafety::stateOnly:
        return "state_only";
    case catalog::GroupSafety::incomplete:
        return "incomplete";
    case catalog::GroupSafety::ambiguous:
        return "ambiguous";
    }
    return nullptr;
}

/** Maps one typed-reference join outcome to its Lua spelling. */
const char* reference_join_text(catalog::ReferenceJoin value) noexcept {
    switch (value) {
    case catalog::ReferenceJoin::unresolved:
        return "unresolved";
    case catalog::ReferenceJoin::exact:
        return "exact";
    case catalog::ReferenceJoin::ambiguous:
        return "ambiguous";
    }
    return nullptr;
}

/** Maps one spatial context join to its Lua spelling. */
const char* spatial_context_text(catalog::SpatialContextJoin value) noexcept {
    switch (value) {
    case catalog::SpatialContextJoin::unresolved:
        return "unresolved";
    case catalog::SpatialContextJoin::packageObjectState:
        return "package_object_state";
    case catalog::SpatialContextJoin::packageStemBubble:
        return "package_stem_bubble";
    }
    return nullptr;
}

/** Maps one authored squad point join outcome to its Lua spelling. */
const char* point_context_status_text(catalog::AuthoredSquadPointContextStatus value) noexcept {
    switch (value) {
    case catalog::AuthoredSquadPointContextStatus::unresolved:
        return "unresolved";
    case catalog::AuthoredSquadPointContextStatus::exact:
        return "exact";
    case catalog::AuthoredSquadPointContextStatus::ambiguous:
        return "ambiguous";
    }
    return nullptr;
}

/** Follows a name or tag-name row to its selected candidate, retaining the evidence tier. */
lua_vm::WorldNameDefinition
selected_name(const catalog::Snapshot& snapshot, std::uint32_t row, bool tagName) noexcept {
    lua_vm::WorldNameDefinition output{};
    std::uint32_t selected = catalog::kNoRow;
    if (tagName) {
        if (row >= snapshot.tagNames.size()) {
            return output;
        }
        const catalog::TagName& name = snapshot.tagNames[row];
        output.candidateCount = name.candidateCount;
        output.provenance = static_cast<std::uint32_t>(name.provenance);
        selected = name.selectedCandidate;
    } else {
        if (row >= snapshot.names.size()) {
            return output;
        }
        const catalog::Name& name = snapshot.names[row];
        output.candidateCount = name.candidateCount;
        output.provenance = static_cast<std::uint32_t>(name.provenance);
        output.strongestTierOverflow = name.strongestTierOverflow;
        selected = name.selectedCandidate;
    }
    if (selected >= snapshot.nameCandidates.size()) {
        return output;
    }
    const catalog::NameCandidate& candidate = snapshot.nameCandidates[selected];
    if (candidate.length > candidate.value.size()) {
        return {};
    }
    output.value = {candidate.value.data(), candidate.length};
    output.sourceTag = candidate.sourceTag;
    output.sourceClassId = candidate.sourceClassId;
    return output;
}

/** Answers one selected-name suffix, or refuses a key that does not carry the prefix. */
bool selected_name_field(const catalog::Snapshot& snapshot,
                         std::uint32_t row,
                         bool tagName,
                         std::string_view key,
                         std::string_view prefix,
                         lua_vm::WorldFieldDefinition& output) noexcept {
    if (!key.starts_with(prefix)) {
        return false;
    }
    const std::string_view suffix = key.substr(prefix.size());
    const lua_vm::WorldNameDefinition name = selected_name(snapshot, row, tagName);
    if (suffix == "_name") {
        return field_string(output, name.value, true);
    }
    if (suffix == "_name_provenance") {
        const char* const text = provenance_text(name.provenance);
        return field_string(output, text == nullptr ? std::string_view{} : text, true);
    }
    if (suffix == "_name_provenance_code") {
        return field_u64(output, name.provenance);
    }
    if (suffix == "_name_candidate_count") {
        return field_u64(output, name.candidateCount);
    }
    if (suffix == "_name_source_tag") {
        return field_u64(output, name.sourceTag);
    }
    if (suffix == "_name_source_class") {
        return field_u64(output, name.sourceClassId);
    }
    if (suffix == "_name_strongest_tier_overflow") {
        return field_bool(output, name.strongestTierOverflow);
    }
    return false;
}

const generated_world::GeneratedWorldView*
checked_world(const void* context, const lua_vm::WorldGenerationIdentity& generation) noexcept {
    const auto* const world = static_cast<const generated_world::GeneratedWorldView*>(context);
    return world != nullptr && same_generation(*world, generation) ? world : nullptr;
}

} // namespace sunrise::server::activity::mission::sdk_bridge
