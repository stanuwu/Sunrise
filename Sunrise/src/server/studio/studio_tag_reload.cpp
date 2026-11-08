#include "studio_tag_reload.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "../../client/hooks/bootflow/bootflow_hook_lifecycle.h"
#include "../../core/logging/log.h"
#include "../../middleware/content/packages/reader/overlay.h"
#include "../../middleware/content/packages/tables/scenario_reader.h"
#include "../../state/activity/forced/activity_forced_destination.h"
#include "../../state/build_data/runtime/build_data_invalidation.h"
#include "../../state/mods/mods_runtime.h"
#include "../bap/runtime.h"
#include "studio_reload_manifest.h"
#include "studio_reload_patch.h"

namespace sunrise::server::studio::tag_reload {
namespace {

namespace wire = command_wire;
namespace manifest = reload_manifest;
namespace invalidation = state::build_data::invalidation;
namespace patch = reload_patch;

// A reload refusal is copied whole into the reply message.
static_assert(state::mods::kReloadMessageCapacity <= wire::kMessageBytes);

/** One worker at a time; its reply waits here until the server thread takes it. */
struct Worker final {
    std::mutex lock;
    HANDLE thread{};
    std::uint32_t editorPid{};
    std::uint32_t request{};
    wire::Kind kind{};
    std::optional<Reply> reply;
    bool running{};
};

Worker g_worker;

void set_message(Reply& reply, const char* format, auto... values) noexcept {
    std::snprintf(reply.message.data(), reply.message.size(), format, values...);
}

/** Maps the editor's manifest for this request. */
struct Mapping final {
    HANDLE handle{};
    std::byte* view{};
    std::size_t bytes{};

    /** Opens the editor's manifest mapping read-write; `view` stays null when it is missing. */
    Mapping(std::uint32_t editorPid, std::uint32_t request) noexcept {
        std::array<wchar_t, 96> name{};
        if (swprintf_s(name.data(),
                       name.size(),
                       manifest::kMappingFormat,
                       static_cast<unsigned long>(editorPid),
                       static_cast<unsigned long>(request))
            <= 0) {
            return;
        }
        handle = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, name.data());
        if (handle == nullptr) {
            return;
        }
        view =
            static_cast<std::byte*>(MapViewOfFile(handle, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0));
        MEMORY_BASIC_INFORMATION info{};
        if (view != nullptr && VirtualQuery(view, &info, sizeof(info)) == sizeof(info)) {
            bytes = (std::min)(info.RegionSize, manifest::kMaximumBytes);
        }
    }

    ~Mapping() {
        if (view != nullptr) {
            UnmapViewOfFile(view);
        }
        if (handle != nullptr) {
            CloseHandle(handle);
        }
    }

    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
};

/** @return True when every offset and count of the header lies inside the mapping. */
[[nodiscard]] bool header_fits(const manifest::ManifestHeader& header,
                               std::uint32_t request,
                               std::size_t bytes) noexcept {
    const std::uint64_t entries = std::uint64_t{header.entryCount} * sizeof(manifest::Entry);
    const std::uint64_t results = std::uint64_t{header.entryCount} * sizeof(manifest::Result);
    return header.magic == manifest::kMagic && header.version == manifest::kVersion
           && header.request == request && header.entryCount <= manifest::kMaximumEntries
           && header.entriesOffset >= sizeof(header) && header.entriesOffset <= bytes
           && entries <= bytes - header.entriesOffset && header.patchOffset <= bytes
           && header.patchBytes <= bytes - header.patchOffset && header.resultsOffset <= bytes
           && results <= bytes - header.resultsOffset
           && header.entriesOffset % alignof(manifest::Entry) == 0
           && header.resultsOffset % alignof(manifest::Result) == 0;
}

/** A tag is served when the reader holds a generation of its package. */
[[nodiscard]] bool served(const manifest::Entry& entry) noexcept {
    std::uint32_t patch = 0;
    return entry.packageId <= 0xFFFF
           && middleware::content::packages::reader::overlay::latest(
               static_cast<std::uint16_t>(entry.packageId), patch);
}

/** @return The byte a manifest Result carries for one tier. */
[[nodiscard]] constexpr std::uint8_t tier_byte(invalidation::LiveTier tier) noexcept {
    return static_cast<std::uint8_t>(tier);
}

/** Outcomes of one request, for its message and log line. */
struct Counts final {
    std::uint32_t applied{};
    std::uint32_t spawn{};
    std::uint32_t reload{};
    std::uint32_t restart{};
    std::uint32_t other{};
};

/** @return The counter one result belongs to. */
[[nodiscard]] std::uint32_t& counter(const manifest::Result& result, Counts& counts) noexcept {
    switch (result.outcome) {
    case manifest::Outcome::applied:
        return counts.applied;
    case manifest::Outcome::nextSpawn:
        return counts.spawn;
    case manifest::Outcome::needsBubbleReload:
        return counts.reload;
    case manifest::Outcome::needsRestart:
        return counts.restart;
    default:
        return counts.other;
    }
}

void count(const manifest::Result& result, Counts& counts) noexcept {
    ++counter(result, counts);
}

void uncount(const manifest::Result& result, Counts& counts) noexcept {
    --counter(result, counts);
}

/** @return True for a result whose bytes went into the client's copy. */
[[nodiscard]] bool patched(const manifest::Result& result) noexcept {
    return (result.tier == tier_byte(invalidation::LiveTier::patch)
            || result.tier == tier_byte(invalidation::LiveTier::spawn))
           && (result.outcome == manifest::Outcome::applied
               || result.outcome == manifest::Outcome::nextSpawn);
}

/**
 * Answers one entry: Sunrise's own data applies at once, and a patch is kept for the client's next
 * load of the tag. A poke-safe patch also goes into the copy the client holds now. The client
 * loads any other change only after a restart.
 * @param domains Gains the build-data domains the entry feeds.
 */
[[nodiscard]] manifest::Result answer(const manifest::Entry& entry,
                                      wire::Kind kind,
                                      std::span<const std::byte> blob,
                                      std::vector<patch::Written>& written,
                                      std::uint32_t& domains) noexcept {
    if (!served(entry)) {
        return {.outcome = manifest::Outcome::refused,
                .detail = static_cast<std::uint32_t>(manifest::Detail::tagMissing)};
    }
    domains |= invalidation::domains_for(entry.classId);
    if (invalidation::live_tier(entry.classId) == invalidation::LiveTier::data) {
        return {.outcome = manifest::Outcome::applied,
                .tier = tier_byte(invalidation::LiveTier::data)};
    }
    const bool patchable = kind == wire::Kind::patchTags && entry.patchRanges != 0
                           && (entry.flags & manifest::kLayoutPreserving) != 0;
    if (!patchable) {
        return {.outcome = manifest::Outcome::needsRestart,
                .tier = tier_byte(invalidation::LiveTier::restart)};
    }
    if (!patch::retain(entry, blob)) {
        return {.outcome = manifest::Outcome::refused,
                .tier = tier_byte(invalidation::LiveTier::restart),
                .detail = static_cast<std::uint32_t>(manifest::Detail::patchUnsupported)};
    }
    const manifest::Result reload{.outcome = manifest::Outcome::needsBubbleReload,
                                  .tier = tier_byte(invalidation::LiveTier::restart)};
    if ((entry.flags & manifest::kPokeSafe) == 0) {
        return reload;
    }
    const bool spawn = (entry.flags & manifest::kSpawnTime) != 0;
    const patch::Answer result = patch::apply(entry, blob, written);
    if (result.outcome == manifest::Outcome::applied) {
        return {.outcome = spawn ? manifest::Outcome::nextSpawn : manifest::Outcome::applied,
                .tier = spawn ? tier_byte(invalidation::LiveTier::spawn)
                              : tier_byte(invalidation::LiveTier::patch)};
    }
    // A tag the client does not hold gets the kept patch when it next loads.
    if (result.outcome == manifest::Outcome::notResident) {
        return reload;
    }
    return {.outcome = result.outcome,
            .tier = tier_byte(invalidation::LiveTier::patch),
            .detail = result.detail};
}

/** Answers one tag request; a failed restage refuses every tag and keeps the old tables. */
Reply serve(std::uint32_t editorPid, std::uint32_t request, wire::Kind kind) noexcept {
    Reply reply{.status = wire::Status::failed};
    const Mapping mapping(editorPid, request);
    if (mapping.view == nullptr || mapping.bytes < sizeof(manifest::ManifestHeader)) {
        set_message(reply, "The tag list of request %u could not be opened.", request);
        return reply;
    }
    manifest::ManifestHeader header{};
    std::memcpy(&header, mapping.view, sizeof(header));
    if (!header_fits(header, request, mapping.bytes)) {
        reply.status = wire::Status::invalid;
        set_message(
            reply, "The tag list does not match request %u or runs past its mapping.", request);
        return reply;
    }
    const std::span entries(
        reinterpret_cast<const manifest::Entry*>(mapping.view + header.entriesOffset),
        header.entryCount);
    const std::span results(
        reinterpret_cast<manifest::Result*>(mapping.view + header.resultsOffset),
        header.entryCount);
    if (kind == wire::Kind::queryResident) {
        std::uint32_t found = 0;
        for (std::size_t index = 0; index < entries.size(); ++index) {
            const patch::Answer answer = patch::query(entries[index]);
            results[index] = {.outcome = answer.outcome, .detail = answer.detail};
            found += answer.outcome == manifest::Outcome::resident ? 1U : 0U;
        }
        reply.status = wire::Status::ok;
        set_message(reply, "%u of %u tags are resident.", found, header.entryCount);
        return reply;
    }
    const auto started = std::chrono::steady_clock::now();
    const state::mods::ReloadResult reloaded = state::mods::reload_packages();
    if (!reloaded.ok) {
        std::fill(
            results.begin(),
            results.end(),
            manifest::Result{.outcome = manifest::Outcome::refused,
                             .detail = static_cast<std::uint32_t>(manifest::Detail::stageFailed)});
        set_message(reply, "%s", reloaded.message.data());
        return reply;
    }
    const std::span blob(reinterpret_cast<const std::byte*>(mapping.view + header.patchOffset),
                         header.patchBytes);
    std::vector<patch::Written> written;
    Counts counts{};
    for (std::size_t index = 0; index < entries.size(); ++index) {
        results[index] = answer(entries[index], kind, blob, written, reply.domains);
        count(results[index], counts);
    }
    for (const std::uint32_t tag : patch::settle(written)) {
        for (std::size_t index = 0; index < entries.size(); ++index) {
            if (entries[index].tag == tag && patched(results[index])) {
                uncount(results[index], counts);
                results[index] = {.outcome = manifest::Outcome::refused,
                                  .tier = results[index].tier,
                                  .detail =
                                      static_cast<std::uint32_t>(manifest::Detail::writeFailed)};
                count(results[index], counts);
            }
        }
    }
    reply.applied = counts.applied;
    reply.status = wire::Status::ok;
    set_message(reply,
                "%u applied, %u next spawn, %u need Reload bubble, %u need a restart, %u not "
                "applied.",
                counts.applied,
                counts.spawn,
                counts.reload,
                counts.restart,
                counts.other);
    core::log::writef(
        core::log::Channel::server,
        core::log::Level::info,
        "ev=studio stage=reload_tags request=%u kind=%u tags=%u applied=%u spawn=%u reload=%u "
        "restart=%u other=%u changed=%d ms=%lld",
        request,
        static_cast<unsigned>(kind),
        header.entryCount,
        counts.applied,
        counts.spawn,
        counts.reload,
        counts.restart,
        counts.other,
        reloaded.changed ? 1 : 0,
        static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now() - started)
                                   .count()));
    return reply;
}

