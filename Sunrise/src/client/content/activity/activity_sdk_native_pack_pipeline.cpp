#include "activity_sdk_native_pack_pipeline.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "../../../state/activity_sdk/runtime.h"
#include "activity_sdk_activity_enrichment_inventory.h"
#include "activity_sdk_actor_rsat_inventory.h"
#include "activity_sdk_authored_scene_inventory.h"
#include "activity_sdk_behavior_inventory.h"
#include "activity_sdk_lua_artifacts.h"
#include "activity_sdk_native_pack_internal.h"
#include "activity_sdk_pack_composer.h"
#include "activity_sdk_policy_input_adapter.h"
#include "activity_sdk_policy_inventory.h"
#include "activity_sdk_squad_inventory.h"
#include "activity_sdk_topology_enrichment.h"

namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline {

bool cancelled(CancelProbe probe, void* context) noexcept {
    return probe != nullptr && probe(context);
}

/** Adapts the checked package reader to the squad and authored-scene boundaries. */
bool read_tag(void* opaque,
              std::uint32_t tag,
              std::vector<std::byte>& bytes,
              std::uint32_t& classId) noexcept {
    bytes.clear();
    if (opaque == nullptr) {
        return false;
    }
    auto& context = *static_cast<PackageContext*>(opaque);
    return context.source != nullptr && context.scratch != nullptr
           && !cancelled(context.cancel, context.cancelContext)
           && reader::read_tag(*context.source, *context.scratch, tag, bytes, classId);
}

bool read_localized_tag(void* opaque,
                        std::uint32_t tag,
                        std::uint32_t expectedClass,
                        std::vector<std::byte>& bytes) noexcept {
    std::uint32_t classId = 0;
    return read_tag(opaque, tag, bytes, classId) && classId == expectedClass;
}
namespace {

namespace format = state::activity_sdk::format;
namespace activity_enrichment = activity_enrichment_inventory;
namespace actor_rsat = actor_rsat_inventory;
namespace authored_scene = authored_scene_inventory;
namespace behaviors = behavior_inventory;
namespace lua = lua_artifacts;
namespace composer = pack_composer;
namespace policy_adapter = policy_input_adapter;
namespace policy = policy_inventory;
namespace squads = squad_inventory;
namespace topology_enrichment = sdk_generation::topology_enrichment;

static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));

/** Appends one live tag to the caller's vector. @return False when the tag or sink is unusable. */
[[nodiscard]] bool collect_tag(void* opaque, std::uint32_t tag) noexcept {
    if (opaque == nullptr || tag == 0 || tag == format::kAbsentIndex) {
        return false;
    }
    try {
        static_cast<std::vector<std::uint32_t>*>(opaque)->push_back(tag);
        return true;
    } catch (...) {
        return false;
    }
}

void report(ProgressProbe probe, void* context, Phase phase) noexcept {
    if (probe != nullptr) {
        probe(context, phase);
    }
}

