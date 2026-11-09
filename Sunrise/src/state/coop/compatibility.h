#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../../middleware/crypto/sha256.h"
#include "../mods/definition.h"

namespace sunrise::state::coop::compatibility {

using Digest = middleware::crypto::sha256::Digest;

/** One loaded mod as a co-op peer must match it. */
struct Mod final {
    mods::Id id{};
    mods::Version version{};
    /** Every file in the mod folder: relative path, size and bytes. */
    Digest files{};
};

/** What two installs must share before they play together. */
struct Record final {
    /** The Sunrise module file, so both sides run the same build. */
    Digest build{};
    /** The client content catalog, installed packages and mod packages together. */
    Digest content{};
    /** The join code, hashed; all zero bytes when none is set. */
    Digest joinCode{};
    /** A cheat mod is loaded or the cheat tools are on. */
    bool cheats{};
    /** Loaded mods sorted by id. Empty in a decoded summary, which carries only `modList`. */
    std::array<Mod, mods::kModCapacity> mods{};
    std::size_t modCount{};
    /** Every mod's id, version and file hash, in id order. */
    Digest modList{};
};

/** The first check a guest failed, in the order the host checks them. */
enum class Mismatch : std::uint8_t {
    none,
    build,
    joinCode,
    cheats,
    mods,
    content,
};

/** Bytes of one encoded summary. */
inline constexpr std::size_t kSummarySize = 4 + 1 + 3 * 32 + 1 + 1 + 32;
/** Bytes one encoded refusal can take, with every mod listed. */
inline constexpr std::size_t kRefusalCapacity =
    4 + 1 + 1 + kSummarySize + mods::kModCapacity * (1 + mods::kIdMaximumLength + 6 + 32);

/**
 * Builds this install's record once at boot. Hashes the module file and every loaded mod's files.
 * @param module This module, whose file names the build.
 * @return False when a file could not be read or hashed.
 */
[[nodiscard]] bool initialize(void* module) noexcept;

/** @return This install's record, or null before `initialize` succeeded. */
[[nodiscard]] const Record* local() noexcept;

/** Writes the summary a guest sends in its hello. @return False when `output` is too small. */
[[nodiscard]] bool
encode_summary(const Record& record, std::span<std::byte> output, std::size_t& written) noexcept;
/** @return True when `input` starts with a whole summary; `output` then holds it, mods empty. */
[[nodiscard]] bool decode_summary(std::span<const std::byte> input, Record& output) noexcept;

/** Writes the host's refusal: the failed check and the host's full record. */
[[nodiscard]] bool encode_refusal(const Record& host,
                                  Mismatch mismatch,
                                  std::span<std::byte> output,
                                  std::size_t& written) noexcept;
/** @return True when `input` is a whole refusal; `host` then holds the host's full record. */
[[nodiscard]] bool
decode_refusal(std::span<const std::byte> input, Mismatch& mismatch, Record& host) noexcept;

/** Bytes of the server-hello field that carries a summary: tag, two-byte varint length, summary. */
inline constexpr std::size_t kHelloFieldSize = 1 + 2 + kSummarySize;
/** Writes the hello field a guest appends after the hello's protocol version. */
[[nodiscard]] bool encode_hello_field(const Record& record,
                                      std::span<std::byte, kHelloFieldSize> output) noexcept;
/**
 * Finds the summary in the bytes that follow a hello's protocol version.
 * @return False for a native hello, which carries no such field.
 */
[[nodiscard]] bool find_hello_field(std::span<const std::byte> tail,
                                    std::span<const std::byte>& summary) noexcept;

/** @return The first check `guest` fails against `host`. */
[[nodiscard]] Mismatch compare(const Record& guest, const Record& host) noexcept;
/** @return A short log name for `mismatch`. */
[[nodiscard]] const char* mismatch_name(Mismatch mismatch) noexcept;
/**
 * Writes one line a player can act on, naming the first difference.
 * @return Characters written, without a terminator.
 */
std::size_t
describe(Mismatch mismatch, const Record& local, const Record& host, std::span<char> output) noexcept;

} // namespace sunrise::state::coop::compatibility