/** The worker reads its request from `g_worker`, which `start` fills before the thread exists. */
DWORD WINAPI run([[maybe_unused]] void* context) {
    const Reply reply = serve(g_worker.editorPid, g_worker.request, g_worker.kind);
    std::lock_guard done(g_worker.lock);
    g_worker.reply = reply;
    g_worker.running = false;
    return 0;
}

/** Waits for the worker thread and releases its handle. */
void join(HANDLE thread) noexcept {
    if (thread != nullptr) {
        WaitForSingleObject(thread, INFINITE);
        CloseHandle(thread);
    }
}

} // namespace

/** Joins a finished worker before starting the next; one worker runs at a time. */
bool start(std::uint32_t editorPid, std::uint32_t request, wire::Kind kind) noexcept {
    std::lock_guard guard(g_worker.lock);
    if (g_worker.running) {
        return false;
    }
    join(std::exchange(g_worker.thread, nullptr));
    g_worker.editorPid = editorPid;
    g_worker.request = request;
    g_worker.kind = kind;
    g_worker.reply.reset();
    g_worker.running = true;
    g_worker.thread = CreateThread(nullptr, 0, &run, nullptr, 0, nullptr);
    if (g_worker.thread == nullptr) {
        g_worker.running = false;
        return false;
    }
    return true;
}

