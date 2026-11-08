/**
 * Where a mission program's script and its Lua require path come from: loaded mods first, then
 * the Sunrise scripts folder.
 * Functions that touch the source buffers run under the mission runtime lock.
 */

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdio>
#include <span>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../state/mods/mods_runtime.h"
#include "mission_script_runtime.h"
#include "mission_script_runtime_internal.h"
#include "mission_script_vm.h"

namespace sunrise::server::activity::mission {
namespace {

/** Every search root ends in this pattern, so `require("a.b")` finds `a\b.lua` below it. */
constexpr std::wstring_view kLuaModulePattern = L"\\?.lua";
/** Separates search roots in a Lua package path. */
constexpr std::wstring_view kSearchRootSeparator = L";";
/** Separates a directory from its leaf. */
constexpr std::wstring_view kPathSeparator = L"\\";

std::array<char, lua_vm::kSourceByteCapacity> g_source{};
core::path::Buffer g_scriptRoot{};
std::array<char, lua_vm::kSearchPathByteCapacity> g_sdkLuaSearchPath{};
/** Mod controller paths are built here; the runtime lock serializes every lookup. */
core::path::Buffer g_modScriptPath{};

/** Handle of the module holding this code, found from a data address inside it. */
[[nodiscard]] HMODULE owning_module() noexcept {
    HMODULE module = nullptr;
    const auto address = reinterpret_cast<LPCWSTR>(g_source.data());
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           address,
                           &module)
        == FALSE) {
        return nullptr;
    }
    return module;
}

/** Folds the activity name into a lowercase file stem; other bytes become single underscores. */
[[nodiscard]] bool controller_stem(const sdk::Catalog& catalog,
                                   const format::Activity& activity,
                                   std::span<char> output) noexcept {
    if (output.size() < 2) {
        return false;
    }
    const std::string_view internal = catalog.string(activity.internalName);
    const std::string_view name =
        internal.empty() ? catalog.string(activity.displayName) : internal;
    std::size_t length = 0;
    bool separator = false;
    for (const unsigned char byte : name) {
        const bool digit = byte >= '0' && byte <= '9';
        const bool upper = byte >= 'A' && byte <= 'Z';
        const bool lower = byte >= 'a' && byte <= 'z';
        if (digit || upper || lower) {
            if (separator && length != 0) {
                if (length + 1 >= output.size()) {
                    return false;
                }
                output[length++] = '_';
            }
            separator = false;
            if (length + 1 >= output.size()) {
                return false;
            }
            output[length++] = static_cast<char>(upper ? byte + ('a' - 'A') : byte);
        } else {
            separator = true;
        }
    }
    if (length == 0) {
        // An activity with no authored name uses this fixed stem.
        constexpr std::string_view unnamed = "unnamed";
        if (unnamed.size() + 1 > output.size()) {
            return false;
        }
        std::copy(unnamed.begin(), unnamed.end(), output.begin());
        length = unnamed.size();
    }
    if (output[0] >= '0' && output[0] <= '9') {
        if (length + 2 > output.size()) {
            return false;
        }
        const auto used = static_cast<std::ptrdiff_t>(length);
        std::move_backward(output.begin(), output.begin() + used, output.begin() + used + 1);
        output[0] = '_';
        ++length;
    }
    output[length] = '\0';
    return true;
}

/** Writes `<stem>/<stem>.lua`: each mission owns a folder named after its script. */
[[nodiscard]] bool controller_name(const sdk::Catalog& catalog,
                                   const format::Activity& activity,
                                   std::span<char> output) noexcept {
    std::array<char, 120> stem{};
    if (!controller_stem(catalog, activity, stem)) {
        return false;
    }
    const int length =
        std::snprintf(output.data(), output.size(), "%s/%s.lua", stem.data(), stem.data());
    return length > 0 && static_cast<std::size_t>(length) < output.size();
}

/** Reads one whole file into the shared source buffer and says why it could not be read. */
[[nodiscard]] SourceStatus read_file(const core::path::Buffer& path,
                                     std::span<const char>& output) noexcept {
    output = {};
    HANDLE file = CreateFileW(path.chars.data(),
                              GENERIC_READ,
                              FILE_SHARE_READ | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND
                   ? SourceStatus::missing
                   : SourceStatus::fileError;
    }
    LARGE_INTEGER size{};
    const bool sizeReady = GetFileSizeEx(file, &size) != FALSE;
    if (!sizeReady || size.QuadPart <= 0
        || static_cast<unsigned long long>(size.QuadPart) > g_source.size()) {
        CloseHandle(file);
        return sizeReady ? SourceStatus::tooLarge : SourceStatus::fileError;
    }
    DWORD read = 0;
    const auto requested = static_cast<DWORD>(size.QuadPart);
    const bool loaded =
        ReadFile(file, g_source.data(), requested, &read, nullptr) != FALSE && read == requested;
    CloseHandle(file);
    if (!loaded) {
        return SourceStatus::fileError;
    }
    output = {g_source.data(), read};
    return SourceStatus::ready;
}

