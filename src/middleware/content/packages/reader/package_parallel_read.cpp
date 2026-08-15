#include <Windows.h>

#include <algorithm>
#include <array>
#include <vector>

#include "parallel.h"

namespace sunrise::middleware::content::packages::reader::parallel {
namespace {

/** Cores left to the game, which is drawing frames while the batch reads. */
constexpr std::size_t kReservedCores = 2;

/** One reader's share of a batch. Nothing here is shared with another reader. */
struct Share {
    const Source* source{};
    std::span<const std::uint32_t> tags;
    Visitor visitor{};
    void* context{};
    std::size_t worker{};
    Scratch* scratch{};
    std::vector<std::byte>* bytes{};
};

/**
 * Reader storage, sized once and kept until the pass releases it.
 * These are megabytes each, so they are allocated rather than declared. Only the reader that
 * owns the index touches one.
 */
std::vector<Scratch> g_scratch{};
std::vector<std::vector<std::byte>> g_bytes{};

/** Where one reader's kept blobs sit inside its own store. */
struct KeptRow {
    std::uint32_t tag{};
    std::size_t offset{};
    std::size_t size{};
};

/** One reader's kept blobs, written only by that reader while a batch runs. */
struct Keep {
    std::vector<std::byte> store;
    std::vector<KeptRow> rows;
};

std::vector<Keep> g_keep{};

/** @param share One reader's share. Reads every tag of it and reports each blob. */
void run_share(Share& share) noexcept {
    for (const std::uint32_t tag : share.tags) {
        if (read_tag(*share.source, *share.scratch, tag, *share.bytes)) {
            share.visitor(share.context, share.worker, tag, *share.bytes);
        }
    }
}

/** @param parameter One reader's share. @return Always zero; it reports through its visitor. */
DWORD WINAPI thread_main(LPVOID parameter) noexcept {
    run_share(*static_cast<Share*>(parameter));
    return 0;
}

/**
 * Keeps one blob for the caller to parse after the batch.
 * It runs on the reader's thread and touches only that reader's store, so nothing locks.
 * @param worker Reader index.
 * @param tag Tag that read.
 * @param blob Entry bytes.
 */
void keep_blob(void*,
               std::size_t worker,
               std::uint32_t tag,
               std::span<const std::byte> blob) noexcept {
    if (worker >= g_keep.size()) {
        return;
    }
    Keep& keep = g_keep[worker];
    const std::size_t offset = keep.store.size();
    keep.store.insert(keep.store.end(), blob.begin(), blob.end());
    keep.rows.push_back(KeptRow{tag, offset, blob.size()});
}

/** @param workers Readers wanted. @return True when storage for that many is ready. */
[[nodiscard]] bool prepare(std::size_t workers) noexcept {
    if (g_scratch.size() >= workers) {
        return true;
    }
    // Growing moves the scratches, so it only ever happens before any file is open.
    g_scratch.resize(workers);
    g_bytes.resize(workers);
    return g_scratch.size() >= workers && g_bytes.size() >= workers;
}

} // namespace

/** @return Reader count, at least one. */
std::size_t worker_count() noexcept {
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const auto cores = static_cast<std::size_t>(info.dwNumberOfProcessors);
    const std::size_t spare = cores > kReservedCores ? cores - kReservedCores : 1;
    return (std::min)(spare, kMaxWorkers);
}

/** Reads a run of tags across several readers and returns when every one has finished. */
bool read_tags(const Source& source,
               std::span<const std::uint32_t> tags,
               Visitor visitor,
               void* context) noexcept {
    if (visitor == nullptr) {
        return false;
    }
    if (tags.empty()) {
        return true;
    }
    const std::size_t workers = (std::min)(worker_count(), tags.size());
    if (!prepare(workers)) {
        return false;
    }

    // Each reader takes one run of tags, not every nth. Tags next to each other share a block,
    // so a reader that keeps hitting its own block cache reads far less.
    const std::size_t share = (tags.size() + workers - 1U) / workers;
    std::array<Share, kMaxWorkers> shares{};
    std::array<HANDLE, kMaxWorkers> threads{};
    std::size_t spawned = 0;
    for (std::size_t worker = 0; worker < workers; ++worker) {
        const std::size_t begin = (std::min)(worker * share, tags.size());
        const std::size_t end = (std::min)(begin + share, tags.size());
        shares[worker] = Share{&source,
                               tags.subspan(begin, end - begin),
                               visitor,
                               context,
                               worker,
                               &g_scratch[worker],
                               &g_bytes[worker]};
    }
    // The calling thread reads the first share, so one fewer thread is spawned for the same work.
    std::array<bool, kMaxWorkers> started{};
    for (std::size_t worker = 1; worker < workers; ++worker) {
        const HANDLE thread = CreateThread(nullptr, 0, &thread_main, &shares[worker], 0, nullptr);
        if (thread == nullptr) {
            continue;
        }
        threads[spawned++] = thread;
        started[worker] = true;
    }
    run_share(shares[0]);
    // A reader that would not start is read here instead, on this thread and with this thread's
    // storage, so the batch is never left partly read. A share that did start is not touched
    // again until its thread has joined.
    for (std::size_t worker = 1; worker < workers; ++worker) {
        if (!started[worker]) {
            shares[worker].worker = 0;
            shares[worker].scratch = &g_scratch[0];
            shares[worker].bytes = &g_bytes[0];
            run_share(shares[worker]);
        }
    }
    if (spawned != 0) {
        (void)WaitForMultipleObjects(static_cast<DWORD>(spawned), threads.data(), TRUE, INFINITE);
        for (std::size_t index = 0; index < spawned; ++index) {
            (void)CloseHandle(threads[index]);
        }
    }
    return true;
}

/** Reads a run of tags across several readers and keeps every blob that read. */
bool read_kept(const Source& source,
               std::span<const std::uint32_t> tags,
               std::vector<Held>& kept) noexcept {
    kept.clear();
    const std::size_t workers = worker_count();
    if (g_keep.size() < workers) {
        g_keep.resize(workers);
    }
    if (g_keep.size() < workers) {
        return false;
    }
    for (Keep& keep : g_keep) {
        keep.store.clear();
        keep.rows.clear();
    }
    if (!read_tags(source, tags, &keep_blob, nullptr)) {
        return false;
    }
    // Each reader took one run of tags in order, so reading the readers in order returns the
    // blobs in the order the caller asked for them.
    for (const Keep& keep : g_keep) {
        for (const KeptRow& row : keep.rows) {
            kept.push_back(Held{row.tag, {keep.store.data() + row.offset, row.size}});
        }
    }
    return true;
}

/** Closes every reader's files and frees the batch storage. */
void release() noexcept {
    for (Scratch& scratch : g_scratch) {
        close_files(scratch);
    }
    g_scratch.clear();
    g_scratch.shrink_to_fit();
    g_bytes.clear();
    g_bytes.shrink_to_fit();
    g_keep.clear();
    g_keep.shrink_to_fit();
}

} // namespace sunrise::middleware::content::packages::reader::parallel