/** Creates the empty lua child the publication commit renames when declarations are off. */
[[nodiscard]] bool ensure_lua_directory(const wchar_t* sdkDirectory) noexcept {
    std::wstring path;
    try {
        path.assign(sdkDirectory);
        path.append(L"\\lua");
    } catch (...) {
        return false;
    }
    if (CreateDirectoryW(path.c_str(), nullptr) != FALSE) {
        return true;
    }
    if (GetLastError() != ERROR_ALREADY_EXISTS) {
        return false;
    }
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
/** Builds one schema fact for every final catalog-global slot row. */
[[nodiscard]] bool build_slot_schemas(const topology_inventory::Snapshot& topology,
                                      const topology_enrichment::Snapshot& enrichment,
                                      std::vector<squads::SlotSchemaFact>& output) {
    output.clear();
    if (topology.slots.size() != enrichment.slots.size()
        || topology.slots.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    try {
        output.reserve(topology.slots.size());
        for (std::uint32_t index = 0; index < topology.slots.size(); ++index) {
            const topology_enrichment::Slot& slot = enrichment.slots[index];
            output.push_back({index,
                              slot.componentClass,
                              slot.senseSchema,
                              slot.authSchema,
                              (slot.flags & format::kSlotSchemaJoinExact) != 0});
        }
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Resolves one exact actor definition tag against the validated sorted actor section. */
[[nodiscard]] bool
resolve_actor(void* opaque, std::uint32_t definitionTag, std::uint32_t& output) noexcept {
    output = format::kAbsentIndex;
    if (opaque == nullptr) {
        return false;
    }
    const auto& actors = *static_cast<const std::vector<actor_rsat::ActorClass>*>(opaque);
    const auto found = std::lower_bound(actors.begin(),
                                        actors.end(),
                                        definitionTag,
                                        [](const actor_rsat::ActorClass& row, std::uint32_t tag) {
                                            return row.definitionTag < tag;
                                        });
    if (found == actors.end() || found->definitionTag != definitionTag
        || static_cast<std::size_t>(found - actors.begin()) >= format::kAbsentIndex) {
        return false;
    }
    output = static_cast<std::uint32_t>(found - actors.begin());
    return true;
}

/** Converts the runtime trust identity to the writer's header-only identity. */
[[nodiscard]] pack::Identity
pack_identity(const state::activity_sdk::identity::Expected& identity) noexcept {
    return {identity.sdkBuildSha256, identity.contentKeySha256, identity.logicalIrSha256};
}

/** Borrows all declaration-bearing final sections for one Lua artifact transaction. */
[[nodiscard]] lua::Source
lua_source(const state::activity_sdk::identity::Expected& identity,
           const composer::Storage& storage,
           std::span<const lua::ScenarioWorldSource> worldSources) noexcept {
    return {identity.sdkBuildSha256,
            identity.payloadSha256,
            identity.contentKeySha256,
            identity.logicalIrSha256,
            storage.strings,
            storage.activities,
            storage.scenarios,
            storage.bubbles,
            storage.states,
            storage.objects,
            storage.occurrences,
            storage.slots,
            storage.squads,
            storage.squadMembers,
            storage.squadAnchors,
            storage.authoredSceneResources,
            storage.authoredSceneSquadEdges,
            storage.taskTargets,
            storage.dialogueCueTexts,
            storage.directiveElements,
            storage.behaviorPrograms,
            storage.behaviorInputs,
            storage.behaviorChannelWrites,
            storage.behaviorOwners,
            storage.behaviorActivityBindings,
            storage.actorClasses,
            storage.actorMessageSchemas,
            storage.actorCommandDefinitions,
            storage.actorBehaviorProfiles,
            storage.simulationEventDefinitions,
            storage.runtimeSchemas,
            storage.runtimeFields,
            storage.runtimeTypeDefinitions,
            storage.sobjectRsats,
            storage.sobjectRsatDescriptors,
            storage.entityTypeDefinitions,
            storage.sobjectRsatFieldBindings,
            storage.actorStateNames,
            storage.actorSequenceTables,
            storage.actorSequenceEntries,
            storage.actorSequenceBindings,
            worldSources};
}

} // namespace

/** Returns the stable log token for each pipeline result. */
const char* status_name(Status value) noexcept {
    switch (value) {
    case Status::ready:
        return "ready";
    case Status::cancelled:
        return "cancelled";
    case Status::invalidInput:
        return "invalid_input";
    case Status::activityEnrichment:
        return "activity_enrichment";
    case Status::topologyEnrichment:
        return "topology_enrichment";
    case Status::squadFacts:
        return "squad_facts";
    case Status::actorRsat:
        return "actor_rsat";
    case Status::squadLink:
        return "squad_link";
    case Status::authoredSceneFacts:
        return "authored_scene_facts";
    case Status::authoredSceneLinks:
        return "authored_scene_links";
    case Status::dialogueCues:
        return "dialogue_cues";
    case Status::authoredText:
        return "authored_text";
    case Status::behaviors:
        return "behaviors";
    case Status::policyInputs:
        return "policy_inputs";
    case Status::policy:
        return "policy";
    case Status::composition:
        return "composition";
    case Status::identity:
        return "identity";
    case Status::luaBuild:
        return "lua_build";
    case Status::publication:
        return "publication";
    case Status::luaPublication:
        return "lua_publication";
    case Status::reload:
        return "reload";
    }
    return "invalid_input";
}

/** Builds and publishes one canonical native SDK generation without a runtime reload. */
Status stage(const wchar_t* sdkDirectory,
             const wchar_t* packPath,
             const reader::Source& source,
             const pack::Digest& sourceFingerprint,
             const activity_inventory::Snapshot& activities,
             topology_inventory::Snapshot& topology,
             std::span<const lua::ScenarioWorldSource> scenarioWorldSources,
             const external_placements::Index& externalPlacements,
             bool luaDeclarations,
             CancelProbe cancel,
             void* cancelContext,
             ProgressProbe progress,
             void* progressContext,
             Result& output) noexcept {
    output = {};
    if (sdkDirectory == nullptr || sdkDirectory[0] == L'\0' || packPath == nullptr
        || packPath[0] == L'\0' || source.directory.empty() || source.keys == nullptr
        || !state::activity_sdk::identity::valid(sourceFingerprint) || !topology.ready) {
        return Status::invalidInput;
    }
    if (cancelled(cancel, cancelContext)) {
        return Status::cancelled;
    }
    try {
        PackageContext packageContext{
            &source,
            std::unique_ptr<reader::Scratch>(new (std::nothrow) reader::Scratch()),
            cancel,
            cancelContext};
        if (packageContext.scratch == nullptr || cancelled(cancel, cancelContext)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled
                                                    : Status::activityEnrichment;
        }
        report(progress, progressContext, Phase::activityMetadata);
        activity_enrichment::Snapshot activityNames{};
        if (!activity_enrichment::build(source, *packageContext.scratch, activities, activityNames)
            || !activity_enrichment::apply(activityNames, topology)) {
            return Status::activityEnrichment;
        }

        report(progress, progressContext, Phase::worldTopology);
        topology_enrichment::Snapshot topologyDetails{};
        if (!topology_enrichment::build_generated(topology, topologyDetails)) {
            return Status::topologyEnrichment;
        }
        if (cancelled(cancel, cancelContext)) {
            return Status::cancelled;
        }

        report(progress, progressContext, Phase::squadFacts);
        squads::Facts squadFacts{};
        if (!squads::collect_facts(topology, &read_tag, &packageContext, squadFacts)
            || !squads::append_external_placements(topology, externalPlacements, squadFacts)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled : Status::squadFacts;
        }

        report(progress, progressContext, Phase::actorDefinitions);
        actor_rsat::Snapshot actorRows{};
        std::vector<std::uint32_t> installedRsats{};
        reader::ScanResult rsatScan{};
        if (!reader::scan_class(source.directory,
                                actor_rsat::kActorRsatClass,
                                &collect_tag,
                                &installedRsats,
                                rsatScan)) {
            return Status::actorRsat;
        }
        std::sort(installedRsats.begin(), installedRsats.end());
        installedRsats.erase(std::unique(installedRsats.begin(), installedRsats.end()),
                             installedRsats.end());
        if (!actor_rsat::build_with_rsats(source,
                                          squadFacts.actorDefinitionTags,
                                          installedRsats,
                                          cancel,
                                          cancelContext,
                                          actorRows)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled : Status::actorRsat;
        }
        report(progress, progressContext, Phase::squadLinks);
        std::vector<squads::SlotSchemaFact> slotSchemas{};
        if (!build_slot_schemas(topology, topologyDetails, slotSchemas)) {
            return Status::squadLink;
        }
        squadFacts.slotSchemas = std::move(slotSchemas);
        squads::Snapshot squadRows{};
        if (!squads::link(
                topology, squadFacts, &resolve_actor, &actorRows.actorClasses, squadRows)) {
            return Status::squadLink;
        }
        report(progress, progressContext, Phase::authoredSceneFacts);
        authored_scene::Facts sceneFacts{};
        if (!authored_scene::derive_facts(topology, squadFacts, sceneFacts)) {
            return Status::authoredSceneFacts;
        }
        report(progress, progressContext, Phase::authoredSceneLinks);
        authored_scene::Snapshot sceneRows{};
        if (!authored_scene::build(topology, sceneFacts, &read_tag, &packageContext, sceneRows)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled
                                                    : Status::authoredSceneLinks;
        }
        report(progress, progressContext, Phase::dialogueCues);
        if (!attach_dialogue_cue_counts(topology, squadFacts, packageContext, topologyDetails)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled : Status::dialogueCues;
        }
        report(progress, progressContext, Phase::authoredText);
        if (!attach_authored_text(topology, squadFacts, packageContext, sceneRows)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled : Status::authoredText;
        }
        if (cancelled(cancel, cancelContext)) {
            return Status::cancelled;
        }

        report(progress, progressContext, Phase::behaviors);
        behaviors::Snapshot behaviorRows{};
        if (!behaviors::build(
                source, squadFacts.actorDefinitionTags, cancel, cancelContext, behaviorRows)) {
            return cancelled(cancel, cancelContext) ? Status::cancelled : Status::behaviors;
        }

        report(progress, progressContext, Phase::actionPolicies);
        policy_adapter::Snapshot policyInputs{};
        if (!policy_adapter::build(activities, topology, topologyDetails, policyInputs)) {
            return Status::policyInputs;
        }
        policy::Snapshot policyRows{};
        if (!policy::build(policyInputs.view(), policyRows)) {
            return Status::policy;
        }
        if (cancelled(cancel, cancelContext)) {
            return Status::cancelled;
        }

        report(progress, progressContext, Phase::packTables);
        const composer::Inputs inputs{&activities,
                                      &activityNames,
                                      &topology,
                                      &topologyDetails,
                                      &policyRows,
                                      &actorRows,
                                      &squadFacts,
                                      &squadRows,
                                      &sceneRows,
                                      &behaviorRows};
        composer::Storage storage{};
        if (!composer::compose_generated(inputs, storage)) {
            return Status::composition;
        }
        pack::PreparedImage image{};
        if (pack::prepare(storage.tables(), image) != pack::Status::ready) {
            return Status::composition;
        }
        const pack::Digest payload = image.payload_sha256();
        const std::uint64_t fileBytes = image.file_size();
        state::activity_sdk::identity::Expected identity{};
        if (!state::activity_sdk::identity::derive(sourceFingerprint, payload, identity)) {
            return Status::identity;
        }
        if (cancelled(cancel, cancelContext)) {
            return Status::cancelled;
        }

        lua::Bundle luaBundle{};
        if (luaDeclarations) {
            report(progress, progressContext, Phase::luaDeclarations);
            if (lua::build(lua_source(identity, storage, scenarioWorldSources), luaBundle)
                != lua::Status::ready) {
                return Status::luaBuild;
            }
        }

        report(progress, progressContext, Phase::outputFiles);
        pack::Digest written{};
        if (pack::publish(packPath, pack_identity(identity), std::move(image), written)
            != pack::Status::ready) {
            return Status::publication;
        }
        lua::Result luaResult{};
        // The commit renames a lua directory whether or not it holds declarations.
        if (!luaDeclarations) {
            if (!ensure_lua_directory(sdkDirectory)) {
                return Status::luaPublication;
            }
        } else {
            if (lua::publish_bundle(sdkDirectory, luaBundle, luaResult) != lua::Status::ready) {
                return Status::luaPublication;
            }
        }
        output.identity = identity;
        output.payloadSha256 = payload;
        output.fileBytes = fileBytes;
        output.luaBytes = luaResult.byteCount;
        output.luaFiles = luaResult.fileCount;
        return Status::ready;
    } catch (...) {
        output = {};
        return cancelled(cancel, cancelContext) ? Status::cancelled : Status::invalidInput;
    }
}

/** Builds, publishes, and reloads one canonical native SDK generation. */
Status publish(void* module,
               const wchar_t* sdkDirectory,
               const wchar_t* packPath,
               const reader::Source& source,
               const pack::Digest& sourceFingerprint,
               const activity_inventory::Snapshot& activities,
               topology_inventory::Snapshot& topology,
               const external_placements::Index& externalPlacements,
               bool luaDeclarations,
               CancelProbe cancel,
               void* cancelContext,
               Result& output) noexcept {
    output = {};
    if (module == nullptr) {
        return Status::invalidInput;
    }
    const Status staged = stage(sdkDirectory,
                                packPath,
                                source,
                                sourceFingerprint,
                                activities,
                                topology,
                                {},
                                externalPlacements,
                                luaDeclarations,
                                cancel,
                                cancelContext,
                                nullptr,
                                nullptr,
                                output);
    if (staged != Status::ready) {
        return staged;
    }
    if (!state::activity_sdk::reload(module, output.identity)) {
        output = {};
        return Status::reload;
    }
    // A reload replaces the catalog the wire descriptors borrow, so they are rebuilt here.
    return Status::ready;
}

} // namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline
