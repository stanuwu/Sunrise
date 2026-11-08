#include "studio_reload_patch.h"

#include <Windows.h>

#include <algorithm>
#include <cstring>
#include <utility>

#include "../../client/content/handles/handle_resolver.h"
#include "../../client/memory/current_process_memory.h"
#include "../../client/targets/game/content.h"
#include "../../state/mods/packages/mod_load_patch.h"

namespace sunrise::server::studio::reload_patch {
namespace {

namespace manifest = reload_manifest;
namespace handles = client::content::handles;

using state::mods::load_patch::crc32;

// A deferred tag-heap move takes effect on a later resourcerer update, a few frames on.
constexpr DWORD kSettleMilliseconds = 100;
// Checks before a range that keeps losing its bytes counts as lost.
constexpr int kSettleRounds = 5;
// PatchRange bytes are padded to this.
constexpr std::size_t kPatchAlign = 4;

/** @return The handle tables, or an empty source before the content targets resolve. */
[[nodiscard]] handles::Source tables() noexcept {
    namespace content = client::targets::game::content;
    handles::Source source{};
    if (content::is_resolved()) {
        source.tablesSlot =
            reinterpret_cast<std::uintptr_t>(content::get().contentHandleTablesSlot);
        source.read = &client::memory::copy_current_process;
    }
    return source;
}

/** One range of an entry, its bytes still inside the patch blob. */
struct Range {
    std::uint32_t offset{};
    std::uint32_t oldCrc32{};
    std::span<const std::byte> bytes;
};

/**
 * Reads an entry's ranges out of the patch blob.
 * @return False when a range header or its bytes run past the blob.
 */
[[nodiscard]] bool
parse(const manifest::Entry& entry, std::span<const std::byte> blob, std::vector<Range>& output) {
    output.clear();
    std::size_t at = entry.firstPatch;
    for (std::uint16_t index = 0; index < entry.patchRanges; ++index) {
        manifest::PatchRange head{};
        if (at > blob.size() || blob.size() - at < sizeof head) {
            return false;
        }
        std::memcpy(&head, blob.data() + at, sizeof head);
        at += sizeof head;
        if (head.length > blob.size() - at) {
            return false;
        }
        output.push_back({head.offset, head.oldCrc32, blob.subspan(at, head.length)});
        at += (head.length + kPatchAlign - 1) / kPatchAlign * kPatchAlign;
    }
    return true;
}

/** @return True when `length` bytes from `offset` stay below `limit`. */
[[nodiscard]] bool inside(std::uint32_t offset, std::size_t length, std::uint32_t limit) noexcept {
    return offset <= limit && length <= limit - offset;
}

/** @return False when the read faults; otherwise `crc` holds the crc32 of the bytes. */
[[nodiscard]] bool
crc_at(std::uintptr_t address, std::uint32_t offset, std::size_t length, std::uint32_t& crc) {
    std::vector<std::byte> bytes(length);
    if (!client::memory::copy_current_process(nullptr, address + offset, bytes)) {
        return false;
    }
    crc = crc32(bytes);
    return true;
}

[[nodiscard]] Answer refused(manifest::Detail detail) noexcept {
    return {manifest::Outcome::refused, static_cast<std::uint32_t>(detail)};
}

} // namespace

/** All guards pass before the first write, so a refused entry leaves the tag as it was. */
Answer apply(const manifest::Entry& entry,
             std::span<const std::byte> patchBlob,
             std::vector<Written>& written) noexcept {
    try {
        std::vector<Range> ranges;
        if (!parse(entry, patchBlob, ranges) || ranges.empty()) {
            return refused(manifest::Detail::patchUnsupported);
        }
        handles::Resident tag{};
        if (!handles::resident(tables(), entry.tag, tag)) {
            return {manifest::Outcome::notResident};
        }
        if (tag.classId != entry.classId) {
            return refused(manifest::Detail::classMismatch);
        }
        const std::uint32_t limit = (std::min)(tag.size, entry.newSize);
        for (const Range& range : ranges) {
            std::uint32_t crc = 0;
            if (!inside(range.offset, range.bytes.size(), limit)) {
                return refused(manifest::Detail::patchUnsupported);
            }
            if (!crc_at(tag.address, range.offset, range.bytes.size(), crc)) {
                return {manifest::Outcome::notResident};
            }
            if (crc != range.oldCrc32) {
                return {manifest::Outcome::guardMismatch};
            }
        }
        for (const Range& range : ranges) {
            if (!client::memory::write_current_process(tag.address + range.offset, range.bytes)) {
                return refused(manifest::Detail::writeFailed);
            }
            written.push_back({entry.tag,
                               range.offset,
                               range.oldCrc32,
                               {range.bytes.begin(), range.bytes.end()}});
        }
        return {manifest::Outcome::applied};
    } catch (...) {
        return refused(manifest::Detail::writeFailed);
    }
}

/** A tag no longer loaded is not patched; its next load reads the patch the client knew at boot. */
std::vector<std::uint32_t> settle(std::span<const Written> written) noexcept {
    try {
        const handles::Source source = tables();
        std::vector<std::uint32_t> wanted;
        wanted.reserve(written.size());
        for (const Written& range : written) {
            wanted.push_back(crc32(range.bytes));
        }
        std::vector<std::uint32_t> pending;
        for (int round = 0; round < kSettleRounds && !written.empty(); ++round) {
            Sleep(kSettleMilliseconds);
            pending.clear();
            for (std::size_t index = 0; index < written.size(); ++index) {
                const Written& range = written[index];
                handles::Resident tag{};
                std::uint32_t crc = 0;
                if (!handles::resident(source, range.tag, tag)
                    || !inside(range.offset, range.bytes.size(), tag.size)
                    || !crc_at(tag.address, range.offset, range.bytes.size(), crc)
                    || crc == wanted[index]) {
                    continue;
                }
                pending.push_back(range.tag);
                // A move copied the block before the write landed; the new copy has the old bytes.
                if (crc == range.oldCrc32) {
                    static_cast<void>(client::memory::write_current_process(
                        tag.address + range.offset, range.bytes));
                }
            }
            if (pending.empty()) {
                break;
            }
        }
        return pending;
    } catch (...) {
        return {};
    }
}

/** The crc covers the bytes the entry's size names, so Studio can compare it with its package. */
Answer query(const manifest::Entry& entry) noexcept {
    try {
        handles::Resident tag{};
        if (!handles::resident(tables(), entry.tag, tag)) {
            return {manifest::Outcome::notResident};
        }
        if (entry.classId != 0 && tag.classId != entry.classId) {
            return refused(manifest::Detail::classMismatch);
        }
        const std::uint32_t size =
            entry.newSize != 0 ? (std::min)(tag.size, entry.newSize) : tag.size;
        std::uint32_t crc = 0;
        if (!crc_at(tag.address, 0, size, crc)) {
            return {manifest::Outcome::notResident};
        }
        return {manifest::Outcome::resident, crc};
    } catch (...) {
        return {manifest::Outcome::notResident};
    }
}

/** The kept ranges copy their bytes, so the manifest mapping can close. */
bool retain(const manifest::Entry& entry, std::span<const std::byte> patchBlob) noexcept {
    try {
        std::vector<Range> ranges;
        if (!parse(entry, patchBlob, ranges) || ranges.empty()) {
            return false;
        }
        std::vector<state::mods::load_patch::Range> kept;
        kept.reserve(ranges.size());
        for (const Range& range : ranges) {
            if (!inside(range.offset, range.bytes.size(), entry.newSize)) {
                return false;
            }
            kept.push_back(
                {range.offset, range.oldCrc32, {range.bytes.begin(), range.bytes.end()}});
        }
        return state::mods::load_patch::retain(
            entry.tag, entry.classId, entry.newSize, std::move(kept));
    } catch (...) {
        return false;
    }
}

} // namespace sunrise::server::studio::reload_patch
