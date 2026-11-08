#include "hash_name_build.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "../../../core/logging/log.h"
#include "../../../middleware/content/packages/named_tags.h"
#include "../../../middleware/content/packages/reader/parallel.h"
#include "../../../middleware/content/packages/schema/generated/tag_layout_80809c_0.h"
#include "../../../state/build_data/runtime.h"
#include "hash_name_matcher.h"

namespace sunrise::client::content::hash_names {
namespace {

namespace layouts = state::build_data::scenarios;
namespace packages = middleware::content::packages;
namespace reader = middleware::content::packages::reader;

/**
 * Tag class of a named installed object.
 * Its blobs end with the object's own id, and a bubble name is usually one token of it.
 * This class gives all but a handful of the names the packages can produce.
 */
constexpr std::uint32_t kNamedObjectClass = packages::schema::generated::SEntityConfig::kClass;

/** @param context Pass storage. @param tag Swept tag. @return True when the tag fits. */
[[nodiscard]] bool collect_tag(void* context, std::uint32_t tag) noexcept {
    return add_tag(*static_cast<Storage*>(context), tag);
}

/** Rows one reader fills. Only the reader holding that index ever writes them. */
using Rows = std::array<names_state::Name, names_state::kNameCapacity>;

/**
 * One row set per reader for the boot pass, kept across batches so a reader builds up its own
 * best names. Each is a quarter of a megabyte, so they are allocated. Freed on publish.
 */
std::vector<Rows> g_rows{};

/** What the readers need to reach the pass. Every reader reads it and none writes it. */
struct BatchContext {
    Storage* storage{};
    /** One row set per reader index. */
    std::vector<Rows>* rows{};
};

/**
 * Sizes the reader rows for a pass.
 * @param storage Pass storage.
 * @param rows Row sets to size.
 * @param workers Readers wanted.
 * @return True when rows are ready.
 */
[[nodiscard]] bool
prepare_rows(const Storage& storage, std::vector<Rows>& rows, std::size_t workers) noexcept {
    if (storage.targetCount > names_state::kNameCapacity) {
        return false;
    }
    try {
        if (rows.size() < workers) {
            rows.resize(workers);
        }
    } catch (...) {
        return false;
    }
    return rows.size() >= workers;
}

/** Frees the reader rows. */
void release_rows() noexcept {
    g_rows.clear();
    g_rows.shrink_to_fit();
}

/**
 * Matches one blob on the reader thread that read it.
 * It touches the pass's read-only targets and the rows of its own reader index. No two readers
 * meet, so nothing here locks.
 * @param worker Reader index.
 * @param blob Whole object bytes.
 */
void collect_blob(void* context,
                  std::size_t worker,
                  std::uint32_t,
                  std::span<const std::byte> blob) noexcept {
    const auto& batch = *static_cast<BatchContext*>(context);
    const Storage& storage = *batch.storage;
    if (worker >= batch.rows->size()) {
        return;
    }
    MatchState state{{storage.targets.data(), storage.targetCount},
                     {(*batch.rows)[worker].data(), storage.targetCount}};
    offer_blob(state, blob);
}

/**
 * Offers one package directory name, which costs no blob read.
 * @param context Pass storage.
 * @param entry Named definition.
 * @return Always true, so one unusable name cannot stop the directory pass.
 */
[[nodiscard]] bool collect_named(void* context, const packages::named_tags::Entry& entry) noexcept {
    auto& storage = *static_cast<Storage*>(context);
    MatchState state = match_state(storage);
    const std::string_view name(entry.name.data(), entry.nameLength);
    std::size_t start = 0;
    for (std::size_t index = 0; index <= name.size(); ++index) {
        // A definition name is a path of ids, so each piece is offered on its own.
        if (index != name.size() && state::build_data::hash_names::name_character(name[index])) {
            continue;
        }
        if (index > start) {
            offer(state, name.substr(start, index - start));
        }
        start = index + 1;
    }
    return true;
}

/**
 * Reports the pass so a boot with no bubble names says which step lost them.
 * @param storage Pass storage holding every count.
 * @param result Outcome text for the log line.
 */
void report(const Storage& storage, const char* result) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const std::uint64_t elapsed =
        storage.startedTick == 0 ? 0 : GetTickCount64() - storage.startedTick;
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=build_data stage=hash_names hashes=%zu objects=%zu "
                                      "read=%zu named=%zu ms=%llu result=%s",
                                      storage.targetCount,
                                      storage.tagCount,
                                      storage.cursor,
                                      storage.resolvedCount,
                                      static_cast<unsigned long long>(elapsed),
                                      result);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         storage.resolvedCount != 0 ? core::log::Level::info
                                                    : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Publishes whatever the pass has found.
 * @param storage Pass storage.
 * @param result Outcome text for the log line.
 * @return True when the table published.
 */
[[nodiscard]] bool publish(Storage& storage, const char* result) noexcept {
    finish(storage);
    const bool published = state::build_data::publish_hash_names(
        std::span(storage.resolved).first(storage.resolvedCount));
    report(storage, published ? result : "publish");
    return published;
}

} // namespace

/** Finds the internal name of every destination bubble it can, once. */
bool build(const reader::Source& source, reader::Scratch&) noexcept {
    if (state::build_data::hash_names_ready()) {
        return true;
    }
    // The destination layouts and the spawn-set catalogue carry the hashes to look for, so there
    // is nothing to do before both of them.
    if (!state::build_data::scenario_layouts_ready() || !state::build_data::spawn_sets_ready()) {
        return false;
    }
    static Storage storage{};
    if (!storage.collected) {
        reset(storage);
        storage.startedTick = GetTickCount64();
        // One whole domain of fixed rows, so the snapshot is static rather than a local.
        static std::array<layouts::Definition, layouts::kDefinitionCapacity> scratch{};
        std::size_t rows = 0;
        if (!state::build_data::snapshot_scenario_layouts(scratch, rows)) {
            report(storage, "targets");
            reset(storage);
            return false;
        }
        // Nothing to name still publishes an empty table; the SDK generator waits for one.
        if (!collect_targets(storage, std::span(scratch).first(rows))) {
            return publish(storage, "no_targets");
        }
        // The package directory names cost no blob read, so they are taken first and a pass that
        // cannot sweep still publishes them.
        packages::named_tags::DirectoryResult directory{};
        (void)packages::named_tags::extract_directory(
            source.directory, source.installedOnly, &collect_named, &storage, directory);
        reader::ScanResult scan{};
        if (!reader::scan_class(source, kNamedObjectClass, &collect_tag, &storage, scan)
            || scan.matches != storage.tagCount) {
            return publish(storage, "sweep");
        }
        storage.collected = true;
        report(storage, "collected");
        return false;
    }

    // The readers are what make this affordable: a block waits out the disk before it decodes,
    // and one reader per share overlaps those waits. The call returns once all of them have
    // joined, so nothing outlives the batch.
    const std::size_t workers = reader::parallel::worker_count();
    if (!prepare_rows(storage, g_rows, workers)) {
        return publish(storage, "readers");
    }
    // A boot slice never waits for the reader; the next slice tries again.
    const reader::parallel::Lease lease{reader::parallel::kTryAcquire};
    if (!lease.held()) {
        return false;
    }
    const std::size_t batch = (std::min)(kBatchTags, storage.tagCount - storage.cursor);
    BatchContext context{&storage, &g_rows};
    if (!reader::parallel::read_tags(
            source, {storage.tags.data() + storage.cursor, batch}, &collect_blob, &context)) {
        return publish(storage, "readers");
    }
    for (std::size_t worker = 0; worker < workers; ++worker) {
        merge(storage, {g_rows[worker].data(), storage.targetCount});
    }
    storage.cursor += batch;
    if (storage.cursor < storage.tagCount) {
        return false;
    }
    const bool published = publish(storage, "ok");
    if (published) {
        release_rows();
        reader::parallel::release();
    }
    return published;
}

/** Finds the names of the given hashes from installed packages only, in one call. */
bool build_private(const reader::Source& source,
                   std::span<const layouts::Definition> definitions,
                   std::vector<names_state::Name>& output) noexcept {
    output.clear();
    const reader::Source installed{source.directory, source.keys, true};
    try {
        // Over a megabyte, so it lives on the heap.
        const auto storage = std::make_unique<Storage>();
        if (!collect_targets(*storage, definitions)) {
            return true;
        }
        packages::named_tags::DirectoryResult directory{};
        (void)packages::named_tags::extract_directory(
            installed.directory, installed.installedOnly, &collect_named, storage.get(), directory);
        reader::ScanResult scan{};
        if (!reader::scan_class(installed, kNamedObjectClass, &collect_tag, storage.get(), scan)
            || scan.matches != storage->tagCount) {
            return false;
        }
        std::vector<Rows> rows{};
        if (!prepare_rows(*storage, rows, reader::parallel::worker_count())) {
            return false;
        }
        BatchContext context{storage.get(), &rows};
        bool read = false;
        {
            const reader::parallel::ScopedRead lease{};
            read = reader::parallel::read_tags(
                installed, {storage->tags.data(), storage->tagCount}, &collect_blob, &context);
        }
        if (!read) {
            return false;
        }
        for (const Rows& worker : rows) {
            merge(*storage, {worker.data(), storage->targetCount});
        }
        finish(*storage);
        output.assign(storage->resolved.begin(),
                      storage->resolved.begin()
                          + static_cast<std::ptrdiff_t>(storage->resolvedCount));
        return true;
    } catch (...) {
        output.clear();
        return false;
    }
}

} // namespace sunrise::client::content::hash_names
