#include <Windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

#include "../../../core/filesystem/temporary_sibling.h"
#include "../../../core/json/reader.h"
#include "../../../core/logging/log.h"
#include "../../../middleware/crypto/sha256.h"
#include "../problem/mod_problem.h"
#include "internal.h"

namespace sunrise::state::mods::packages::internal {
namespace {

namespace remap = merge::remap;
namespace name = content_manifest::scanner::package;
namespace sha256 = middleware::crypto::sha256;

// refmap.bin: magic "SRMP", version, then counts; rows are 16 bytes.
constexpr std::uint32_t kMapMagic = 0x504D5253U;
constexpr std::uint16_t kMapVersion = 1;
constexpr std::size_t kMapHeaderSize = 16, kRowSize = 16;
// A row is package, entry and offset, then the kind byte padded to a word.
static_assert(kRowSize == 4 * sizeof(std::uint32_t));
// Studio writes the map beside mod.json.
constexpr std::wstring_view kMapLeaf = L"refmap.bin";
// A map larger than this is refused; one row per reference keeps real maps far smaller.
constexpr std::size_t kMapByteLimit = std::size_t{64} << 20;
// Row kinds; a wide reference is never rewritten, since its key does not name a package.
constexpr std::uint8_t kTagRow = 1, kWideRow = 2, kSidecarRow = 3, kLuaRow = 4, kPackageRow = 5;
// The probe Studio allocates with; a remap probes with the mod id plus this suffix.
constexpr std::string_view kProbeSeed = "sunrise-mod-packages\n", kRemapSuffix = "#remap";
// Rewritten copies live in folders under the merge directory, which the prune leaves alone.
constexpr std::wstring_view kRemapFolder = L"remap";
// The remap file is small; one entry per remapped mod.
constexpr std::size_t kRemapFileByteLimit = std::size_t{1} << 20;
constexpr std::uint64_t kRemapFileVersion = 1;
// JSON nesting the remap file uses: root, mod list, mod, move list, move.
constexpr unsigned kRemapFileDepth = 8;

/** What one mod's refmap.bin says. */
struct ReferenceMap final {
    std::string modId;
    std::vector<std::uint16_t> packages;
    std::vector<remap::Location> locations;
    std::size_t luaRows{};
};

/** Reads little-endian fields front to back; any read past the end fails. */
struct Cursor final {
    std::span<const std::byte> bytes;
    std::size_t at{};

    template <typename Value> [[nodiscard]] bool take(Value& output) noexcept {
        if (bytes.size() - at < sizeof(Value) || at > bytes.size()) {
            return false;
        }
        std::memcpy(&output, bytes.data() + at, sizeof(Value));
        at += sizeof(Value);
        return true;
    }

    [[nodiscard]] bool skip(std::size_t count) noexcept {
        if (at > bytes.size() || bytes.size() - at < count) {
            return false;
        }
        at += count;
        return true;
    }

