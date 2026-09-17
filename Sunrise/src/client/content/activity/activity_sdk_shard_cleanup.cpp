#include "activity_sdk_shard_cleanup.h"

#include <Windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sunrise::client::content::activity::sdk_generation {
namespace {

namespace generated = state::activity_sdk::generated_world;
namespace manifest = state::activity_sdk::generated_world::manifest;

/** Resolves one directory. */
[[nodiscard]] bool ordinary_directory_path(const std::wstring& input,
                                           std::wstring& output) noexcept {
    output.clear();
    const DWORD needed = GetFullPathNameW(input.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return false;
    }
    try {
        output.assign(static_cast<std::size_t>(needed), L'\0');
        const DWORD written = GetFullPathNameW(input.c_str(), needed, output.data(), nullptr);
        if (written == 0 || written >= needed) {
            output.clear();
            return false;
        }
        output.resize(written);
        if (output.size() < 3U || output[1] != L':' || output[2] != L'\\') {
            output.clear();
            return false;
        }
        std::size_t cursor = 3U;
        while (true) {
            const std::size_t separator = output.find(L'\\', cursor);
            const std::size_t end = separator == std::wstring::npos ? output.size() : separator;
            const std::wstring prefix = output.substr(0, end);
            const DWORD attributes = GetFileAttributesW(prefix.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES
                || (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                output.clear();
                return false;
            }
            if (separator == std::wstring::npos) {
                return true;
            }
            cursor = separator + 1U;
        }
    } catch (...) {
        output.clear();
        return false;
    }
}

/** Decodes one hexadecimal digit without changing output on failure. */
[[nodiscard]] bool hex_value(wchar_t character, unsigned& output) noexcept {
    if (character >= L'0' && character <= L'9') {
        output = static_cast<unsigned>(character - L'0');
        return true;
    }
    if (character >= L'a' && character <= L'f') {
        output = static_cast<unsigned>(character - L'a') + 10U;
        return true;
    }
    if (character >= L'A' && character <= L'F') {
        output = static_cast<unsigned>(character - L'A') + 10U;
        return true;
    }
    return false;
}

/** Recognizes only immutable shard names owned by this generator. */
[[nodiscard]] bool parse_shard_leaf(std::wstring_view leaf,
                                    std::uint32_t& scenarioTag,
                                    generated::Digest& digest) noexcept {
    // A shard leaf is an 8-hex tag, a dash, the hex digest, then the pack suffix.
    constexpr std::size_t kTagLength = 8;
    constexpr std::size_t kDigestLength = generated::Digest{}.size() * 2;
    constexpr std::wstring_view kSuffix = L".pack";
    if (leaf.size() != kTagLength + 1 + kDigestLength + kSuffix.size() || leaf[kTagLength] != L'-'
        || leaf.substr(leaf.size() - kSuffix.size()) != kSuffix) {
        return false;
    }
    scenarioTag = 0;
    for (std::size_t index = 0; index < kTagLength; ++index) {
        unsigned value = 0;
        if (!hex_value(leaf[index], value)) {
            return false;
        }
        scenarioTag = (scenarioTag << 4U) | value;
    }
    for (std::size_t index = 0; index < digest.size(); ++index) {
        unsigned high = 0;
        unsigned low = 0;
        const std::size_t offset = kTagLength + 1 + index * 2;
        if (!hex_value(leaf[offset], high) || !hex_value(leaf[offset + 1], low)) {
            return false;
        }
        digest[index] = static_cast<std::byte>((high << 4U) | low);
    }
    return scenarioTag != 0;
}

} // namespace

/** Deletes only owned shards missing from the active manifest or carrying a stale digest. */
bool clean_stale_shards(const std::wstring& directory,
                        std::span<const manifest::Record> active) noexcept {
    const auto ordered = [](const manifest::Record& first, const manifest::Record& second) {
        return first.scenarioTag < second.scenarioTag;
    };
    if (!std::is_sorted(active.begin(), active.end(), ordered)
        || std::adjacent_find(active.begin(),
                              active.end(),
                              [](const manifest::Record& first, const manifest::Record& second) {
                                  return first.scenarioTag == second.scenarioTag;
                              })
               != active.end()) {
        return false;
    }
    std::wstring canonicalDirectory;
    if (!ordinary_directory_path(directory, canonicalDirectory)) {
        return false;
    }
    std::wstring search;
    try {
        search = canonicalDirectory;
        search.append(L"\\*");
    } catch (...) {
        return false;
    }
    WIN32_FIND_DATAW found{};
    HANDLE cursor = FindFirstFileW(search.c_str(), &found);
    if (cursor == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    bool complete = true;
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        const std::wstring_view leaf(found.cFileName);
        bool remove = false;
        std::uint32_t scenarioTag = 0;
        generated::Digest digest{};
        if (parse_shard_leaf(leaf, scenarioTag, digest)) {
            const auto record =
                std::lower_bound(active.begin(),
                                 active.end(),
                                 scenarioTag,
                                 [](const manifest::Record& row, std::uint32_t tag) {
                                     return row.scenarioTag < tag;
                                 });
            remove = record == active.end() || record->scenarioTag != scenarioTag
                     || record->shardPayloadSha256 != digest;
        }
        if (!remove) {
            continue;
        }
        try {
            std::wstring path = canonicalDirectory;
            path.push_back(L'\\');
            path.append(leaf);
            complete = DeleteFileW(path.c_str()) != FALSE && complete;
        } catch (...) {
            complete = false;
        }
    } while (FindNextFileW(cursor, &found) != FALSE);
    const DWORD endError = GetLastError();
    return FindClose(cursor) != FALSE && endError == ERROR_NO_MORE_FILES && complete;
}

} // namespace sunrise::client::content::activity::sdk_generation
