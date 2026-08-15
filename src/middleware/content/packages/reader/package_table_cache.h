#pragma once

#include <cstdint>

#include "internal.h"

namespace sunrise::middleware::content::packages::reader::table_cache {

/**
 * Reports one entry record, holding the package's tables from the first use.
 * Two tables are a few hundred KiB and a pass reads a package hundreds of thousands of times,
 * so holding them removes the per-record read. They live in the reader's scratch, so no locking.
 * @param scratch The reader's own storage.
 * @param path Full package path.
 * @param header Parsed package header.
 * @param index Entry index from the tag handle.
 * @param record Receives the record.
 * @return True when the record is held or read.
 */
[[nodiscard]] bool entry_record(Scratch& scratch,
                                const Path& path,
                                const Header& header,
                                std::uint32_t index,
                                layout::EntryRecord& record) noexcept;

/**
 * Reports one block record, holding the package's tables from the first use.
 * @param scratch The reader's own storage.
 * @param path Full package path.
 * @param header Parsed package header.
 * @param index Block index.
 * @param record Receives the record.
 * @return True when the record is held or read.
 */
[[nodiscard]] bool block_record(Scratch& scratch,
                                const Path& path,
                                const Header& header,
                                std::uint32_t index,
                                layout::BlockRecord& record) noexcept;

} // namespace sunrise::middleware::content::packages::reader::table_cache