    void align() noexcept {
        at = (at + 3) & ~std::size_t{3};
    }
};

/**
 * Parses refmap.bin. Lua rows are counted, not kept: copies of scripts are not served yet.
 * @return False when the file is missing or breaks the format.
 */
[[nodiscard]] bool read_map(const std::wstring& path, ReferenceMap& output) {
    std::vector<std::byte> bytes;
    if (read_whole_file(path, kMapByteLimit, bytes) != FileRead::ok) {
        return false;
    }
    Cursor cursor{bytes};
    std::uint32_t magic = 0;
    std::uint16_t version = 0;
    std::uint16_t packageCount = 0;
    std::uint32_t rowCount = 0;
    std::uint16_t fileCount = 0;
    std::uint16_t idLength = 0;
    if (!cursor.take(magic) || !cursor.take(version) || !cursor.take(packageCount)
        || !cursor.take(rowCount) || !cursor.take(fileCount) || !cursor.take(idLength)
        || magic != kMapMagic || version != kMapVersion || bytes.size() - cursor.at < idLength) {
        return false;
    }
    output.modId.assign(reinterpret_cast<const char*>(bytes.data() + cursor.at), idLength);
    (void)cursor.skip(idLength);
    cursor.align();
    output.packages.resize(packageCount);
    for (std::uint16_t& package : output.packages) {
        if (!cursor.take(package)) {
            return false;
        }
    }
    cursor.align();
    for (std::uint16_t file = 0; file < fileCount; ++file) {
        std::uint16_t length = 0;
        if (!cursor.take(length) || !cursor.skip(length)) {
            return false;
        }
    }
    cursor.align();
    if (cursor.at > bytes.size()
        || (bytes.size() - cursor.at) != std::size_t{rowCount} * kRowSize) {
        return false;
    }
    for (std::uint32_t row = 0; row < rowCount; ++row) {
        std::uint32_t package = 0;
        std::uint32_t entry = 0;
        std::uint32_t offset = 0;
        std::uint32_t kind = 0;
        (void)cursor.take(package);
        (void)cursor.take(entry);
        (void)cursor.take(offset);
        (void)cursor.take(kind);
        switch (kind) {
        case kTagRow:
        case kSidecarRow:
        case kPackageRow:
            if (package > (std::numeric_limits<std::uint16_t>::max)()) {
                return false;
            }
            output.locations.push_back({static_cast<std::uint16_t>(package),
                                        entry,
                                        offset,
                                        static_cast<remap::Kind>(kind)});
            break;
        case kWideRow:
            break;
        case kLuaRow:
            ++output.luaRows;
            break;
        default:
            return false;
        }
    }
    std::sort(output.packages.begin(), output.packages.end());
    return true;
}

/** @return True when another source holds the id: installed, startup or an earlier mod. */
[[nodiscard]] bool taken(const Work& work, std::uint16_t id) noexcept {
    return work.bases[id].present || work.reserved.test(id);
}

/** @return Who holds a taken id, for the warning. */
[[nodiscard]] std::string holder(const Work& work, std::uint16_t id) {
    if (work.reserved.test(id)) {
        return "a startup package";
    }
    const Base& base = work.bases[id];
    return base.owner == kInstalledOwner ? std::string("the installed packages")
                                         : std::string(work.records[base.owner].manifest.id.view());
}

/**
 * The first id of the mod's remap probe that `blocked` lets through: SHA-256 of the seed picks
 * a start in the new-package range, then ids run upward, wrapping.
 * @return Zero when every id is blocked.
 */
template <typename Blocked>
[[nodiscard]] std::uint16_t probe(std::string_view modId, const Blocked& blocked) {
    std::string seed(kProbeSeed);
    seed.append(modId).append(kRemapSuffix);
    sha256::Digest digest{};
    if (!sha256::hash(std::as_bytes(std::span(seed.data(), seed.size())), digest)) {
        return 0;
    }
    std::uint32_t first = 0;
    std::memcpy(&first, digest.data(), sizeof first);
    const std::uint32_t range = kLastOwnPackageId - kFirstOwnPackageId + 1U;
    for (std::uint32_t step = 0; step < range; ++step) {
        const auto id = static_cast<std::uint16_t>(kFirstOwnPackageId + (first + step) % range);
        if (!blocked(id)) {
            return id;
        }
    }
    return 0;
}

/** @return The leaf with its package id replaced: ``<family>_<id>_<generation>.pkg``. */
[[nodiscard]] std::wstring renamed(std::wstring_view leaf, std::uint16_t id) {
    const std::size_t last = leaf.rfind(L'_');
    const std::size_t before = last == std::wstring_view::npos ? last : leaf.rfind(L'_', last - 1);
    if (before == std::wstring_view::npos) {
        return std::wstring(leaf);
    }
    std::array<wchar_t, 5> digits{};
    (void)swprintf_s(digits.data(), digits.size(), L"%04x", static_cast<unsigned>(id));
    return std::wstring(leaf.substr(0, before + 1)).append(digits.data()).append(leaf.substr(last));
}

/** @return The ASCII form of a leaf for messages; other characters become '?'. */
[[nodiscard]] std::string narrow(std::wstring_view leaf) {
    std::string output;
    for (const wchar_t character : leaf) {
        output.push_back(character < 0x80 ? static_cast<char>(character) : '?');
    }
    return output;
}

[[nodiscard]] bool make_directory(const std::wstring& path) {
    return CreateDirectoryW(path.c_str(), nullptr) != FALSE
           || GetLastError() == ERROR_ALREADY_EXISTS;
}

/** @return The saved moves of a mod, when its manifest still names the same packages. */
[[nodiscard]] const SavedRemap* saved_for(const Work& work, const Manifest& manifest) {
    const auto packages = std::span(manifest.packages).first(manifest.packageCount);
    for (const SavedRemap& saved : work.savedRemaps) {
        if (saved.modId == manifest.id.view()
            && std::equal(
                saved.packages.begin(), saved.packages.end(), packages.begin(), packages.end())) {
            return &saved;
        }
    }
    return nullptr;
}

/**
 * Chooses each own package's id: a saved one while it is still free, else a free id from the
 * remap probe when the own id is taken, else the own id.
 * @return False when no free id is left.
 */
[[nodiscard]] bool choose(const Work& work,
                          std::string_view modId,
                          std::span<const std::uint16_t> own,
                          const SavedRemap* saved,
                          std::vector<remap::Move>& moves) {
    std::vector<std::uint16_t> chosen;
    const auto blocked = [&](std::uint16_t id) {
        return taken(work, id) || std::find(own.begin(), own.end(), id) != own.end()
               || std::find(chosen.begin(), chosen.end(), id) != chosen.end();
    };
    for (const std::uint16_t id : own) {
        std::uint16_t target = id;
        if (saved != nullptr) {
            const auto previous =
                std::find_if(saved->moves.begin(),
                             saved->moves.end(),
                             [id](const remap::Move& move) { return move.from == id; });
            if (previous != saved->moves.end() && !blocked(previous->to)) {
                target = previous->to;
            }
        }
        if (target == id && taken(work, id)) {
            target = probe(modId, blocked);
            if (target == 0) {
                return false;
            }
        }
        if (target != id) {
            moves.push_back({id, target});
            chosen.push_back(target);
        }
    }
    return true;
}

/** Appends one JSON string of ASCII id text. */
void quote(std::string& output, std::string_view text) {
    output.push_back('"');
    output.append(text);
    output.push_back('"');
}

/** Appends one package id as a "0x" hex string. */
void hex_id(std::string& output, std::uint16_t id) {
    std::array<char, 8> text{};
    (void)std::snprintf(text.data(), text.size(), "\"0x%04X\"", static_cast<unsigned>(id));
    output.append(text.data());
}

/** Reads one "0x" package id. */
[[nodiscard]] bool read_id(core::json::Reader& reader, std::uint16_t& output) noexcept {
    std::uint64_t value = 0;
    if (!reader.unsigned_value(value) || !own_package_id(value)) {
        return false;
    }
    output = static_cast<std::uint16_t>(value);
    return true;
}

/**
 * Reads one saved mod: its id, its manifest packages and its moves.
 * @return False on a missing or malformed field.
 */
[[nodiscard]] bool read_saved(core::json::Reader& reader, SavedRemap& output) {
    if (!reader.consume('{')) {
        return false;
    }
    bool more = !reader.consume('}');
    while (more) {
        std::string_view key;
        if (!reader.string(key) || !reader.consume(':')) {
            return false;
        }
        if (key == "id") {
            std::string_view id;
            if (!reader.string(id)) {
                return false;
            }
            output.modId.assign(id);
        } else if (key == "packages") {
            if (!reader.consume('[')) {
                return false;
            }
            for (bool next = !reader.consume(']'); next;) {
                std::uint16_t id = 0;
                if (!read_id(reader, id)) {
                    return false;
                }
                output.packages.push_back(id);
                next = !reader.consume(']');
                if (next && !reader.consume(',')) {
                    return false;
                }
            }
        } else if (key == "moves") {
            if (!reader.consume('[')) {
                return false;
            }
            for (bool next = !reader.consume(']'); next;) {
                remap::Move move{};
                std::string_view field;
                if (!reader.consume('{') || !reader.string(field) || field != "from"
                    || !reader.consume(':') || !read_id(reader, move.from) || !reader.consume(',')
                    || !reader.string(field) || field != "to" || !reader.consume(':')
                    || !read_id(reader, move.to) || !reader.consume('}')) {
                    return false;
                }
                output.moves.push_back(move);
                next = !reader.consume(']');
                if (next && !reader.consume(',')) {
                    return false;
                }
            }
        } else if (!reader.skip_value(kRemapFileDepth)) {
            return false;
        }
        more = !reader.consume('}');
        if (more && !reader.consume(',')) {
            return false;
        }
    }
    std::sort(output.packages.begin(), output.packages.end());
    return !output.modId.empty();
}

/** @return The remap file text for these entries. */
[[nodiscard]] std::string remap_text(std::span<const SavedRemap> entries) {
    std::string text = "{\n  \"version\": 1,\n  \"mods\": [";
    for (std::size_t index = 0; index < entries.size(); ++index) {
        const SavedRemap& entry = entries[index];
        text.append(index == 0 ? "\n    {\"id\": " : ",\n    {\"id\": ");
        quote(text, entry.modId);
        text.append(", \"packages\": [");
        for (std::size_t id = 0; id < entry.packages.size(); ++id) {
            if (id != 0) {
                text.append(", ");
            }
            hex_id(text, entry.packages[id]);
        }
        text.append("], \"moves\": [");
        for (std::size_t move = 0; move < entry.moves.size(); ++move) {
            text.append(move == 0 ? "{\"from\": " : ", {\"from\": ");
            hex_id(text, entry.moves[move].from);
            text.append(", \"to\": ");
            hex_id(text, entry.moves[move].to);
            text.push_back('}');
        }
        text.append("]}");
    }
    text.append(entries.empty() ? "]\n}\n" : "\n  ]\n}\n");
    return text;
}

} // namespace

/** Plans the moves first; a mod with nothing to move stages its own files untouched. */
bool relocate(Work& work,
              std::size_t recordIndex,
              const std::wstring& directory,
              std::vector<Listed>& added,
              std::vector<Listed>& patches) {
    Record& record = work.records[recordIndex];
    const std::string_view modId = record.manifest.id.view();
    std::vector<std::uint16_t> own;
    for (const Listed& file : added) {
        if (file.status == name::NameStatus::recognized && own_package_id(file.parsed.packageId)) {
            own.push_back(file.parsed.packageId);
        }
    }
    std::sort(own.begin(), own.end());
    own.erase(std::unique(own.begin(), own.end()), own.end());
    const SavedRemap* saved = saved_for(work, record.manifest);
    const bool clash =
        std::any_of(own.begin(), own.end(), [&](std::uint16_t id) { return taken(work, id); });
    if (!clash && (saved == nullptr || saved->moves.empty())) {
        return true;
    }
    ReferenceMap map;
    const auto manifestPackages =
        std::span(record.manifest.packages).first(record.manifest.packageCount);
    if (!read_map(join(directory, kMapLeaf), map) || map.modId != modId
        || !std::equal(map.packages.begin(),
                       map.packages.end(),
                       manifestPackages.begin(),
                       manifestPackages.end())
        || !std::equal(own.begin(), own.end(), manifestPackages.begin(), manifestPackages.end())) {
        // No usable map: the stage refuses the clash as it always has.
        return true;
    }
    if (map.luaRows != 0) {
        problem::warn(record,
                      Reason::packageRemapped,
                      "Its packages cannot move to free ids: its scripts name its own tags.");
        return true;
    }
    std::vector<remap::Move> moves;
    if (!choose(work, modId, own, saved, moves)) {
        problem::refuse(
            record, Reason::capacity, "No free package id is left to move its packages to.");
        return false;
    }
    if (moves.empty()) {
        return true;
    }
    const std::wstring root = join(join(std::wstring(work.paths.mergeDirectory), kRemapFolder),
                                   std::wstring(modId.begin(), modId.end()));
    if (!ensure_merge_directory(work)
        || !make_directory(join(std::wstring(work.paths.mergeDirectory), kRemapFolder))
        || !make_directory(root) || !make_directory(join(root, kPackagesFolder))
        || !make_directory(join(root, kPatchesFolder))) {
        problem::refuse(
            record, Reason::folderPath, "The folder for its moved packages could not be made.");
        return false;
    }
    // Every family a move or a row touches is copied; the others stage from the mod folder.
    std::vector<std::uint16_t> families;
    families.reserve(moves.size() + map.locations.size());
    for (const remap::Move& move : moves) {
        families.push_back(move.from);
    }
    for (const remap::Location& location : map.locations) {
        families.push_back(location.package);
    }
    const auto touched = [&](std::uint16_t id) {
        return std::find(families.begin(), families.end(), id) != families.end();
    };
    std::vector<remap::File> files;
    std::vector<Listed*> listed;
    for (auto* folder : {&added, &patches}) {
        const std::wstring_view leafFolder = folder == &added ? kPackagesFolder : kPatchesFolder;
        for (Listed& file : *folder) {
            if (file.status != name::NameStatus::recognized || !touched(file.parsed.packageId)) {
                continue;
            }
            const std::wstring leaf = file.path.substr(file.path.find_last_of(L'\\') + 1);
            const auto move =
                std::find_if(moves.begin(), moves.end(), [&](const remap::Move& entry) {
                    return entry.from == file.parsed.packageId;
                });
            const std::wstring output =
                join(join(root, leafFolder), move == moves.end() ? leaf : renamed(leaf, move->to));
            files.push_back({file.path, output});
            listed.push_back(&file);
        }
    }
    if (const merge::Status status = remap::rewrite(files, map.locations, moves);
        status != merge::Status::ok) {
        problem::refuse(record,
                        Reason::packageInvalid,
                        "Its packages could not be moved to free ids: %s.",
                        merge::status_text(status));
        return false;
    }
    for (std::size_t index = 0; index < files.size(); ++index) {
        Listed& file = *listed[index];
        const std::wstring leaf =
            files[index].output.substr(files[index].output.find_last_of(L'\\') + 1);
        file.path = files[index].output;
        file.leaf = narrow(leaf);
        file.status = name::parse_file_name(leaf, file.parsed);
    }
    for (const remap::Move& move : moves) {
        problem::warn(record,
                      Reason::packageRemapped,
                      "Package 0x%04X is also used by %s, so this mod uses 0x%04X.",
                      move.from,
                      holder(work, move.from).c_str(),
                      move.to);
        core::log::writef(core::log::Channel::state,
                          core::log::Level::warn,
                          "ev=mods stage=remap mod=%s from=0x%04X to=0x%04X saved=%d",
                          record.manifest.id.bytes.data(),
                          move.from,
                          move.to,
                          saved != nullptr ? 1 : 0);
    }
    work.remaps.push_back({std::string(modId), own, moves});
    return true;
}

/** A malformed file is logged and read as empty, so the stage chooses again. */
void load_remaps(Work& work) {
    work.savedRemaps.clear();
    std::vector<std::byte> bytes;
    if (work.paths.remapsFile.empty()
        || read_whole_file(std::wstring(work.paths.remapsFile), kRemapFileByteLimit, bytes)
               != FileRead::ok) {
        return;
    }
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    core::json::Reader reader(text);
    std::vector<SavedRemap> entries;
    bool valid = reader.consume('{');
    for (bool more = valid && !reader.consume('}'); valid && more;) {
        std::string_view key;
        valid = reader.string(key) && reader.consume(':');
        if (valid && key == "version") {
            std::uint64_t version = 0;
            valid = reader.unsigned_integer(version) && version == kRemapFileVersion;
        } else if (valid && key == "mods") {
            valid = reader.consume('[');
            for (bool next = valid && !reader.consume(']'); valid && next;) {
                SavedRemap entry;
                valid = read_saved(reader, entry);
                entries.push_back(std::move(entry));
                next = valid && !reader.consume(']');
                valid = valid && (!next || reader.consume(','));
            }
        } else if (valid) {
            valid = reader.skip_value(kRemapFileDepth);
        }
        more = valid && !reader.consume('}');
        valid = valid && (!more || reader.consume(','));
    }
    if (!valid || !reader.at_end()) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::warn,
                         "ev=mods stage=remap_file result=ignored reason=format");
        return;
    }
    work.savedRemaps = std::move(entries);
}