/** One controller lookup carried through the mod list. */
struct ModControllerLookup final {
    std::wstring_view name;
    std::span<const char>* output{};
    std::array<char, state::mods::kIdCapacity>* modId{};
    SourceStatus status{SourceStatus::missing};
};

/**
 * Reads the controller from the last loaded mod that ships it.
 * @return Always true; the lookup status says what was found.
 */
[[nodiscard]] bool read_mod_controller(void* opaque, const state::mods::View& view) noexcept {
    auto& lookup = *static_cast<ModControllerLookup*>(opaque);
    // A later mod in load order replaces an earlier mod's script, and every mod replaces the base.
    for (std::size_t index = view.loadedCount; index-- > 0;) {
        const state::mods::Record& record = view.records[index];
        g_modScriptPath = {};
        if (!record.hasScripts || !append_mod_directory(g_modScriptPath, view, record)
            || !core::path::append(g_modScriptPath, kPathSeparator)
            || !core::path::append(g_modScriptPath, state::mods::kScriptsFolder)
            || !core::path::append(g_modScriptPath, kPathSeparator)
            || !core::path::append(g_modScriptPath, lookup.name)) {
            continue;
        }
        lookup.status = read_file(g_modScriptPath, *lookup.output);
        if (lookup.status != SourceStatus::missing) {
            *lookup.modId = record.manifest.id.bytes;
            std::array<char, state::mods::kIdCapacity + sizeof("mod=")> fields{};
            const int written = std::snprintf(
                fields.data(), fields.size(), "mod=%s", record.manifest.id.bytes.data());
            log_line(core::log::Level::debug,
                     nullptr,
                     "source",
                     lookup.status == SourceStatus::ready ? "ok" : "fail",
                     written > 0 ? std::string_view(fields.data()) : std::string_view{});
            return true;
        }
    }
    return true;
}

/** One search path carried through the mod list. */
struct ModSearchRoots final {
    core::path::Buffer* path{};
    bool complete{true};
};

/**
 * Adds each loaded mod's `sdk` in load order, then its `scripts` in reverse load order.
 * Mod SDK modules follow the generated ones, so a mod cannot shadow a generated module.
 * @return Always true; `complete` is false when a root did not fit.
 */
[[nodiscard]] bool append_mod_roots(void* opaque, const state::mods::View& view) noexcept {
    auto& roots = *static_cast<ModSearchRoots*>(opaque);
    core::path::Buffer& path = *roots.path;
    for (std::size_t index = 0; index < view.loadedCount; ++index) {
        const state::mods::Record& record = view.records[index];
        if (record.hasSdk) {
            roots.complete = roots.complete && append_mod_directory(path, view, record)
                             && core::path::append(path, kPathSeparator)
                             && core::path::append(path, state::mods::kSdkFolder)
                             && core::path::append(path, kLuaModulePattern)
                             && core::path::append(path, kSearchRootSeparator);
        }
    }
    for (std::size_t index = view.loadedCount; index-- > 0;) {
        const state::mods::Record& record = view.records[index];
        if (record.hasScripts) {
            roots.complete = roots.complete && append_mod_directory(path, view, record)
                             && core::path::append(path, kPathSeparator)
                             && core::path::append(path, state::mods::kScriptsFolder)
                             && core::path::append(path, kLuaModulePattern)
                             && core::path::append(path, kSearchRootSeparator);
        }
    }
    return true;
}

} // namespace

/**
 * Appends `<mods>\<folder>` for one mod record to a path.
 * @return False when the path does not fit.
 */
bool append_mod_directory(core::path::Buffer& path,
                          const state::mods::View& view,
                          const state::mods::Record& record) noexcept {
    std::array<wchar_t, state::mods::kFolderCapacity> folder{};
    const std::string_view text = record.folder.view();
    std::copy(text.begin(), text.end(), folder.begin());
    return core::path::append(path, view.modsDirectory) && core::path::append(path, kPathSeparator)
           && core::path::append(path, {folder.data(), text.size()});
}

