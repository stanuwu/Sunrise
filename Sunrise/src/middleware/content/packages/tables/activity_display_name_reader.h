#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "../schema/generated/tag_classes_80805e.h"
#include "../schema/generated/tag_classes_80805f.h"
#include "../schema/generated/tag_classes_80809a.h"

namespace sunrise::middleware::content::packages::tables::activity_display_names {

/** Class of the installed client activity table. */
inline constexpr std::uint32_t kActivityClientTableClass =
    schema::generated::ActivityClientTable::kClass;
/** Class of the installed string-bank index. */
inline constexpr std::uint32_t kStringBankIndexClass = schema::generated::StringBankIndex::kClass;
/** Per-bank container class. */
inline constexpr std::uint32_t kStringContainerClass = schema::generated::SLocalizedStrings::kClass;
/** Per-language string-data class. */
inline constexpr std::uint32_t kLanguageDataClass =
    schema::generated::SLocalizedStringsData::kClass;
/** Generated SDK display text must fit this storage including its terminator. */
inline constexpr std::size_t kDisplayNameCapacity = 256;

/** One exact English display name or an authored reference with no English bank entry. */
struct Name final {
    std::array<char, kDisplayNameCapacity> value{};
    std::uint16_t length{};
    std::uint32_t bankIndex{};
    std::uint32_t stringHash{};
    bool authoredEmpty{};
};

/** Complete dynamically read activity-name result. */
struct Snapshot final {
    std::vector<Name> names{};
    std::uint32_t resolvedCount{};
    std::uint32_t authoredEmptyCount{};
};

/** One direct localized-string reference carried by authored activity content. */
struct Reference final {
    std::uint32_t containerTag{};
    std::uint32_t stringHash{};
};

/** Callback that reads one installed tag and verifies its class. */
using TagReader = bool (*)(void* context,
                           std::uint32_t tag,
                           std::uint32_t expectedClass,
                           std::vector<std::byte>& output) noexcept;

/** Injected installed-content source used by live and synthetic readers. */
struct Source final {
    void* context{};
    TagReader read{};
    /** Unique installed tag found by scanning kActivityClientTableClass. */
    std::uint32_t activityTableTag{};
    /** Unique installed tag found by scanning kStringBankIndexClass. */
    std::uint32_t stringBankIndexTag{};
};

/**
 * Reads every activity display reference and resolves exact English strings by bank and hash.
 * Missing banks, duplicate hashes, malformed UTF-8, or formatted strings fail the whole result.
 * A reference absent from an otherwise valid English bank is an authored empty name.
 * @param source Installed or synthetic tag source.
 * @param definitionHashes Activity-definition hashes in activity-index order.
 * @param output Receives only a complete, exact result.
 * @return True when every available authored reference resolves exactly.
 */
[[nodiscard]] bool build(const Source& source,
                         std::span<const std::uint32_t> definitionHashes,
                         Snapshot& output) noexcept;

/**
 * Resolves direct English container/hash references without an activity-table join.
 * Unsupported formatted references remain authored-empty so independent literal rows survive.
 */
[[nodiscard]] bool
resolve(const Source& source, std::span<const Reference> references, Snapshot& output) noexcept;

/**
 * Finds the string container that holds each wanted hash. The lowest container tag holding a hash
 * wins, so one install always gives the same answer; an unreadable container is skipped.
 * @param containerTags Installed string containers, in any order.
 * @param stringHashes Hashes to find, ascending without repeats.
 * @param output Receives one reference per located hash, in hash order.
 * @return False only on bad input or allocation failure.
 */
[[nodiscard]] bool locate(const Source& source,
                          std::span<const std::uint32_t> containerTags,
                          std::span<const std::uint32_t> stringHashes,
                          std::vector<Reference>& output) noexcept;

} // namespace sunrise::middleware::content::packages::tables::activity_display_names
