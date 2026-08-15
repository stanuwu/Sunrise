#include "game_host_classification.h"

#include <Windows.h>

#include <array>
#include <string_view>

namespace sunrise::client::runtime::host {
namespace {

/** The production game host requires the early game-owned network hook group. */
constexpr std::wstring_view kGameExecutableName{L"destiny2.exe"};
/** Both Windows path separators may precede the executable basename. */
constexpr std::wstring_view kPathSeparators{L"\\/"};
/** Windows extended paths allow 32767 characters plus a null. */
constexpr DWORD kMaximumExtendedPathCharacters = 32768;

} // namespace

/** Classifies an executable path against the supported game host name. */
NetworkRequirement classify(std::wstring_view executablePath) noexcept {
    if (executablePath.empty()) {
        return NetworkRequirement::required;
    }
    const std::size_t separator = executablePath.find_last_of(kPathSeparators);
    const std::wstring_view filename = separator == std::wstring_view::npos
                                           ? executablePath
                                           : executablePath.substr(separator + 1);
    if (filename.size() != kGameExecutableName.size()) {
        return NetworkRequirement::notApplicable;
    }
    const int comparison = CompareStringOrdinal(filename.data(),
                                                static_cast<int>(filename.size()),
                                                kGameExecutableName.data(),
                                                static_cast<int>(kGameExecutableName.size()),
                                                TRUE);
    if (comparison == CSTR_LESS_THAN || comparison == CSTR_GREATER_THAN) {
        return NetworkRequirement::notApplicable;
    }
    // Equal and failed comparisons both keep game-network readiness fail-closed.
    return NetworkRequirement::required;
}

/** Reads the current executable path and applies the game-host readiness policy. */
NetworkRequirement current_requirement() noexcept {
    std::array<wchar_t, kMaximumExtendedPathCharacters> path{};
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        // Unknown hosts are required so an identity lookup failure cannot bypass readiness.
        return NetworkRequirement::required;
    }
    return classify(std::wstring_view(path.data(), length));
}

} // namespace sunrise::client::runtime::host