/** Reads the activity's script: a loaded mod's, then the authored one, then the generated one. */
SourceStatus read_source(const sdk::Catalog& catalog,
                         const format::Activity& activity,
                         std::span<const char>& output,
                         std::array<char, state::mods::kIdCapacity>& modId) noexcept {
    modId = {};
    std::array<char, 260> authoredName{};
    if (!controller_name(catalog, activity, authoredName)) {
        return SourceStatus::fileError;
    }
    std::array<wchar_t, 260> wideName{};
    const int wideLength = MultiByteToWideChar(CP_UTF8,
                                               MB_ERR_INVALID_CHARS,
                                               authoredName.data(),
                                               -1,
                                               wideName.data(),
                                               static_cast<int>(wideName.size()));
    // Use backslashes; a root with the `\\?\` prefix does not accept `/`.
    std::replace(wideName.begin(), wideName.end(), L'/', L'\\');
    if (wideLength <= 1) {
        return SourceStatus::fileError;
    }
    ModControllerLookup lookup{std::wstring_view(wideName.data()), &output, &modId};
    if (state::mods::visit(&read_mod_controller, &lookup)
        && lookup.status != SourceStatus::missing) {
        return lookup.status;
    }
    core::path::Buffer authoredPath = g_scriptRoot;
    if (!core::path::append(authoredPath, kPathSeparator)
        || !core::path::append(authoredPath, wideName.data())) {
        return SourceStatus::fileError;
    }
    const SourceStatus authored = read_file(authoredPath, output);
    if (authored != SourceStatus::missing) {
        return authored;
    }

    core::path::Buffer generatedPath = g_scriptRoot;
    std::array<wchar_t, 96> generatedName{};
    const int generatedLength = _snwprintf_s(generatedName.data(),
                                             generatedName.size(),
                                             _TRUNCATE,
                                             L"\\activities\\a_%04u_%08x.lua",
                                             activity.activityIndex,
                                             activity.definitionHash);
    if (generatedLength <= 0
        || !core::path::append(generatedPath,
                               {generatedName.data(), static_cast<std::size_t>(generatedLength)})) {
        return SourceStatus::fileError;
    }
    return read_file(generatedPath, output);
}

/** @return The Lua require path every program opens with. */
const std::array<char, lua_vm::kSearchPathByteCapacity>& sdk_lua_search_path() noexcept {
    return g_sdkLuaSearchPath;
}

/** Leaves the output empty when the view names no activity. */
void controller_file(const sdk::BoundView& view, std::span<char> output) noexcept {
    if (output.empty()) {
        return;
    }
    const format::Activity* const activity = sdk::bound_activity(view);
    if (activity == nullptr || !controller_name(*view.catalog, *activity, output)) {
        output[0] = '\0';
    }
}

/** Resolves the script root and the SDK Lua search path. Logs its own refusal. */
bool resolve_script_paths() noexcept {
    HMODULE const module = owning_module();
    core::path::Buffer artifact{};
    core::path::Buffer sdkLua{};
    ModSearchRoots modRoots{&sdkLua};
    // Generated modules come first, so a controller cannot shadow one by filename. Mod roots
    // sit between the generated modules and the base scripts.
    const bool resolved =
        module != nullptr && core::path::artifact_directory(module, artifact)
        && core::path::assign(g_scriptRoot, {artifact.chars.data(), artifact.length})
        && core::path::append(g_scriptRoot, L"\\scripts")
        && core::path::append(sdkLua, {artifact.chars.data(), artifact.length})
        && core::path::append(sdkLua, L"\\sdk\\lua\\?.lua;");
    if (resolved) {
        // No published mod list means no mods, so a failed visit does not fail the paths.
        (void)state::mods::visit(&append_mod_roots, &modRoots);
    }
    if (!resolved || !modRoots.complete
        || !core::path::append(sdkLua, {g_scriptRoot.chars.data(), g_scriptRoot.length})
        || !core::path::append(sdkLua, kLuaModulePattern)) {
        log_line(core::log::Level::warn, nullptr, "initialize", "path_error");
        return false;
    }
    const int sdkLuaBytes = WideCharToMultiByte(CP_UTF8,
                                                WC_ERR_INVALID_CHARS,
                                                sdkLua.chars.data(),
                                                -1,
                                                g_sdkLuaSearchPath.data(),
                                                static_cast<int>(g_sdkLuaSearchPath.size()),
                                                nullptr,
                                                nullptr);
    if (sdkLuaBytes <= 1) {
        g_sdkLuaSearchPath = {};
        log_line(core::log::Level::warn, nullptr, "initialize", "sdk_lua_path_error");
        return false;
    }
    return true;
}

/** Zeroes the bytes `read_source` returned, once the program no longer needs them. */
void clear_source(std::span<const char> source) noexcept {
    const std::size_t bytes = (std::min)(source.size(), g_source.size());
    std::fill_n(g_source.begin(), bytes, '\0');
}

/** Clears the script buffer and both paths. */
void clear_script_sources() noexcept {
    std::fill(g_source.begin(), g_source.end(), '\0');
    g_scriptRoot = {};
    g_sdkLuaSearchPath = {};
}

} // namespace sunrise::server::activity::mission
