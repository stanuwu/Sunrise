#pragma once

#include <cstdint>
#include <span>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/activity_sdk/generation/pack_writer.h"
#include "../../../state/activity_sdk/identity.h"
#include "activity_sdk_activity_inventory.h"
#include "activity_sdk_decode_plans_image.h"
#include "activity_sdk_external_placements.h"
#include "activity_sdk_lua_artifacts.h"
#include "activity_sdk_topology_inventory.h"

namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline {

namespace pack = state::activity_sdk::generation::pack;

/** Exact fail-closed stages of one native all-activity publication. */
enum class Status : std::uint8_t {
    ready,
    cancelled,
    invalidInput,
    activityEnrichment,
    topologyEnrichment,
    squadFacts,
    actorRsat,
    squadLink,
    authoredSceneFacts,
    authoredSceneLinks,
    dialogueCues,
    authoredText,
    behaviors,
    policyInputs,
    policy,
    composition,
    identity,
    luaBuild,
    publication,
    luaPublication,
};

/** Real pack-building boundaries exposed to presentation without changing generation state. */
enum class Phase : std::uint8_t {
    activityMetadata,
    worldTopology,
    squadFacts,
    actorDefinitions,
    squadLinks,
    authoredSceneFacts,
    authoredSceneLinks,
    dialogueCues,
    authoredText,
    behaviors,
    actionPolicies,
    packTables,
    luaDeclarations,
    outputFiles,
};

/** Optional worker cancellation probe checked across package-backed stages. */
using CancelProbe = bool (*)(void* context) noexcept;

/** Optional observer used only to present the current pack-building phase. */
using ProgressProbe = void (*)(void* context, Phase phase) noexcept;

/** Final identity returned only after every requested output commits. */
struct Result final {
    state::activity_sdk::identity::Expected identity{};
    pack::Digest payloadSha256{};
    std::uint64_t fileBytes{};
    std::uint32_t luaFiles{};
};

/** @return Stable diagnostic name for one pipeline outcome. */
[[nodiscard]] const char* status_name(Status value) noexcept;

/**
 * Builds and publishes the current activity and panel SDK without touching runtime State.
 * The caller may use an isolated sibling tree and commit it after its wider estate validates.
 * @param luaDeclarations Writes the sdk/lua tree. Off leaves an empty lua directory for the commit.
 */
[[nodiscard]] Status stage(const wchar_t* sdkDirectory,
                           const wchar_t* packPath,
                           const middleware::content::packages::reader::Source& source,
                           const decode_plans::Image& reflectionImage,
                           const pack::Digest& sourceFingerprint,
                           const activity_inventory::Snapshot& activities,
                           topology_inventory::Snapshot& topology,
                           std::span<const lua_artifacts::ScenarioWorldSource> scenarioWorldSources,
                           const external_placements::Index& externalPlacements,
                           bool luaDeclarations,
                           CancelProbe cancel,
                           void* cancelContext,
                           ProgressProbe progress,
                           void* progressContext,
                           Result& output) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline
