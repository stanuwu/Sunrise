#include "spawn_set_build.h"

#include <Windows.h>

#include <array>
#include <cstdio>
#include <span>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/reader/parallel.h"
#include "../../../middleware/content/packages/tables/component_container_reader.h"
#include "../../../middleware/content/packages/tables/spawn_reader.h"
#include "../../../state/build_data/runtime.h"
#include "spawn_set_catalog_builder.h"

namespace sunrise::client::content::spawn_sets {
namespace {

namespace reader = middleware::content::packages::reader;
namespace tables = middleware::content::packages::tables;

/** Package-reader inputs adapted to the injected blob-reader boundary. */
struct ReadContext {
    const reader::Source* source{};
    reader::Scratch* scratch{};
    std::vector<std::byte>* bytes{};
    /** Blobs the last read-ahead kept, in the order the pass asks for them. */
    std::vector<reader::parallel::Held>* kept{};
    /** Row of that run the pass has reached. */
    std::size_t keptCursor{};
};

/** @param context Pass storage. @param entry Scan match. @return True when the row fits. */
[[nodiscard]] bool collect(void* context, const reader::ClassEntry& entry) noexcept {
    return add_entry(*static_cast<Storage*>(context), entry);
}

/** @param context Pass storage. @param tag Component tag. @return True when the row fits. */
[[nodiscard]] bool collect_component(void* context, std::uint32_t tag) noexcept {
    return add_component(*static_cast<Storage*>(context), tag);
}

/** @param context Pass storage. @param tag Container tag. @return True when the row fits. */
[[nodiscard]] bool collect_container(void* context, std::uint32_t tag) noexcept {
    return add_container(*static_cast<Storage*>(context), tag);
}

/**
 * Reads a run of tags the pass is about to ask for, on several readers at once.
 * @param context Package source and kept blobs.
 * @param tags Tags the pass will ask for, in order.
 */
void prefetch_blobs(void* context, std::span<const std::uint32_t> tags) noexcept {
    auto& read = *static_cast<ReadContext*>(context);
    read.keptCursor = 0;
    if (!reader::parallel::read_kept(*read.source, tags, *read.kept)) {
        read.kept->clear();
    }
}

/**
 * Reads one spawn-set tag through the production package reader.
 * A blob the read-ahead already holds is served from it. Anything else reads here, so a tag the
 * read-ahead missed or could not read behaves exactly as it always did.
 * @param context Package source, scratch, output bytes and kept blobs.
 * @param tag Spawn-set tag.
 * @param blob Receives bytes live until the next read.
 * @return True when the installed entry decodes.
 */
[[nodiscard]] bool
read_blob(void* context, std::uint32_t tag, std::span<const std::byte>& blob) noexcept {
    auto& read = *static_cast<ReadContext*>(context);
    blob = {};
    // The kept rows are in the order the pass asks for them, so one cursor serves them.
    if (read.keptCursor < read.kept->size() && (*read.kept)[read.keptCursor].tag == tag) {
        blob = (*read.kept)[read.keptCursor].blob;
        ++read.keptCursor;
        return true;
    }
    if (!reader::read_tag(*read.source, *read.scratch, tag, *read.bytes)) {
        return false;
    }
    blob = *read.bytes;
    return true;
}

/**
 * Reports the pass so a boot that shows no spawn set says which step lost the rows.
 * @param storage Pass storage holding every count.
 * @param result Outcome text for the log line.
 */
void report(const Storage& storage, const char* result) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const std::uint64_t elapsed =
        storage.startedTick == 0 ? 0 : GetTickCount64() - storage.startedTick;
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=build_data stage=spawn_sets tags=%zu stems=%zu "
                                      "hashes=%zu bound=%zu components=%zu containers=%zu "
                                      "read=%zu skipped=%zu ms=%llu result=%s",
                                      storage.entryCount,
                                      storage.stemCount,
                                      storage.nameHashCount,
                                      storage.spawnComponentCount,
                                      storage.componentCount,
                                      storage.containerCount,
                                      storage.cursor,
                                      storage.skipped,
                                      static_cast<unsigned long long>(elapsed),
                                      result);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         storage.stemCount != 0 ? core::log::Level::info : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace

/** Extracts and publishes the spawn-set catalogue from the installed packages. */
bool build(const reader::Source& source, reader::Scratch& scratch) noexcept {
    if (state::build_data::spawn_sets_ready()) {
        return true;
    }
    static Storage storage{};
    static std::vector<std::byte> bytes{};
    static std::uint64_t readDeadlineTick{};
    if (!storage.collected) {
        reset(storage);
        storage.startedTick = GetTickCount64();
        reader::ScanResult result{};
        // The sweep reads every installed package header and entry table, so it runs once. A pass
        // that re-swept on each retry cost minutes of boot and read nothing new.
        reader::ScanResult components{};
        reader::ScanResult containers{};
        if (!reader::scan_class_entries(
                source.directory, tables::kSpawnSetClass, &collect, &storage, result)
            || result.matches != storage.entryCount
            || !reader::scan_class(
                source.directory, tables::kComponentClass, &collect_component, &storage, components)
            || !reader::scan_class(
                source.directory, tables::kContainerClass, &collect_container, &storage, containers)
            || !finish_collection(storage)) {
            // A build that installs no spawn set has a whole empty catalogue, not a failed pass.
            report(storage, storage.entryCount == 0 ? "empty" : "sweep");
            reset(storage);
            return state::build_data::publish_spawn_sets({}, {});
        }
        readDeadlineTick = GetTickCount64() + kReadWindowMs;
        report(storage, "collected");
        return false;
    }
    static std::vector<reader::parallel::Held> kept{};
    ReadContext context{&source, &scratch, &bytes, &kept, 0};
    const AdvanceResult result = advance(storage,
                                         &read_blob,
                                         &prefetch_blobs,
                                         &context,
                                         kReadBudget,
                                         GetTickCount64() >= readDeadlineTick);
    if (result == AdvanceResult::invalid) {
        // A retry cannot help, and an empty catalogue still finishes the pipeline.
        report(storage, "read");
        reset(storage);
        return state::build_data::publish_spawn_sets({}, {});
    }
    if (result != AdvanceResult::complete) {
        return false;
    }
    const bool published = state::build_data::publish_spawn_sets(
        std::span(storage.stems).first(storage.stemCount),
        std::span(storage.nameHashes).first(storage.nameHashCount));
    report(storage, published ? "ok" : "publish");
    if (published) {
        bytes.clear();
        bytes.shrink_to_fit();
        kept.clear();
        kept.shrink_to_fit();
    }
    return published;
}

} // namespace sunrise::client::content::spawn_sets
