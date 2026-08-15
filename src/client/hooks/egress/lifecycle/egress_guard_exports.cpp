#include "../dns/egress_dns_replacements.h"
#include "../extensions/egress_extension_replacements.h"
#include "../resolver/replacements.h"
#include "../winsock/replacements.h"
#include "internal.h"

namespace sunrise::client::hooks::egress::lifecycle {
namespace {

struct ExportDefinition {
    ModuleSlot module{};
    const char* name{};
    void* replacement{};
};

template <typename Function>
[[nodiscard]] ExportDefinition
export_definition(ModuleSlot module, const char* name, Function replacement) noexcept {
    return ExportDefinition{module, name, reinterpret_cast<void*>(replacement)};
}

} // namespace

/** Finds the whole Windows SDK egress surface for one atomic Detours batch. */
bool resolve_specs(std::span<hooking::detour::Spec, kHookCount> specs,
                   std::span<const char*, kHookCount> names,
                   std::span<bool, kHookCount> resolved,
                   std::size_t& count) noexcept {
    count = 0;
    const std::array<ExportDefinition, kHookCount> exports{
        export_definition(ModuleSlot::winsock, "connect", &winsock::connection::connect_socket),
        export_definition(
            ModuleSlot::winsock, "WSAConnect", &winsock::connection::connect_socket_ex),
        export_definition(
            ModuleSlot::winsock, "WSAConnectByNameA", &winsock::connection::connect_by_name_a),
        export_definition(
            ModuleSlot::winsock, "WSAConnectByNameW", &winsock::connection::connect_by_name_w),
        export_definition(
            ModuleSlot::winsock, "WSAConnectByList", &winsock::connection::connect_by_list),
        export_definition(ModuleSlot::winsock, "send", &winsock::transmission::send_bytes),
        export_definition(ModuleSlot::winsock, "WSASend", &winsock::transmission::send_buffers),
        export_definition(ModuleSlot::winsock, "sendto", &winsock::transmission::send_bytes_to),
        export_definition(
            ModuleSlot::winsock, "WSASendTo", &winsock::transmission::send_buffers_to),
        export_definition(ModuleSlot::winsock, "WSASendMsg", &winsock::transmission::send_message),
        export_definition(
            ModuleSlot::winsock, "WSASendDisconnect", &winsock::transmission::send_disconnect),
        export_definition(ModuleSlot::winsock, "WSAJoinLeaf", &winsock::connection::join_leaf),
        export_definition(ModuleSlot::extensions, "TransmitFile", &extensions::transmit_file),
        export_definition(ModuleSlot::winsock, "WSAIoctl", &winsock::control::socket_control),
        export_definition(ModuleSlot::winsock, "getaddrinfo", &resolver::address_info_a),
        export_definition(ModuleSlot::winsock, "GetAddrInfoW", &resolver::address_info_w),
        export_definition(ModuleSlot::winsock, "GetAddrInfoExA", &resolver::address_info_ex_a),
        export_definition(ModuleSlot::winsock, "GetAddrInfoExW", &resolver::address_info_ex_w),
        export_definition(ModuleSlot::winsock, "gethostbyname", &resolver::host_by_name),
        export_definition(ModuleSlot::winsock, "gethostbyaddr", &resolver::host_by_address),
        export_definition(ModuleSlot::winsock, "getnameinfo", &resolver::name_info_a),
        export_definition(ModuleSlot::winsock, "GetNameInfoW", &resolver::name_info_w),
        export_definition(ModuleSlot::dns, "DnsQuery_A", &dns::query_a),
        export_definition(ModuleSlot::dns, "DnsQuery_W", &dns::query_w),
        export_definition(ModuleSlot::dns, "DnsQuery_UTF8", &dns::query_utf8),
        export_definition(ModuleSlot::dns, "DnsQueryEx", &dns::query_ex),
        export_definition(ModuleSlot::winsock, "recv", &winsock::reception::receive_bytes),
        export_definition(ModuleSlot::winsock, "WSARecv", &winsock::reception::receive_buffers),
        export_definition(ModuleSlot::winsock, "recvfrom", &winsock::reception::receive_bytes_from),
        export_definition(
            ModuleSlot::winsock, "WSARecvFrom", &winsock::reception::receive_buffers_from),
        export_definition(ModuleSlot::dns, "DnsQueryRaw", &dns::query_raw),
    };

    for (std::size_t index = 0; index < exports.size(); ++index) {
        const ExportDefinition& definition = exports[index];
        const HMODULE module = module_handle(definition.module);
        void* const target = reinterpret_cast<void*>(GetProcAddress(module, definition.name));
        names[index] = definition.name;
        resolved[index] = target != nullptr;
        if (target == nullptr) {
            // Older Windows builds cannot expose an egress path through an absent DnsQueryRaw.
            return index == kRequiredHookCount;
        }
        specs[index] = hooking::detour::Spec{target, definition.replacement};
        count = index + 1;
    }
    return count >= kRequiredHookCount;
}

} // namespace sunrise::client::hooks::egress::lifecycle
