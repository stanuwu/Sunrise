#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "../internal.h"

namespace sunrise::steam::interfaces::methods {
namespace {

/** The default branch name. Content selection reads it. */
constexpr std::string_view kBetaName = "public";
/** There are no launch parameters. The value must be an empty string, never null. */
constexpr char kEmptyQueryParam[] = "";
/** Build id reported when no store backend gives one. */
constexpr int kBuildId = 10;
/** The classic Windows path limit, including the null. */
constexpr DWORD kPathCapacity = 260;

/**
 * Finds the directory holding the running executable.
 * @param output Receives a null-terminated path.
 * @param capacity Caller storage size, including the null.
 * @return Characters written without the null, or zero on failure.
 */
[[nodiscard]] std::uint32_t executable_directory(char* output, std::uint32_t capacity) noexcept {
    char path[kPathCapacity]{};
    const DWORD length = GetModuleFileNameA(nullptr, path, kPathCapacity);
    if (length == 0 || length >= kPathCapacity) {
        return 0;
    }
    std::string_view view(path, length);
    const std::size_t separator = view.find_last_of("\\/");
    if (separator == std::string_view::npos) {
        return 0;
    }
    view = view.substr(0, separator);
    if (output == nullptr || capacity <= view.size()) {
        return 0;
    }
    std::copy(view.begin(), view.end(), output);
    output[view.size()] = '\0';
    return static_cast<std::uint32_t>(view.size());
}

} // namespace

/**
 * @param name Receives the branch name.
 * @param capacity Caller storage size, including the null.
 * @return True when the branch name fits.
 */
bool current_beta_name([[maybe_unused]] void* self, char* name, int capacity) noexcept {
    if (name == nullptr || capacity <= static_cast<int>(kBetaName.size())) {
        return false;
    }
    std::copy(kBetaName.begin(), kBetaName.end(), name);
    name[kBetaName.size()] = '\0';
    return true;
}

/**
 * Reports the directory the game runs from. Its content lives there too.
 * @param appId Ignored. There is one local install.
 * @param folder Receives the install path.
 * @param capacity Caller storage size, including the null.
 * @return Path length without the null, or zero when it does not fit.
 */
std::uint32_t app_install_dir([[maybe_unused]] void* self,
                              [[maybe_unused]] DWORD appId,
                              char* folder,
                              std::uint32_t capacity) noexcept {
    return executable_directory(folder, capacity);
}

/** @return Empty string. A null return would fault a caller that reads the result. */
const char* launch_query_param([[maybe_unused]] void* self,
                               [[maybe_unused]] const char* key) noexcept {
    return kEmptyQueryParam;
}

/** @return Stable build id for this install. */
int app_build_id([[maybe_unused]] void* self) noexcept {
    return kBuildId;
}

} // namespace sunrise::steam::interfaces::methods