/** Hands over the reply once; a reply for another request is dropped. */
bool take(std::uint32_t request, Reply& reply) noexcept {
    std::lock_guard guard(g_worker.lock);
    if (!g_worker.reply.has_value()) {
        return false;
    }
    const bool match = g_worker.request == request;
    if (match) {
        reply = *g_worker.reply;
    }
    g_worker.reply.reset();
    return match;
}

void stop() noexcept {
    HANDLE thread = nullptr;
    {
        std::lock_guard guard(g_worker.lock);
        thread = std::exchange(g_worker.thread, nullptr);
    }
    join(thread);
}

/** Answers failed when the player is in no world or the destination cannot be stored. */
Reply reload_bubble(std::string_view bubble) noexcept {
    Reply reply{.status = wire::Status::invalid};
    // TODO: resolve a named bubble to its slice set; it needs a bubble-name lookup in the catalog.
    if (!bubble.empty()) {
        set_message(
            reply,
            "Naming a bubble is not supported yet; send an empty message for the current one.");
        return reply;
    }
    const client::hooks::bootflow::CurrentSliceSet slice =
        client::hooks::bootflow::current_slice_set();
    bap::CurrentActivityLinkView view{};
    if (!slice.available || !slice.present || slice.index < 0
        || !bap::current_activity_link_view(slice.index, view)) {
        reply.status = wire::Status::failed;
        set_message(reply, "The player is in no world, so there is no bubble to reload.");
        return reply;
    }
    const auto& destination = view.binding.destination;
    state::activity::forced::ForcedDestination forced{};
    forced.packageNameLength = destination.packageNameLength;
    std::transform(destination.packageName.begin(),
                   destination.packageName.end(),
                   forced.packageName.begin(),
                   [](std::int8_t value) { return static_cast<char>(value); });
    forced.bubble = static_cast<std::uint8_t>(
        slice.index / middleware::content::packages::tables::kSliceSetIndexFactor);
    forced.sliceSet = static_cast<std::uint16_t>(slice.index);
    forced.hasBubble = true;
    forced.hasSliceSet = true;
    if (destination.activityIndex >= 0) {
        forced.activityIndex = static_cast<std::uint16_t>(destination.activityIndex);
        forced.hasActivityIndex = true;
    }
    forced.enabled = true;
    if (!state::activity::forced::publish(forced)) {
        reply.status = wire::Status::failed;
        set_message(reply,
                    "The current destination could not be stored as the forced destination.");
        return reply;
    }
    reply.status = wire::Status::ok;
    set_message(reply,
                "The next launch lands in bubble %u, slice set %d. Leave the world through orbit "
                "or Change Character, then launch; clear the override in Sunrise afterwards.",
                static_cast<unsigned>(forced.bubble),
                slice.index);
    return reply;
}

} // namespace sunrise::server::studio::tag_reload
