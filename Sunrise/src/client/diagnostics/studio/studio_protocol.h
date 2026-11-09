#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::client::diagnostics::studio {

/** The local mapping uses the ASCII magic SSLV and protocol version 1. */
inline constexpr std::uint32_t kMagic = 0x564C5353;
inline constexpr std::uint32_t kVersion = 1;
/** A connection expires after three seconds without an editor heartbeat. */
inline constexpr std::uint64_t kHeartbeatTimeout = 3000;
/** Disconnected clients check for a requested connection once per second. */
inline constexpr std::uint64_t kConnectInterval = 1000;
/** The editor and frame publisher communicate only within the local Windows session. */
inline constexpr wchar_t kMappingFormat[] = L"Local\\SunriseStudio.Live.%lu";
/** Protocol version 1 fixes the size and byte offsets used by the editor. */
inline constexpr std::size_t kFrameBytes = 128;
inline constexpr std::size_t kSequenceOffset = 24;
inline constexpr std::size_t kFrameTimeOffset = 32;
inline constexpr std::size_t kPlayerOffset = 56;
inline constexpr std::size_t kHorizontalFovOffset = 104;
inline constexpr std::size_t kPlayerHandleOffset = 112;

enum SnapshotFlags : std::uint32_t {
    inWorld = 1,
    playerPresent = 2,
    cameraPresent = 4,
    sliceAvailable = 8,
    slicePresent = 16,
    playerHandlePresent = 32,
};

/** All fields use little-endian fixed-width values; sequence covers only the published data. */
struct alignas(8) SharedSnapshot final {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t bytes{};
    std::uint32_t editorPid{};
    alignas(8) volatile std::int64_t heartbeat{};
    alignas(4) volatile std::int32_t sequence{};
    std::uint32_t reserved{};
    std::uint64_t frameTime{};
    std::uint32_t gamePid{};
    std::uint32_t flags{};
    std::int32_t currentSlice{-1};
    std::uint32_t reserved2{};
    std::array<float, 3> player{};
    std::array<float, 3> cameraPosition{};
    std::array<float, 3> cameraForward{};
    std::array<float, 3> cameraUp{};
    float horizontalFov{};
    float aspect{};
    /** Full handle of the object the local player controls; valid with playerHandlePresent. */
    std::uint32_t playerHandle{};
    std::array<std::byte, 12> padding{};
};

static_assert(sizeof(SharedSnapshot) == kFrameBytes);
static_assert(offsetof(SharedSnapshot, sequence) == kSequenceOffset);
static_assert(offsetof(SharedSnapshot, frameTime) == kFrameTimeOffset);
static_assert(offsetof(SharedSnapshot, player) == kPlayerOffset);
static_assert(offsetof(SharedSnapshot, horizontalFov) == kHorizontalFovOffset);
static_assert(offsetof(SharedSnapshot, playerHandle) == kPlayerHandleOffset);

} // namespace sunrise::client::diagnostics::studio