/** Entries of mods that did not stage this time are kept, so they keep their ids when back. */
void save_remaps(const Work& work) {
    if (work.paths.remapsFile.empty()) {
        return;
    }
    try {
        std::vector<SavedRemap> entries = work.remaps;
        for (const SavedRemap& saved : work.savedRemaps) {
            const bool restaged =
                std::any_of(work.records.begin(), work.records.end(), [&](const Record& record) {
                    return record.status == Status::loaded
                           && record.manifest.id.view() == saved.modId;
                });
            if (!restaged) {
                entries.push_back(saved);
            }
        }
        std::sort(
            entries.begin(), entries.end(), [](const SavedRemap& first, const SavedRemap& second) {
                return first.modId < second.modId;
            });
        const std::string text = remap_text(entries);
        std::vector<std::byte> current;
        const std::wstring path(work.paths.remapsFile);
        if (read_whole_file(path, kRemapFileByteLimit, current) == FileRead::ok
            && current.size() == text.size()
            && std::memcmp(current.data(), text.data(), text.size()) == 0) {
            return;
        }
        if (entries.empty() && current.empty()) {
            return;
        }
        if (!core::path::replace_file(path, text)) {
            core::log::write(core::log::Channel::state,
                             core::log::Level::warn,
                             "ev=mods stage=remap_file result=fail reason=write");
        }
    } catch (...) {}
}

} // namespace sunrise::state::mods::packages::internal
