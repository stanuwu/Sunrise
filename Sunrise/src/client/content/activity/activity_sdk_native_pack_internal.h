#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "activity_sdk_authored_scene_inventory.h"
#include "activity_sdk_native_pack_pipeline.h"
#include "activity_sdk_squad_inventory.h"
#include "activity_sdk_topology_enrichment.h"

namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline {

namespace reader = middleware::content::packages::reader;
namespace authored_scene = authored_scene_inventory;
namespace squads = squad_inventory;
namespace topology_enrichment = sdk_generation::topology_enrichment;

/** One cache-owning package reader shared by squad and authored-scene extraction. */
struct PackageContext final {
    const reader::Source* source{};
    std::unique_ptr<reader::Scratch> scratch{};
    CancelProbe cancel{};
    void* cancelContext{};

    ~PackageContext() noexcept {
        if (scratch != nullptr) {
            reader::close_files(*scratch);
        }
    }
};

/** @return True when the caller asked this pass to stop. */
[[nodiscard]] bool cancelled(CancelProbe probe, void* context) noexcept;

/** Adapts the checked package reader to the squad and authored-scene boundaries. */
[[nodiscard]] bool read_tag(void* opaque,
                            std::uint32_t tag,
                            std::vector<std::byte>& bytes,
                            std::uint32_t& classId) noexcept;

/** Reads one tag only when its physical class matches the typed edge being followed. */
[[nodiscard]] bool read_localized_tag(void* opaque,
                                      std::uint32_t tag,
                                      std::uint32_t expectedClass,
                                      std::vector<std::byte>& bytes) noexcept;

/** Extracts localized dialogue aliases and safe authored directive elements. */
[[nodiscard]] bool attach_authored_text(const topology_inventory::Snapshot& topology,
                                        const topology_enrichment::Snapshot& enrichment,
                                        const squads::Facts& facts,
                                        PackageContext& packageContext,
                                        authored_scene::Snapshot& output);

/** Extracts the groups each exact combat objective can assign to a squad. */
[[nodiscard]] bool attach_combat_objective_groups(const topology_inventory::Snapshot& topology,
                                                  const squads::Facts& facts,
                                                  PackageContext& packageContext,
                                                  authored_scene::Snapshot& output);

/** Resolves the native type-53 authored list and attaches its exact bound to the SDK slot row. */
[[nodiscard]] bool attach_dialogue_cue_counts(const topology_inventory::Snapshot& topology,
                                              const squads::Facts& facts,
                                              PackageContext& packageContext,
                                              topology_enrichment::Snapshot& enrichment);

} // namespace sunrise::client::content::activity::sdk_generation::native_pack_pipeline
