#pragma once

#include <cstdint>

#include "../../../../../core/filesystem/path.h"
#include "../../../definition.h"
#include "../../records/domains.h"

namespace sunrise::state::build_data::cache::temporary {

/**
 * Writes one whole cache under a new sibling name only this writer owns.
 * @param finalPath Null-terminated final cache path.
 * @param build Complete build identity.
 * @param domains Complete sorted mapping domains.
 * @param checksum Payload checksum computed earlier, stored in the header.
 * @param temporaryPath Receives the written sibling path on success.
 * @return True when one unique temporary file is complete and closed.
 */
[[nodiscard]] bool write(const wchar_t* finalPath,
                         const BuildIdentity& build,
                         records::Domains domains,
                         std::uint64_t checksum,
                         core::path::Buffer& temporaryPath) noexcept;

} // namespace sunrise::state::build_data::cache::temporary
