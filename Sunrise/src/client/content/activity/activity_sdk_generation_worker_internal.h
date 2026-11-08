#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../state/activity_sdk/generated_world/catalog_manifest.h"
#include "../../../state/activity_sdk/generated_world/format.h"
#include "../../../state/activity_sdk/generation/definition.h"
#include "../../../state/build_data/hash_names/definition.h"
#include "../../../state/build_data/scriptables/definition.h"
#include "../../../state/content/content_catalog.h"
#include "../scenarios/scenario_build.h"
#include "activity_sdk_activity_inventory.h"
#include "activity_sdk_external_placements.h"
#include "activity_sdk_generation_worker.h"
#include "activity_sdk_lua_artifacts.h"
#include "activity_sdk_topology_inventory.h"
#include "scriptable_catalog_builder.h"

namespace sunrise::client::content::activity::sdk_generation::worker_internal {

namespace generated_world = state::activity_sdk::generated_world;
namespace manifest = state::activity_sdk::generated_world::manifest;
namespace package_reader = middleware::content::packages::reader;
namespace catalog = state::build_data::scriptables;
namespace builder = ::sunrise::client::content::activity::scriptables::internal;
namespace inventory = activity_inventory;
namespace topology = topology_inventory;

/** Holds one step token plus the builder's own 96-byte refusal text. */
inline constexpr std::size_t kDetailCapacity = 160;

/** One stable scenario identity copied out of the package inventory. */
struct Scenario final {
    std::uint32_t tag{};
    std::array<char, catalog::kScenarioNameCapacity> name{};
    std::uint8_t nameLength{};
};

/** Inputs and owned state kept for one live or offline package pass. */
struct Work final {
    void* module{};
    package_reader::BlockKeys keys{};
    generated_world::Digest sourceFingerprint{};
    std::wstring packageDirectory{};
    std::wstring sdkDirectory{};
    std::wstring scenarioDirectory{};
    std::wstring catalogPath{};
    std::wstring packPath{};
    std::wstring cacheScenarioDirectory{};
    std::wstring cacheCatalogPath{};
    /** Owned client executable whose reflection metadata compiles the decoder cache. */
    std::wstring executablePath{};
    /** Base of the running client module; null in the offline pass, which reads the file. */
    const void* executableModule{};
    std::vector<Scenario> scenarios{};
    std::vector<manifest::ActivityRootRecord> activityRoots{};
    std::vector<manifest::ActivityVariantRecord> activityVariants{};
    inventory::Snapshot activityInventory{};
    topology::Snapshot canonicalTopology{};
    std::vector<lua_artifacts::ScenarioWorldSource> scenarioWorldSources{};
    /** Placements the scenarios load from container lists and type-4 descriptors. */
    external_placements::Index externalPlacements{};
    /** Layouts read from installed packages only. Built only when a mod serves package files. */
    std::unique_ptr<::sunrise::client::content::scenarios::LayoutSet> installedLayouts{};
    /** Named definitions read from installed packages only, built with the layouts. */
    std::vector<state::content::Definition> installedDefinitions{};
    /** Identifier names read from installed packages only, sorted by hash. */
    std::vector<state::build_data::hash_names::Name> installedHashNames{};
    /** Tables the scenario builds read. Null members read the ones State publishes. */
    builder::BuildTables tables{};
    generated_world::Digest payloadSha256{};
    std::uint64_t packBytes{};
    std::uint32_t luaFiles{};
    OfflineBuildResult* offlineResult{};
    OfflineBuildStatus* offlineStatus{};
    bool offline{};
    /** Writes the sdk/lua declaration tree. Live passes take it from the boot policy. */
    bool luaDeclarations{};
};

/** One indexed result keeps parallel work deterministic when threads finish out of order. */
struct ScenarioBuildResult final {
    manifest::Record record{};
    std::shared_ptr<const catalog::Snapshot> snapshot{};
    std::array<char, kDetailCapacity> detail{};
    bool attempted{};
    bool ready{};
    bool reused{};
};

/** @return True when the selected live or offline caller asked the pass to stop. */
[[nodiscard]] bool cancel_requested() noexcept;

/** Sends one worker event to the selected live or offline observer. */
void publish_progress(state::activity_sdk::generation::Status status,
                      std::uint32_t current,
                      std::uint32_t total,
                      std::uint32_t scenarioTag,
                      std::string_view detail,
                      bool determinate = true) noexcept;

/** Builds all scenario snapshots in parallel while retaining scenario-order output. */
[[nodiscard]] bool build_scenarios(Work& work,
                                   const package_reader::Source& source,
                                   const builder::ContainerIndex& containers,
                                   const manifest::Catalog& prior,
                                   bool priorReady,
                                   std::size_t firstScenario,
                                   std::size_t scenarioCount,
                                   std::vector<ScenarioBuildResult>& results);

/** Converts and checks the package-backed activity inventory used by every later stage. */
[[nodiscard]] bool build_inventory(Work& work, const package_reader::Source& source) noexcept;

/**
 * Builds the destination layouts, named definitions and identifier names from installed packages.
 * Needed when a mod serves package files, because the tables State publishes then include them.
 * @param work Pass whose scenario builds read the tables; its tables stay empty on failure.
 * @param source Installed-only package source with block keys.
 * @return True when all three tables are complete.
 */
[[nodiscard]] bool build_installed_tables(Work& work,
                                          const package_reader::Source& source) noexcept;

/** Emits transparent positional trigger declarations for one mission module. */
[[nodiscard]] bool build_world_source(const catalog::Snapshot& snapshot,
                                      lua_artifacts::ScenarioWorldSource& result);

/** @return True when the native binding partition is safe to publish. */
[[nodiscard]] bool binding_ready(const inventory::BindingCompleteness& completeness,
                                 std::size_t activityCount) noexcept;

/** @return True when the persisted binding partition is safe to consume. */
[[nodiscard]] bool binding_ready(const manifest::BindingCompleteness& completeness,
                                 std::size_t activityCount) noexcept;

/** Formats the content-addressed path for one exact scenario payload. */
[[nodiscard]] bool shard_path(const std::wstring& directory,
                              std::uint32_t scenarioTag,
                              const generated_world::Digest& digest,
                              std::wstring& output) noexcept;

/** Reopens one full shard into caller-owned snapshot storage. */
[[nodiscard]] bool load_record(const std::wstring& scenarioDirectory,
                               const generated_world::Digest& sourceFingerprint,
                               const Scenario& scenario,
                               const manifest::Record& record,
                               catalog::Snapshot& output) noexcept;

/** Reopens one full shard under shared immutable ownership. */
[[nodiscard]] bool load_full_record(const std::wstring& scenarioDirectory,
                                    const generated_world::Digest& sourceFingerprint,
                                    const Scenario& scenario,
                                    const manifest::Record& record,
                                    std::shared_ptr<const catalog::Snapshot>& output) noexcept;

/** Finds one tag in the sorted scenario catalog. */
[[nodiscard]] const manifest::Record* find_record(const manifest::Catalog& catalog,
                                                  std::uint32_t scenarioTag) noexcept;

/** Accepts one exact name or the native unnamed-scenario fallback. */
[[nodiscard]] bool record_name_matches(const manifest::Record& record,
                                       std::uint32_t scenarioTag,
                                       std::string_view requestedName) noexcept;

/**
 * Builds the pack, Lua, and catalog in the selected isolated output. Live work then commits only
 * its owned Lua subtree, pack, and catalog with catalog last and rollback.
 */
[[nodiscard]] bool publish_estate(Work& work,
                                  const package_reader::Source& source,
                                  std::span<const manifest::Record> records,
                                  const char*& failureDetail) noexcept;

} // namespace sunrise::client::content::activity::sdk_generation::worker_internal
