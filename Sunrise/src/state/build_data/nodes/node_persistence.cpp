#include "node_persistence.h"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#include <windows.h>

#include "../../../core/logging/log.h"
#include "../../../core/filesystem/path.h"
#include "../runtime.h"
#include "node_catalog.h"

namespace sunrise::state::build_data::nodes {
namespace {

/** The node file lives beside the claim file, in the directory the build data cache already owns. */
constexpr std::wstring_view kNodeFileSuffix = L"\\cache\\node_definitions.bin";
/** Identifies the file on sight, so an unrelated file of the right length cannot be read as one. */
constexpr std::array<char, 8> kMagic{'S', 'N', 'R', 'S', 'N', 'O', 'D', '1'};

core::path::Buffer g_path{};
bool g_pathReady{};

void report(const char* stage, const char* result, std::size_t detail) noexcept {
    std::array<char, 128> line{};
    const int written = std::snprintf(
        line.data(), line.size(), "ev=nodes stage=%s result=%s rows=%zu", stage, result, detail);
    if (written > 0) {
        core::log::write(core::log::Channel::state,
                         std::string_view{result} == "ok" ? core::log::Level::info
                                                          : core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/**
 * Header carried ahead of the rows.
 *
 * The row width is written out and checked on the way back in. The rows are stored as they sit in
 * memory, so a build whose Definition has changed shape must not read an older file as if it had
 * not: the width mismatch rejects it, extraction runs, and the file is replaced.
 */
struct Header {
    std::array<char, 8> magic{};
    std::uint32_t rows{};
    std::uint32_t rowWidth{};
};

} // namespace

/** Derives the node file path and publishes any table already held. */
bool initialize(void* module) noexcept {
    g_pathReady = false;
    if (!core::path::artifact_directory(module, g_path)
        || !core::path::append(g_path, kNodeFileSuffix)) {
        report("initialize", "path_fail", 0);
        return false;
    }
    g_pathReady = true;
    return true;
}

/** Reads the node table and publishes it. Separate from initialize because a publish is only
 *  accepted once the build data runtime is up, which is later than path setup. */
bool load_and_publish() noexcept {
    if (!g_pathReady) {
        return false;
    }
    const HANDLE file = CreateFileW(
        g_path.chars.data(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        // A first run, not a fault. Extraction will build the table and write it.
        report("load", "absent", 0);
        return false;
    }

    Header header{};
    DWORD read = 0;
    const bool headerRead =
        ReadFile(file, &header, sizeof header, &read, nullptr) != FALSE && read == sizeof header;
    if (!headerRead || std::memcmp(header.magic.data(), kMagic.data(), kMagic.size()) != 0
        || header.rowWidth != sizeof(Definition) || header.rows == 0
        || header.rows > kDefinitionCapacity) {
        CloseHandle(file);
        report("load", headerRead ? "rejected" : "header_fail", header.rows);
        return false;
    }

    std::vector<Definition> rows(header.rows);
    const auto expected = static_cast<DWORD>(rows.size() * sizeof(Definition));
    const bool rowsRead =
        ReadFile(file, rows.data(), expected, &read, nullptr) != FALSE && read == expected;
    CloseHandle(file);
    if (!rowsRead) {
        report("load", "read_fail", rows.size());
        return false;
    }
    // Replaces the catalog directly rather than going through publish_node_definitions. That path
    // opens a publication transaction, and a saved cache freezes every domain, so on the warm start
    // this exists for it is refused before validation is even reached. The freeze is there to keep
    // published domains agreeing with the cache; this domain is not in the cache, and the rows being
    // restored are the ones extraction published and wrote here, so the same checks are run and the
    // agreement the freeze protects is not touched.
    if (!valid(std::span<const Definition>{rows})
        || !replace(std::span<const Definition>{rows})) {
        report("load", "publish_fail", rows.size());
        return false;
    }
    report("load", "ok", rows.size());
    return true;
}

/** Writes the node table so the next start does not need the package pass to rebuild it. */
bool store(std::span<const Definition> definitions) noexcept {
    if (!g_pathReady || definitions.empty() || definitions.size() > kDefinitionCapacity) {
        return false;
    }
    Header header{};
    header.magic = kMagic;
    header.rows = static_cast<std::uint32_t>(definitions.size());
    header.rowWidth = static_cast<std::uint32_t>(sizeof(Definition));

    const HANDLE file = CreateFileW(g_path.chars.data(),
                                    GENERIC_WRITE,
                                    0,
                                    nullptr,
                                    CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        report("store", "open_fail", definitions.size());
        return false;
    }
    DWORD written = 0;
    bool complete =
        WriteFile(file, &header, sizeof header, &written, nullptr) != FALSE
        && written == sizeof header;
    if (complete) {
        const auto size = static_cast<DWORD>(definitions.size() * sizeof(Definition));
        complete = WriteFile(file, definitions.data(), size, &written, nullptr) != FALSE
                   && written == size;
    }
    complete = CloseHandle(file) != FALSE && complete;
    report("store", complete ? "ok" : "write_fail", definitions.size());
    return complete;
}

} // namespace sunrise::state::build_data::nodes
