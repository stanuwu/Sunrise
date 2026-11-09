#include "compatibility.h"

#include <Windows.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "../../core/logging/log.h"
#include "../../core/settings/settings.h"
#include "../content_manifest/content_manifest_state_runtime.h"
#include "../mods/mods_runtime.h"

namespace sunrise::state::coop::compatibility {
namespace {

namespace sha256 = middleware::crypto::sha256;

/** Leads a summary. */
constexpr std::array<char, 4> kSummaryMagic{'S', 'R', 'C', 'P'};
/** Leads a refusal. */
constexpr std::array<char, 4> kRefusalMagic{'S', 'R', 'C', 'R'};
/** Raised whenever either wire form changes; peers with another value never match. */
constexpr std::uint8_t kWireVersion = 1;
/** Separates the join code's key hash from any other use of the same text. */
constexpr std::string_view kJoinCodeDomain = "sunrise-coop-join-key:";
/** Field 15, length-delimited: a tag no native hello carries. */
constexpr std::byte kHelloFieldTag{0x7A};
/** Low seven bits of a varint byte, and the bit that says another byte follows. */
constexpr unsigned kVarintBits = 7;
constexpr unsigned kVarintMore = 0x80;
/** Bytes the hello field's length counts: the summary and the public key. */
constexpr std::size_t kHelloFieldBody = kSummarySize + kPublicKeySize;
static_assert(kHelloFieldBody >= kVarintMore && kHelloFieldBody < kVarintMore * kVarintMore);
/** Bytes read from a file per hash step. */
constexpr std::size_t kReadChunk = 1024 * 1024;

#pragma pack(push, 1)
struct SummaryWire final {
    std::array<char, 4> magic;
    std::uint8_t version;
    Digest build;
    Digest content;
    std::uint8_t cheats;
    std::uint8_t modCount;
    Digest modList;
};
struct RefusalHead final {
    std::array<char, 4> magic;
    std::uint8_t version;
    std::uint8_t mismatch;
};
struct ModWire final {
    std::uint8_t idLength;
    std::array<char, mods::kIdMaximumLength> id;
    std::uint16_t major;
    std::uint16_t minor;
    std::uint16_t patch;
    Digest files;
};
#pragma pack(pop)
static_assert(sizeof(SummaryWire) == kSummarySize);
static_assert(kRefusalCapacity
              == sizeof(RefusalHead) + sizeof(SummaryWire) + mods::kModCapacity * sizeof(ModWire));

Record g_local{};
std::atomic<const Record*> g_published{};

/** Adds one little-endian 64-bit value. */
void add_u64(sha256::Stream& stream, std::uint64_t value) noexcept {
    std::array<std::byte, 8> bytes{};
    std::memcpy(bytes.data(), &value, bytes.size());
    stream.add(bytes);
}

/** Adds the bytes of one file. @return False when it cannot be read whole. */
[[nodiscard]] bool add_file(sha256::Stream& stream, const std::wstring& path) noexcept {
    const HANDLE file = CreateFileW(path.c_str(),
                                    GENERIC_READ,
                                    FILE_SHARE_READ,
                                    nullptr,
                                    OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN,
                                    nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool ok = true;
    try {
        std::vector<std::byte> chunk(kReadChunk);
        for (;;) {
            DWORD read = 0;
            if (ReadFile(file, chunk.data(), static_cast<DWORD>(chunk.size()), &read, nullptr)
                == FALSE) {
                ok = false;
                break;
            }
            if (read == 0) {
                break;
            }
            stream.add(std::span(chunk).first(read));
        }
    } catch (...) {
        ok = false;
    }
    CloseHandle(file);
    return ok;
}

/** Collects every file under `root`, as paths relative to it. @return False on a failed scan. */
[[nodiscard]] bool list_files(const std::wstring& root,
                              const std::wstring& relative,
                              std::vector<std::wstring>& output) {
    WIN32_FIND_DATAW entry{};
    const std::wstring pattern = root + L"\\" + relative + (relative.empty() ? L"*" : L"\\*");
    const HANDLE search = FindFirstFileW(pattern.c_str(), &entry);
    if (search == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    bool ok = true;
    do {
        const std::wstring_view name(entry.cFileName);
        if (name == L"." || name == L"..") {
            continue;
        }
        const std::wstring child =
            relative.empty() ? std::wstring(name) : relative + L"\\" + std::wstring(name);
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            ok = list_files(root, child, output);
        } else {
            output.push_back(child);
        }
    } while (ok && FindNextFileW(search, &entry) != FALSE);
    if (ok && GetLastError() != ERROR_NO_MORE_FILES) {
        ok = false;
    }
    FindClose(search);
    return ok;
}

/** @return `text` lower-cased, so path order and hashes ignore case as Windows does. */
[[nodiscard]] std::wstring lowered(std::wstring text) {
    if (!text.empty()) {
        CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
    }
    return text;
}

/** Hashes one mod folder: each file's lower-case relative path, size and bytes, path order. */
[[nodiscard]] bool hash_folder(const std::wstring& root, Digest& output) noexcept {
    try {
        std::vector<std::wstring> files;
        if (!list_files(root, {}, files)) {
            return false;
        }
        for (auto& file : files) {
            file = lowered(file);
        }
        std::sort(files.begin(), files.end());
        sha256::Stream stream;
        for (const auto& file : files) {
            add_u64(stream, file.size());
            stream.add(std::as_bytes(std::span(file)));
            WIN32_FILE_ATTRIBUTE_DATA data{};
            const std::wstring full = root + L"\\" + file;
            if (GetFileAttributesExW(full.c_str(), GetFileExInfoStandard, &data) == FALSE) {
                return false;
            }
            add_u64(stream,
                    (static_cast<std::uint64_t>(data.nFileSizeHigh) << 32U) | data.nFileSizeLow);
            if (!add_file(stream, full)) {
                return false;
            }
        }
        return stream.finish(output);
    } catch (...) {
        return false;
    }
}

/** Visitor context: the record being filled and the first failure. */
struct ModCollection final {
    Record* record{};
    bool ok{true};
};

/** Hashes every loaded mod into the record. */
bool collect_mods(void* context, const mods::View& view) noexcept {
    auto& collection = *static_cast<ModCollection*>(context);
    Record& record = *collection.record;
    try {
        for (std::size_t index = 0; index < view.count; ++index) {
            const mods::Record& mod = view.records[index];
            if (mod.status != mods::Status::loaded || record.modCount >= record.mods.size()) {
                continue;
            }
            const std::string_view folder = mod.folder.view();
            std::wstring root(view.modsDirectory);
            root += L"\\";
            root.append(folder.begin(), folder.end());
            Mod& entry = record.mods[record.modCount];
            entry.id = mod.manifest.id;
            entry.version = mod.manifest.version;
            if (!hash_folder(root, entry.files)) {
                core::log::writef(core::log::Channel::state,
                                  core::log::Level::error,
                                  "ev=coop stage=mod_hash result=fail mod=%.*s",
                                  static_cast<int>(entry.id.view().size()),
                                  entry.id.view().data());
                collection.ok = false;
                return true;
            }
            ++record.modCount;
        }
    } catch (...) {
        collection.ok = false;
    }
    return true;
}

/** Hashes the sorted mod list into one value. */
[[nodiscard]] bool hash_mod_list(Record& record) noexcept {
    std::sort(record.mods.begin(),
              record.mods.begin() + static_cast<std::ptrdiff_t>(record.modCount),
              [](const Mod& left, const Mod& right) { return left.id.view() < right.id.view(); });
    sha256::Stream stream;
    for (std::size_t index = 0; index < record.modCount; ++index) {
        const Mod& mod = record.mods[index];
        add_u64(stream, mod.id.view().size());
        stream.add(std::as_bytes(std::span(mod.id.view())));
        add_u64(stream,
                (std::uint64_t{mod.version.major} << 32U)
                    | (std::uint64_t{mod.version.minor} << 16U) | mod.version.patch);
        stream.add(mod.files);
    }
    return stream.finish(record.modList);
}

/** Hashes the module file this code runs from. */
[[nodiscard]] bool hash_module(void* module, Digest& output) noexcept {
    try {
        std::wstring path(MAX_PATH, L'\0');
        for (;;) {
            const DWORD length = GetModuleFileNameW(
                static_cast<HMODULE>(module), path.data(), static_cast<DWORD>(path.size()));
            if (length == 0) {
                return false;
            }
            if (length < path.size()) {
                path.resize(length);
                break;
            }
            path.resize(path.size() * 2);
        }
        sha256::Stream stream;
        return add_file(stream, path) && stream.finish(output);
    } catch (...) {
        return false;
    }
}

/** Writes one summary into a buffer known to be large enough. */
void write_summary(const Record& record, std::byte* output) noexcept {
    SummaryWire wire{};
    wire.magic = kSummaryMagic;
    wire.version = kWireVersion;
    wire.build = record.build;
    wire.content = record.content;
    wire.cheats = record.cheats ? 1U : 0U;
    wire.modCount = static_cast<std::uint8_t>(record.modCount);
    wire.modList = record.modList;
    std::memcpy(output, &wire, sizeof wire);
}

/** @return The mod named `id` in `record`, or null. */
[[nodiscard]] const Mod* find_mod(const Record& record, std::string_view id) noexcept {
    for (std::size_t index = 0; index < record.modCount; ++index) {
        if (record.mods[index].id.view() == id) {
            return &record.mods[index];
        }
    }
    return nullptr;
}

/** Names the first mod difference between this install and the host. */
int describe_mods(const Record& local, const Record& host, std::span<char> output) noexcept {
    for (std::size_t index = 0; index < host.modCount; ++index) {
        const Mod& theirs = host.mods[index];
        const std::string_view id = theirs.id.view();
        const Mod* ours = find_mod(local, id);
        const int idLength = static_cast<int>(id.size());
        if (ours == nullptr) {
            return std::snprintf(output.data(),
                                 output.size(),
                                 "Co-op refused: the host has mod %.*s %u.%u.%u, which is not "
                                 "installed here.",
                                 idLength,
                                 id.data(),
                                 unsigned{theirs.version.major},
                                 unsigned{theirs.version.minor},
                                 unsigned{theirs.version.patch});
        }
        if (ours->version.major != theirs.version.major
            || ours->version.minor != theirs.version.minor
            || ours->version.patch != theirs.version.patch) {
            return std::snprintf(output.data(),
                                 output.size(),
                                 "Co-op refused: mod %.*s is %u.%u.%u here and %u.%u.%u on the "
                                 "host.",
                                 idLength,
                                 id.data(),
                                 unsigned{ours->version.major},
                                 unsigned{ours->version.minor},
                                 unsigned{ours->version.patch},
                                 unsigned{theirs.version.major},
                                 unsigned{theirs.version.minor},
                                 unsigned{theirs.version.patch});
        }
        if (ours->files != theirs.files) {
            return std::snprintf(output.data(),
                                 output.size(),
                                 "Co-op refused: mod %.*s has the same version but different "
                                 "files than the host's copy.",
                                 idLength,
                                 id.data());
        }
    }
    for (std::size_t index = 0; index < local.modCount; ++index) {
        const std::string_view id = local.mods[index].id.view();
        if (find_mod(host, id) == nullptr) {
            return std::snprintf(output.data(),
                                 output.size(),
                                 "Co-op refused: mod %.*s is installed here but not on the host.",
                                 static_cast<int>(id.size()),
                                 id.data());
        }
    }
    return std::snprintf(
        output.data(), output.size(), "Co-op refused: the mod list differs from the host's.");
}

} // namespace

bool initialize(void* module) noexcept {
    Record& record = g_local;
    record = {};
    if (!hash_module(module, record.build)) {
        core::log::write(core::log::Channel::state,
                         core::log::Level::error,
                         "ev=coop stage=build_hash result=fail");
        return false;
    }
    content_manifest::Fingerprint content{};
    if (content_manifest::build_fingerprint(content)) {
        std::memcpy(record.content.data(), content.data(), record.content.size());
    }
    const auto& settings = core::settings::get();
    record.cheats = settings.client.cheats || mods::cheat_count() != 0;
    if (settings.joinCodeLength != 0) {
        const std::string_view code(settings.joinCode.data(), settings.joinCodeLength);
        if (!sha256::hash_pair(std::as_bytes(std::span(kJoinCodeDomain)),
                               std::as_bytes(std::span(code)),
                               record.joinSecret)) {
            return false;
        }
    }
    ModCollection collection{&record};
    (void)mods::visit(&collect_mods, &collection);
    if (!collection.ok || !hash_mod_list(record)) {
        return false;
    }
    g_published.store(&g_local, std::memory_order_release);
    core::log::writef(core::log::Channel::state,
                      core::log::Level::info,
                      "ev=coop stage=compatibility result=ok mods=%zu cheats=%d join_code=%d",
                      record.modCount,
                      record.cheats ? 1 : 0,
                      settings.joinCodeLength != 0 ? 1 : 0);
    return true;
}

const Record* local() noexcept {
    return g_published.load(std::memory_order_acquire);
}

bool encode_summary(const Record& record,
                    std::span<std::byte> output,
                    std::size_t& written) noexcept {
    written = 0;
    if (output.size() < sizeof(SummaryWire)) {
        return false;
    }
    write_summary(record, output.data());
    written = sizeof(SummaryWire);
    return true;
}

bool decode_summary(std::span<const std::byte> input, Record& output) noexcept {
    output = {};
    if (input.size() < sizeof(SummaryWire)) {
        return false;
    }
    SummaryWire wire{};
    std::memcpy(&wire, input.data(), sizeof wire);
    if (wire.magic != kSummaryMagic || wire.version != kWireVersion
        || wire.modCount > mods::kModCapacity) {
        return false;
    }
    output.build = wire.build;
    output.content = wire.content;
    output.cheats = wire.cheats != 0;
    output.modCount = wire.modCount;
    output.modList = wire.modList;
    return true;
}

bool encode_refusal(const Record& host,
                    Mismatch mismatch,
                    std::span<std::byte> output,
                    std::size_t& written) noexcept {
    written = 0;
    const std::size_t size =
        sizeof(RefusalHead) + sizeof(SummaryWire) + host.modCount * sizeof(ModWire);
    if (output.size() < size || host.modCount > mods::kModCapacity) {
        return false;
    }
    const RefusalHead head{kRefusalMagic, kWireVersion, static_cast<std::uint8_t>(mismatch)};
    std::memcpy(output.data(), &head, sizeof head);
    write_summary(host, output.data() + sizeof head);
    std::byte* cursor = output.data() + sizeof head + sizeof(SummaryWire);
    for (std::size_t index = 0; index < host.modCount; ++index) {
        const Mod& mod = host.mods[index];
        ModWire wire{};
        wire.idLength = static_cast<std::uint8_t>(mod.id.length);
        std::copy_n(mod.id.bytes.begin(), mod.id.length, wire.id.begin());
        wire.major = mod.version.major;
        wire.minor = mod.version.minor;
        wire.patch = mod.version.patch;
        wire.files = mod.files;
        std::memcpy(cursor, &wire, sizeof wire);
        cursor += sizeof wire;
    }
    written = size;
    return true;
}

bool decode_refusal(std::span<const std::byte> input, Mismatch& mismatch, Record& host) noexcept {
    mismatch = Mismatch::none;
    host = {};
    RefusalHead head{};
    if (input.size() < sizeof head) {
        return false;
    }
    std::memcpy(&head, input.data(), sizeof head);
    if (head.magic != kRefusalMagic || head.version != kWireVersion
        || head.mismatch > static_cast<std::uint8_t>(Mismatch::content)
        || !decode_summary(input.subspan(sizeof head), host)) {
        return false;
    }
    const std::size_t entries = sizeof head + sizeof(SummaryWire);
    if (input.size() != entries + host.modCount * sizeof(ModWire)) {
        return false;
    }
    for (std::size_t index = 0; index < host.modCount; ++index) {
        ModWire wire{};
        std::memcpy(&wire, input.data() + entries + index * sizeof wire, sizeof wire);
        if (wire.idLength > mods::kIdMaximumLength) {
            return false;
        }
        Mod& mod = host.mods[index];
        std::copy_n(wire.id.begin(), wire.idLength, mod.id.bytes.begin());
        mod.id.length = wire.idLength;
        mod.version = {wire.major, wire.minor, wire.patch};
        mod.files = wire.files;
    }
    mismatch = static_cast<Mismatch>(head.mismatch);
    return true;
}

bool encode_hello_field(const Record& record,
                        std::span<const std::byte, kPublicKeySize> publicKey,
                        std::span<std::byte, kHelloFieldSize> output) noexcept {
    output[0] = kHelloFieldTag;
    output[1] = static_cast<std::byte>((kHelloFieldBody & (kVarintMore - 1)) | kVarintMore);
    output[2] = static_cast<std::byte>(kHelloFieldBody >> kVarintBits);
    std::size_t written = 0;
    if (!encode_summary(record, output.subspan<3, kSummarySize>(), written)
        || written != kSummarySize) {
        return false;
    }
    std::memcpy(output.data() + 3 + kSummarySize, publicKey.data(), kPublicKeySize);
    return true;
}

bool find_hello_field(std::span<const std::byte> tail,
                      std::span<const std::byte>& summary,
                      std::span<const std::byte>& publicKey) noexcept {
    summary = {};
    publicKey = {};
    if (tail.size() != kHelloFieldSize || tail[0] != kHelloFieldTag) {
        return false;
    }
    const std::size_t length = (std::to_integer<std::size_t>(tail[1]) & (kVarintMore - 1))
                               | (std::to_integer<std::size_t>(tail[2]) << kVarintBits);
    if ((std::to_integer<unsigned>(tail[1]) & kVarintMore) == 0 || length != kHelloFieldBody) {
        return false;
    }
    summary = tail.subspan(3, kSummarySize);
    publicKey = tail.subspan(3 + kSummarySize, kPublicKeySize);
    return true;
}

Mismatch compare(const Record& guest, const Record& host) noexcept {
    if (guest.build != host.build) {
        return Mismatch::build;
    }
    if (guest.cheats != host.cheats) {
        return Mismatch::cheats;
    }
    if (guest.modCount != host.modCount || guest.modList != host.modList) {
        return Mismatch::mods;
    }
    return guest.content != host.content ? Mismatch::content : Mismatch::none;
}

const char* mismatch_name(Mismatch mismatch) noexcept {
    switch (mismatch) {
    case Mismatch::none:
        return "none";
    case Mismatch::build:
        return "build";
    case Mismatch::joinCode:
        return "join_code";
    case Mismatch::cheats:
        return "cheats";
    case Mismatch::mods:
        return "mods";
    case Mismatch::content:
        return "content";
    }
    return "unknown";
}

std::size_t describe(Mismatch mismatch,
                     const Record& local,
                     const Record& host,
                     std::span<char> output) noexcept {
    if (output.empty()) {
        return 0;
    }
    int written = 0;
    switch (mismatch) {
    case Mismatch::build:
        written = std::snprintf(output.data(),
                                output.size(),
                                "Co-op refused: the host runs a different Sunrise build.");
        break;
    case Mismatch::joinCode:
        written = std::snprintf(output.data(),
                                output.size(),
                                "Co-op refused: the join code does not match the host's.");
        break;
    case Mismatch::cheats:
        written = std::snprintf(output.data(),
                                output.size(),
                                "Co-op refused: cheats are %s here and %s on the host.",
                                local.cheats ? "on" : "off",
                                host.cheats ? "on" : "off");
        break;
    case Mismatch::mods:
        written = describe_mods(local, host, output);
        break;
    case Mismatch::content:
        written = std::snprintf(output.data(),
                                output.size(),
                                "Co-op refused: the installed game content differs from the "
                                "host's.");
        break;
    case Mismatch::none:
        break;
    }
    if (written <= 0) {
        return 0;
    }
    return (std::min)(static_cast<std::size_t>(written), output.size() - 1);
}

} // namespace sunrise::state::coop::compatibility