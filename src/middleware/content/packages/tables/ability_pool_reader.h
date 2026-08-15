#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::middleware::content::packages::tables::abilities {

/** No shipped socket entry list declares more entries than this. */
inline constexpr std::size_t kEntryCapacity = 64;
/** No shipped pool variant declares more records than this. */
inline constexpr std::size_t kPoolRecordCapacity = 64;
/** A category or link byte is empty at all bits set. */
inline constexpr std::uint8_t kEmptyByte = 0xFF;
/** The engine's no-hash sentinel marks an entry with no plug source of its own. */
inline constexpr std::uint32_t kNoPlugSource = 0x811C9DC5U;

/** One socket-entry-list entry, as the ability bucket walk reads it. */
struct Entry {
    /** Plug source hash, or the no-hash sentinel when the entry supplies none. */
    std::uint32_t plugSource{kNoPlugSource};
    /** Entries in one group are alternatives. Only one member is active. */
    std::uint8_t group{kEmptyByte};
    std::uint8_t kind{kEmptyByte};
    std::uint32_t poolTag{};
    bool hasSelector{};
    std::uint8_t selector{kEmptyByte};
};

/** One 16-byte pool record. It is a definition hash or a link to another entry's pool. */
struct PoolRecord {
    std::uint32_t definitionHash{kNoPlugSource};
    /** Bucket category this hash belongs to, or empty when it belongs to the overflow bank. */
    std::uint8_t category{kEmptyByte};
    std::uint8_t linkEntry{kEmptyByte};
    std::uint8_t linkSubgroup{kEmptyByte};
    std::uint8_t linkElement{kEmptyByte};
    /** Bucket kind this record's entry declares. */
    std::uint8_t kind{kEmptyByte};
    /** Bucket the record lands in, or empty when the record links on to another entry. */
    std::uint8_t destination{kEmptyByte};
};

/**
 * Reads every entry of one socket-entry-list definition blob.
 * @param blob Whole socket-entry-list definition bytes.
 * @param output Entry storage.
 * @return The number of entries read.
 */
[[nodiscard]] std::size_t read_entries(std::span<const std::byte> blob,
                                       std::span<Entry> output) noexcept;

/**
 * Reads the active records of one entry's pool blob.
 * A pool declares several variants. Which is active depends on the group count and on whether
 * the entry carries a selector, so the entry is needed to read its own pool.
 * @param blob Whole pool bytes.
 * @param entry Entry naming the pool.
 * @param subgroup Pool subgroup ordinal.
 * @param output Record storage.
 * @return The number of records read.
 */
[[nodiscard]] std::size_t read_pool_records(std::span<const std::byte> blob,
                                            const Entry& entry,
                                            std::uint8_t subgroup,
                                            std::span<PoolRecord> output) noexcept;

} // namespace sunrise::middleware::content::packages::tables::abilities
