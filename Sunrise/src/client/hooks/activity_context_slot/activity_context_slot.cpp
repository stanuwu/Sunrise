/**
 * Opt-in activity context slot policy.
 *
 * The Client activates each activity context through World_CheckActivityBubbles, whose sixth
 * argument is the context's replicated-record slot. A direct launch passes 0; the Client's own
 * incoming-to-current activity swap passes 1. On slot 0 the type-30 owner gate (0x4E4C20) returns
 * 0, and the player monitor then compares each player's account SOID with the Activity Host
 * session SOID. Those are different IDs by design, so no owner-filtered player monitor ever
 * counts a player. On slot 1 the gate skips that filter.
 *
 * A mission script opts its activity into slot 1 with one header directive (see
 * activity_context_slot_directive.h). The directive is read from the player's own copy of the
 * script, so the policy needs nothing from the Activity Host and applies the same way when the
 * Client points at an external server. Activities whose script does not ask keep the native slot.
 *
 * Slot 1 is not gate-only. The Client also reads it to pick a message domain for two send paths
 * (0x4D2D10: 2 on slot 0, 3 otherwise), to skip one slot-0-only dispatcher handler (0x3CB4A0) and
 * to set one flag (0x17136D0). Before 0.5, Sunrise forced slot 1 for every activity.
 */

#include "activity_context_slot.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../core/filesystem/path.h"
#include "../../../core/logging/log.h"
#include "../../../server/activity/mission/mission_script_runtime.h"
#include "../../../state/activity_sdk/runtime.h"
#include "../../hooking/detour.h"
#include "../../patterns/image_scan.h"
#include "../../patterns/signature_text.h"
#include "activity_context_slot_directive.h"

namespace sunrise::client::hooks::activity_context_slot {
namespace {

using patterns::scan_main_image_unique;
using patterns::signature;
using patterns::signature_length;

/**
 * `World_CheckActivityBubbles`* (RVA 0x3CDB80), matched from its prologue through the activity
 * tag load and the handle shift, which is unique in the image.
 */
constexpr std::string_view kCheckText =
    "48 89 5C 24 ? 48 89 6C 24 ? 48 89 74 24 ? 57 41 56 41 57 48 83 EC ? 48 8B 79 10 41 8B C0 "
    "41 8B D8 C1 F8 0D";
constexpr auto kCheck = signature<signature_length(kCheckText)>(kCheckText);

/** The slot a direct launch passes. Only this value is ever replaced. */
constexpr std::int32_t kDirectSlot = 0;
/** The slot the Client's own activity-swap path passes. */
constexpr std::int32_t kSwappedSlot = 1;

/** Bytes of each script read. The directive must sit in the leading comment lines. */
constexpr std::size_t kHeaderBytes = 8192;
/** Distinct controller scripts compared for one scenario tag. */
constexpr std::size_t kStemCapacity = 8;
constexpr std::size_t kStemBytes = 120;
constexpr std::size_t kControllerNameBytes = 260;
/** Decision lines per run. The check runs once per activity context activation. */
constexpr std::uint32_t kReportLimit = 16;

using Check =
    std::uint8_t(__fastcall*)(void*, void*, std::int32_t, void*, std::int64_t, std::int32_t);

using Stem = std::array<char, kStemBytes>;

enum class Reason : std::uint8_t {
    declared,
    notRequested,
    noScript,
    notInCatalog,
    noCatalog,
    invalidDirective,
    conflict,
    readError,
    capacity,
};

struct Decision final {
    Reason reason{Reason::noCatalog};
    /** The first script consulted, for the log line. */
    Stem stem{};
    std::uint32_t scripts{};
};

hooking::detour::Handle g_handle{};
/** Serializes decisions, which share the path and header buffers below. */
SRWLOCK g_lock{SRWLOCK_INIT};
std::atomic_bool g_installed{false};
std::atomic_uint32_t g_reports{};
core::path::Buffer g_scriptRoot{};

[[nodiscard]] const char* reason_name(Reason value) noexcept {
    switch (value) {
    case Reason::declared:
        return "declared";
    case Reason::notRequested:
        return "not_requested";
    case Reason::noScript:
        return "no_script";
    case Reason::notInCatalog:
        return "not_in_catalog";
    case Reason::noCatalog:
        return "no_catalog";
    case Reason::invalidDirective:
        return "invalid_directive";
    case Reason::conflict:
        return "conflict";
    case Reason::readError:
        return "read_error";
    case Reason::capacity:
        return "capacity";
    }
    return "unknown";
}

/** @return The Sunrise module, found from this function's own address. */
[[nodiscard]] HMODULE owning_module() noexcept {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&owning_module),
                           &module)
        == FALSE) {
        return nullptr;
    }
    return module;
}

enum class ReadStatus : std::uint8_t { ready, missing, error };

/** Reads the first kHeaderBytes of one script. */
[[nodiscard]] ReadStatus read_header(const core::path::Buffer& path,
                                     std::array<char, kHeaderBytes>& buffer,
                                     std::string_view& output,
                                     bool& wholeFile) noexcept {
    output = {};
    wholeFile = false;
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
                   ? ReadStatus::missing
                   : ReadStatus::error;
    }
    LARGE_INTEGER size{};
    DWORD read = 0;
    const bool loaded = GetFileSizeEx(file, &size) != FALSE && size.QuadPart >= 0
                        && ReadFile(file,
                                    buffer.data(),
                                    static_cast<DWORD>(buffer.size()),
                                    &read,
                                    nullptr)
                               != FALSE;
    CloseHandle(file);
    if (!loaded) {
        return ReadStatus::error;
    }
    output = {buffer.data(), read};
    wholeFile = static_cast<unsigned long long>(size.QuadPart) <= read;
    return ReadStatus::ready;
}

/** Appends `\<stem>\<stem>.lua` or `\<stem>.lua`. The stem is ASCII by construction. */
[[nodiscard]] bool
script_path(std::string_view stem, bool nested, core::path::Buffer& output) noexcept {
    std::array<wchar_t, kStemBytes> wide{};
    if (stem.empty() || stem.size() >= wide.size()) {
        return false;
    }
    std::transform(stem.begin(), stem.end(), wide.begin(), [](char value) noexcept {
        return static_cast<wchar_t>(static_cast<unsigned char>(value));
    });
    const std::wstring_view name(wide.data(), stem.size());
    output = g_scriptRoot;
    return core::path::append(output, L"\\") && core::path::append(output, name)
           && (!nested
               || (core::path::append(output, L"\\") && core::path::append(output, name)))
           && core::path::append(output, L".lua");
}

/**
 * Reads the directive of one controller, trying the nested `<stem>/<stem>.lua` layout first and
 * the flat pre-0.5 `<stem>.lua` layout second, the same order the Activity Host loads them.
 */
[[nodiscard]] ReadStatus read_controller(std::string_view stem, Request& output) noexcept {
    output = Request::none;
    // Too large for the game thread's stack; g_lock serializes every use.
    static core::path::Buffer path{};
    static std::array<char, kHeaderBytes> buffer{};
    for (const bool nested : {true, false}) {
        if (!script_path(stem, nested, path)) {
            return ReadStatus::error;
        }
        std::string_view header{};
        bool wholeFile = false;
        const ReadStatus status = read_header(path, buffer, header, wholeFile);
        if (status == ReadStatus::missing) {
            continue;
        }
        if (status == ReadStatus::ready) {
            output = read_directive(header, wholeFile);
        }
        return status;
    }
    return ReadStatus::missing;
}

/**
 * Resolves every catalog activity bound to one scenario tag to its controller script and
 * combines their directives. Slot 1 is used only when every authored script found asks for it.
 */
[[nodiscard]] Decision decide(std::uint32_t scenarioTag) noexcept {
    Decision decision{};
    const state::activity_sdk::Snapshot catalog = state::activity_sdk::snapshot();
    if (catalog == nullptr || g_scriptRoot.length == 0) {
        return decision;
    }
    std::array<Stem, kStemCapacity> seen{};
    std::size_t seenCount = 0;
    bool matched = false;
    bool anySwapped = false;
    bool anyOther = false;
    const auto activities = catalog->activities();
    for (std::size_t row = 0; row < activities.size(); ++row) {
        if (activities[row].selectedScenarioTag != scenarioTag) {
            continue;
        }
        matched = true;
        std::array<char, kControllerNameBytes> name{};
        if (!server::activity::mission::controller_file_name(static_cast<std::uint32_t>(row + 1),
                                                             name)) {
            continue;
        }
        const std::string_view full(name.data());
        const std::string_view stem = full.substr(0, full.find('/'));
        if (stem.empty() || stem.size() >= kStemBytes) {
            continue;
        }
        const bool known =
            std::any_of(seen.begin(), seen.begin() + seenCount, [stem](const Stem& value) {
                return std::string_view(value.data()) == stem;
            });
        if (known) {
            continue;
        }
        if (seenCount == seen.size()) {
            decision.reason = Reason::capacity;
            return decision;
        }
        std::copy(stem.begin(), stem.end(), seen[seenCount].begin());
        ++seenCount;
        Request request = Request::none;
        const ReadStatus status = read_controller(stem, request);
        if (status == ReadStatus::missing) {
            continue;
        }
        if (decision.scripts++ == 0) {
            decision.stem = seen[seenCount - 1];
        }
        if (status == ReadStatus::error) {
            decision.reason = Reason::readError;
            decision.stem = seen[seenCount - 1];
            return decision;
        }
        if (request == Request::invalid) {
            decision.reason = Reason::invalidDirective;
            decision.stem = seen[seenCount - 1];
            return decision;
        }
        (request == Request::swapped ? anySwapped : anyOther) = true;
    }
    if (!matched) {
        decision.reason = Reason::notInCatalog;
    } else if (decision.scripts == 0) {
        decision.reason = Reason::noScript;
    } else if (anySwapped && anyOther) {
        decision.reason = Reason::conflict;
    } else {
        decision.reason = anySwapped ? Reason::declared : Reason::notRequested;
    }
    return decision;
}

