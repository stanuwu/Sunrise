#pragma once

#include <type_traits>

#include "../dns/egress_dns_replacements.h"
#include "../extensions/egress_extension_replacements.h"
#include "../resolver/replacements.h"
#include "../winsock/replacements.h"

namespace sunrise::client::hooks::egress::platform {

/** Every detour replacement must convert to its exact Windows SDK function type. */
static_assert(
    std::is_convertible_v<decltype(&winsock::connection::connect_socket), decltype(&::connect)>);
static_assert(std::is_convertible_v<decltype(&winsock::connection::connect_socket_ex),
                                    decltype(&::WSAConnect)>);
static_assert(std::is_convertible_v<decltype(&winsock::connection::connect_by_name_a),
                                    decltype(&::WSAConnectByNameA)>);
static_assert(std::is_convertible_v<decltype(&winsock::connection::connect_by_name_w),
                                    decltype(&::WSAConnectByNameW)>);
static_assert(std::is_convertible_v<decltype(&winsock::connection::connect_by_list),
                                    decltype(&::WSAConnectByList)>);
static_assert(
    std::is_convertible_v<decltype(&winsock::transmission::send_bytes), decltype(&::send)>);
static_assert(
    std::is_convertible_v<decltype(&winsock::transmission::send_buffers), decltype(&::WSASend)>);
static_assert(
    std::is_convertible_v<decltype(&winsock::transmission::send_bytes_to), decltype(&::sendto)>);
static_assert(std::is_convertible_v<decltype(&winsock::transmission::send_buffers_to),
                                    decltype(&::WSASendTo)>);
static_assert(
    std::is_convertible_v<decltype(&winsock::transmission::send_message), decltype(&::WSASendMsg)>);
static_assert(std::is_convertible_v<decltype(&winsock::transmission::send_disconnect),
                                    decltype(&::WSASendDisconnect)>);
static_assert(
    std::is_convertible_v<decltype(&winsock::connection::join_leaf), decltype(&::WSAJoinLeaf)>);
static_assert(
    std::is_convertible_v<decltype(&extensions::transmit_file), decltype(&::TransmitFile)>);
static_assert(
    std::is_convertible_v<decltype(&winsock::control::socket_control), decltype(&::WSAIoctl)>);
static_assert(std::is_convertible_v<decltype(&resolver::address_info_a), decltype(&::getaddrinfo)>);
static_assert(
    std::is_convertible_v<decltype(&resolver::address_info_w), decltype(&::GetAddrInfoW)>);
static_assert(
    std::is_convertible_v<decltype(&resolver::address_info_ex_a), decltype(&::GetAddrInfoExA)>);
static_assert(
    std::is_convertible_v<decltype(&resolver::address_info_ex_w), decltype(&::GetAddrInfoExW)>);
static_assert(std::is_convertible_v<decltype(&resolver::host_by_name), decltype(&::gethostbyname)>);
static_assert(
    std::is_convertible_v<decltype(&resolver::host_by_address), decltype(&::gethostbyaddr)>);
static_assert(std::is_convertible_v<decltype(&resolver::name_info_a), decltype(&::getnameinfo)>);
static_assert(std::is_convertible_v<decltype(&resolver::name_info_w), decltype(&::GetNameInfoW)>);
static_assert(std::is_convertible_v<decltype(&dns::query_a), decltype(&::DnsQuery_A)>);
static_assert(std::is_convertible_v<decltype(&dns::query_w), decltype(&::DnsQuery_W)>);
static_assert(std::is_convertible_v<decltype(&dns::query_utf8), decltype(&::DnsQuery_UTF8)>);
static_assert(std::is_convertible_v<decltype(&dns::query_ex), decltype(&::DnsQueryEx)>);
static_assert(
    std::is_convertible_v<decltype(&winsock::reception::receive_bytes), decltype(&::recv)>);
static_assert(
    std::is_convertible_v<decltype(&winsock::reception::receive_buffers), decltype(&::WSARecv)>);
static_assert(std::is_convertible_v<decltype(&winsock::reception::receive_bytes_from),
                                    decltype(&::recvfrom)>);
static_assert(std::is_convertible_v<decltype(&winsock::reception::receive_buffers_from),
                                    decltype(&::WSARecvFrom)>);
static_assert(std::is_convertible_v<decltype(&dns::query_raw), decltype(&::DnsQueryRaw)>);

} // namespace sunrise::client::hooks::egress::platform
