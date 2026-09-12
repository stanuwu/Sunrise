#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "../../../state/activity_sdk/format.h"

namespace sunrise::state::activity_sdk {
class Catalog;
}

namespace sunrise::client::content::activity::sdk_generation::lua_artifacts {

namespace format = state::activity_sdk::format;

/** Direct Lua declarations derived from one generated-world scenario shard. */
struct ScenarioWorldSource final {
    std::uint32_t scenarioTag{};
    std::string source{};
};

/** Generation and publication outcomes stay stable for worker diagnostics. */
enum class Status : std::uint8_t {
    ready,
    invalidInput,
    unsupportedAbi,
    buildFailure,
    directoryFailure,
    writeFailure,
};

/** Borrowed named mission rows used to write direct Lua source modules. */
struct Source final {
    std::array<std::byte, 32> sdkBuildSha256{};
    std::array<std::byte, 32> sdkPayloadSha256{};
    std::array<std::byte, 32> contentKeySha256{};
    std::array<std::byte, 32> logicalIrSha256{};
    std::span<const std::byte> strings{};
    std::span<const format::Activity> activities{};
    std::span<const format::Scenario> scenarios{};
    std::span<const format::Bubble> bubbles{};
    std::span<const format::State> states{};
    std::span<const format::Object> objects{};
    std::span<const format::Occurrence> occurrences{};
    std::span<const format::Slot> slots{};
    std::span<const format::Squad> squads{};
    std::span<const format::SquadMember> squadMembers{};
    std::span<const format::SquadAnchor> squadAnchors{};
    std::span<const format::AuthoredSceneResource> authoredSceneResources{};
    std::span<const format::AuthoredSceneSquadEdge> authoredSceneSquadEdges{};
    std::span<const format::TaskTarget> taskTargets{};
    std::span<const format::DialogueCueText> dialogueCueTexts{};
    std::span<const format::DirectiveElement> directiveElements{};
    std::span<const format::BehaviorProgram> behaviorPrograms{};
    std::span<const format::BehaviorInput> behaviorInputs{};
    std::span<const format::BehaviorChannelWrite> behaviorChannelWrites{};
    std::span<const format::BehaviorOwner> behaviorOwners{};
    std::span<const format::BehaviorActivityBinding> behaviorActivityBindings{};
    std::span<const format::ActorClass> actorClasses{};
    std::span<const format::ActorMessageSchema> actorMessageSchemas{};
    std::span<const format::ActorCommandDefinition> actorCommandDefinitions{};
    std::span<const format::ActorBehaviorProfile> actorBehaviorProfiles{};
    std::span<const format::SimulationEventDefinition> simulationEventDefinitions{};
    std::span<const format::RuntimeSchema> runtimeSchemas{};
    std::span<const format::RuntimeField> runtimeFields{};
    std::span<const format::RuntimeTypeDefinition> runtimeTypeDefinitions{};
    std::span<const format::SobjectRsat> sobjectRsats{};
    std::span<const format::SobjectRsatDescriptor> sobjectRsatDescriptors{};
    std::span<const format::EntityTypeDefinition> entityTypeDefinitions{};
    std::span<const format::SobjectRsatFieldBinding> sobjectRsatFieldBindings{};
    std::span<const format::ActorStateName> actorStateNames{};
    std::span<const format::ActorSequenceTable> actorSequenceTables{};
    std::span<const format::ActorSequenceEntry> actorSequenceEntries{};
    std::span<const format::ActorSequenceBinding> actorSequenceBindings{};
    std::span<const ScenarioWorldSource> scenarioWorldSources{};
};

/** One readable generated Lua module with a filesystem-safe canonical name. */
struct SourceModule final {
    std::string stem{};
    std::string source{};
};

/** Complete in-memory transaction before any artifact path changes. */
struct Bundle final {
    std::string activityIndex{};
    std::string missionIndex{};
    std::string activitySdkModule{};
    std::string behaviorModule{};
    std::string manifestJson{};
    std::vector<SourceModule> activityModules{};
    std::vector<SourceModule> missionModules{};
};

/** Counts and bytes returned only after the index commit file is published. */
struct Result final {
    std::uint32_t activityCount{};
    std::uint32_t fileCount{};
    std::uint64_t byteCount{};
};

/** @return Stable text for one native Lua artifact result. */
[[nodiscard]] const char* status_name(Status value) noexcept;

/** Builds every declaration in memory without touching the artifact tree. */
[[nodiscard]] Status build(const Source& source, Bundle& output) noexcept;

/** Publishes complete files below sdkDirectory/lua and commits activities.lua last. */
[[nodiscard]] Status
publish_bundle(const wchar_t* sdkDirectory, const Bundle& bundle, Result& output) noexcept;

/** Builds then publishes one complete declaration transaction. */
[[nodiscard]] Status
publish(const wchar_t* sdkDirectory, const Source& source, Result& output) noexcept;

/** @return True when the committed Lua manifest exactly matches the loaded SDK. */
[[nodiscard]] bool is_current(const wchar_t* sdkDirectory,
                              const state::activity_sdk::Catalog& catalog) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::lua_artifacts