void report(std::uint32_t scenarioTag,
            const Decision& decision,
            std::int32_t was,
            std::int32_t now) noexcept {
    if (g_reports.fetch_add(1, std::memory_order_relaxed) >= kReportLimit) {
        return;
    }
    const bool fault = decision.reason == Reason::invalidDirective
                       || decision.reason == Reason::conflict
                       || decision.reason == Reason::readError
                       || decision.reason == Reason::capacity;
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity_context_slot result=%s reason=%s tag=0x%08X "
                                      "script=%s scripts=%u was=%d now=%d",
                                      now == kSwappedSlot ? "swapped" : "native",
                                      reason_name(decision.reason),
                                      scenarioTag,
                                      decision.stem[0] != '\0' ? decision.stem.data() : "-",
                                      decision.scripts,
                                      static_cast<int>(was),
                                      static_cast<int>(now));
    if (written > 0) {
        const auto length = static_cast<std::size_t>(written) < line.size()
                                ? static_cast<std::size_t>(written)
                                : line.size() - 1;
        core::log::write(core::log::Channel::client,
                         fault ? core::log::Level::warn : core::log::Level::info,
                         {line.data(), length});
    }
}

/**
 * Passes every argument through, replacing only a direct-launch slot for an activity whose
 * script asked for the swapped slot. Argument 5 stays as the Client computed it, as it did under
 * the pre-0.5 hook. The return type is the native one-byte result; do not change it.
 */
__declspec(noinline) std::uint8_t __fastcall check(void* context,
                                                   void* reporter,
                                                   std::int32_t activity,
                                                   void* prefix,
                                                   std::int64_t roleIsLocal,
                                                   std::int32_t slot) noexcept {
    const auto original = reinterpret_cast<Check>(g_handle.original);
    if (slot != kDirectSlot) {
        return original(context, reporter, activity, prefix, roleIsLocal, slot);
    }
    const auto scenarioTag = static_cast<std::uint32_t>(activity);
    AcquireSRWLockExclusive(&g_lock);
    const Decision decision = decide(scenarioTag);
    ReleaseSRWLockExclusive(&g_lock);
    const std::int32_t chosen = decision.reason == Reason::declared ? kSwappedSlot : slot;
    report(scenarioTag, decision, slot, chosen);
    return original(context, reporter, activity, prefix, roleIsLocal, chosen);
}

} // namespace

bool install() noexcept {
    if (g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    HMODULE const module = owning_module();
    if (module == nullptr || !core::path::artifact_directory(module, g_scriptRoot)
        || !core::path::append(g_scriptRoot, L"\\scripts")) {
        g_scriptRoot = {};
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=activity_context_slot stage=install result=fail reason=path");
        return false;
    }
    std::byte* const target = scan_main_image_unique(kCheck, "activity_context_slot_check");
    if (target == nullptr) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=activity_context_slot stage=install result=fail reason=target");
        return false;
    }
    g_reports.store(0, std::memory_order_release);
    if (!hooking::detour::install(
            hooking::detour::Spec{target, reinterpret_cast<void*>(&check)}, g_handle)) {
        core::log::write(core::log::Channel::client,
                         core::log::Level::warn,
                         "ev=activity_context_slot stage=install result=fail reason=attach");
        return false;
    }
    g_installed.store(true, std::memory_order_release);
    core::log::write(core::log::Channel::client,
                     core::log::Level::info,
                     "ev=activity_context_slot stage=install result=ok");
    return true;
}

bool uninstall() noexcept {
    if (!g_installed.load(std::memory_order_acquire)) {
        return true;
    }
    if (!hooking::detour::uninstall(g_handle)) {
        return false;
    }
    g_installed.store(false, std::memory_order_release);
    return true;
}

} // namespace sunrise::client::hooks::activity_context_slot
