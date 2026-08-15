#include "egress_policy_logging.h"

#include <array>
#include <cstdint>
#include <cstdio>
#include <string_view>

#include "../../../../core/logging/log.h"

namespace sunrise::client::hooks::egress::policy {
namespace {

/** A missing original denies an otherwise local connect. */
constexpr std::string_view kConnectDenyRedirect =
    "ev=egress stage=connect target=redirect result=deny";
/** Invalid address families stay outside the local route. */
constexpr std::string_view kConnectDenyOther = "ev=egress stage=connect target=other result=deny";
/** A missing original denies an otherwise local send. */
constexpr std::string_view kSendDenyRedirect = "ev=egress stage=send target=redirect result=deny";
/** Invalid address families stay outside the local route. */
constexpr std::string_view kSendDenyOther = "ev=egress stage=send target=other result=deny";
/** Fixed name-event storage keeps stack use small. */
constexpr std::size_t kNameEventCapacity = 448;
/** Names longer than this are cut; no resolver accepts more. */
constexpr std::size_t kNameLimit = 255;

/** Owning module and offset of a caller, so a hook names the code that asked. */
struct CallerSite final {
    std::array<char, 32> module{};
    std::uintptr_t offset{};
};

/** @param caller Return address captured at the replacement entry. @return Its owning site. */
[[nodiscard]] CallerSite inspect_caller(const void* caller) noexcept {
    CallerSite site{};
    HMODULE owner{};
    if (caller == nullptr
        || GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                                  | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(caller),
                              &owner)
               == FALSE) {
        return site;
    }
    site.offset =
        reinterpret_cast<std::uintptr_t>(caller) - reinterpret_cast<std::uintptr_t>(owner);
    std::array<wchar_t, MAX_PATH> path{};
    const DWORD length = GetModuleFileNameW(owner, path.data(), static_cast<DWORD>(path.size()));
    std::size_t start = 0;
    for (DWORD index = 0; index < length; ++index) {
        if (path[index] == L'\\' || path[index] == L'/') {
            start = index + 1;
        }
    }
    std::size_t written = 0;
    for (DWORD index = static_cast<DWORD>(start);
         index < length && written + 1 < site.module.size();
         ++index) {
        const wchar_t value = path[index];
        site.module[written++] = value > 0 && value < 128 ? static_cast<char>(value) : '?';
    }
    return site;
}

/** @param operation Name or provider category. @return Its fixed stage token. */
[[nodiscard]] std::string_view name_stage(NameOperation operation) noexcept {
    if (operation == NameOperation::resolve) {
        return "resolve";
    }
    if (operation == NameOperation::reverse) {
        return "reverse";
    }
    return operation == NameOperation::dns ? "dns" : "control";
}

} // namespace

/** Logs one fixed event for a refused call, with no endpoint or payload data. */
void log_decision(SocketOperation operation, bool targetsRedirect, bool allowed) noexcept {
    if (allowed) {
        return;
    }
    const std::string_view event =
        operation == SocketOperation::connect
            ? (targetsRedirect ? kConnectDenyRedirect : kConnectDenyOther)
            : (targetsRedirect ? kSendDenyRedirect : kSendDenyOther);
    core::log::write(core::log::Channel::client, core::log::Level::warn, event);
}

/**
 * Logs one name or provider decision, with the requested name and its selector.
 * @param operation Name or provider category.
 * @param allowed True when the original was called.
 * @param name Borrowed requested name; empty where the entry point has none.
 * @param selector Record type, address family, or control code.
 * @param status Resolver status where the entry point produced one.
 * @param caller Return address captured at the replacement entry.
 */
void log_name_decision(NameOperation operation,
                       bool allowed,
                       std::string_view name,
                       unsigned selector,
                       unsigned status,
                       const void* caller) noexcept {
    const std::string_view stage = name_stage(operation);
    const CallerSite site = inspect_caller(caller);
    const std::size_t nameLength = name.size() < kNameLimit ? name.size() : kNameLimit;
    std::array<char, kNameEventCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=egress stage=%.*s target=redirect action=%s "
                      "selector=0x%X status=0x%X caller=%s+0x%llX name=%.*s result=ok",
                      static_cast<int>(stage.size()),
                      stage.data(),
                      allowed ? "allow" : "deny",
                      selector,
                      status,
                      site.module.data(),
                      static_cast<unsigned long long>(site.offset),
                      static_cast<int>(nameLength),
                      name.data());
    if (written <= 0) {
        return;
    }
    const auto length = static_cast<std::size_t>(written) < line.size()
                            ? static_cast<std::size_t>(written)
                            : line.size() - 1;
    core::log::write(core::log::Channel::client, core::log::Level::debug, {line.data(), length});
}

/**
 * Narrows one wide name into fixed storage before logging the decision.
 * @param operation Name category.
 * @param allowed True when the original was called.
 * @param name Borrowed wide requested name.
 * @param selector Record type, address family, or control code.
 * @param status Resolver status where the entry point produced one.
 * @param caller Return address captured at the replacement entry.
 */
void log_name_decision(NameOperation operation,
                       bool allowed,
                       std::wstring_view name,
                       unsigned selector,
                       unsigned status,
                       const void* caller) noexcept {
    std::array<char, kNameLimit + 1> narrowed{};
    const std::size_t length = name.size() < kNameLimit ? name.size() : kNameLimit;
    for (std::size_t index = 0; index < length; ++index) {
        const wchar_t value = name[index];
        narrowed[index] = value > 0 && value < 128 ? static_cast<char>(value) : '?';
    }
    log_name_decision(
        operation, allowed, std::string_view(narrowed.data(), length), selector, status, caller);
}

} // namespace sunrise::client::hooks::egress::policy
