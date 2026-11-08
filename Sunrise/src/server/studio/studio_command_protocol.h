#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace sunrise::server::studio::command_wire {

/** Commands use their own local mapping and the ASCII magic SSCM. */
inline constexpr std::uint32_t kMagic = 0x4D435353;
/** Version 2 adds command kinds and keeps the version 1 layout, so both are served. */
inline constexpr std::uint32_t kVersion = 2;
inline constexpr std::uint32_t kOldestVersion = 1;
inline constexpr wchar_t kMappingFormat[] = L"Local\\SunriseStudio.Command.%lu";
/** Version one fixes the whole mapping at this size. */
inline constexpr std::size_t kMappingBytes = 320;
/** The longest mod id, its null, and padding to an 8-byte boundary. */
inline constexpr std::size_t kModIdBytes = 72;
inline constexpr std::size_t kMessageBytes = 192;
/** Studio and the game address these words at fixed offsets. */
inline constexpr std::size_t kHeartbeatOffset = 16, kRequestOffset = 24, kResponseOffset = 28;
inline constexpr std::size_t kModIdOffset = 48, kMessageOffset = 120;
/** Reply word offsets Studio mirrors; version 2 keeps them where version 1 had them. */
inline constexpr std::size_t kKindOffset = 32, kStatusOffset = 36, kGamePidOffset = 40;
inline constexpr std::size_t kReloadedOffset = 44;
/** A missing editor is looked for once a second. */
inline constexpr std::uint64_t kConnectIntervalMs = 1'000;
/** An editor that has not refreshed its heartbeat this long is gone. */
inline constexpr std::uint64_t kHeartbeatTimeoutMs = 3'000;

/** Kinds after reloadMod are version 2; the tag kinds carry a manifest. */
enum class Kind : std::uint32_t {
    none,
    reloadMod,
    reloadTags,
    patchTags,
    reloadBubble,
    queryResident
};

enum class Status : std::uint32_t { none, ok, invalid, disabled, notLoaded, compileError, failed };

/** Studio writes the request, then its id; the game writes the reply, then the response id. */
struct alignas(8) SharedCommand final {
    std::uint32_t magic{};
    std::uint32_t version{};
    std::uint32_t bytes{};
    std::uint32_t editorPid{};
    alignas(8) volatile std::int64_t heartbeat{};
    alignas(4) volatile std::int32_t request{};
    alignas(4) volatile std::int32_t response{};
    Kind kind{};
    Status status{};
    std::uint32_t gamePid{};
    std::uint32_t reloaded{};
    std::array<char, kModIdBytes> modId{};
    std::array<char, kMessageBytes> message{};
    std::array<std::uint8_t, 8> reserved{};
};

static_assert(offsetof(SharedCommand, kind) == kKindOffset);
static_assert(offsetof(SharedCommand, status) == kStatusOffset);
static_assert(offsetof(SharedCommand, gamePid) == kGamePidOffset);
static_assert(offsetof(SharedCommand, reloaded) == kReloadedOffset);
static_assert(offsetof(SharedCommand, heartbeat) == kHeartbeatOffset);
static_assert(offsetof(SharedCommand, request) == kRequestOffset);
static_assert(offsetof(SharedCommand, response) == kResponseOffset);
static_assert(offsetof(SharedCommand, modId) == kModIdOffset);
static_assert(offsetof(SharedCommand, message) == kMessageOffset);
static_assert(sizeof(SharedCommand) == kMappingBytes);

} // namespace sunrise::server::studio::command_wire
