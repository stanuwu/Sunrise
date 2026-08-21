#pragma once

#include <Windows.h>

#include <cstdint>

#include "../../definition.h"
#include "../records/domains.h"
#include "../records/format.h"

namespace sunrise::state::build_data::cache::read {

/**
 * Reads one whole object at the current file position.
 * @tparam Value Trivially copied cache object.
 * @param file Open cache handle.
 * @param value Receives the whole object.
 * @return True when Windows reads every byte.
 */
template <typename Value> [[nodiscard]] bool read_value(HANDLE file, Value& value) noexcept {
    DWORD transferred = 0;
    return ReadFile(file, &value, static_cast<DWORD>(sizeof value), &transferred, nullptr) != FALSE
           && transferred == sizeof value;
}

/** Clears every row in the caller's storage after a failed load. */
void clear(records::MutableDomains output) noexcept;

/**
 * Computes the exact cache file size for every record array.
 * @param counts Header row counts.
 * @param size Receives the exact byte count.
 * @return True when the total size fits.
 */
[[nodiscard]] bool expected_size(const records::DomainCounts& counts, std::uint64_t& size) noexcept;

/**
 * Reads, checksums, and checks every payload array.
 * @param file Open cache handle, positioned after its header.
 * @param build Cache build identity.
 * @param constants Header constants, held until the whole load commits.
 * @param counts Checked row counts.
 * @param output Fixed caller storage for every domain.
 * @param checksum Receives the checksum over the packed record bytes.
 * @return True when all records decode and pass their checks together.
 */
[[nodiscard]] bool read_payload(HANDLE file,
                                const BuildIdentity& build,
                                const records::InvestmentConstants& constants,
                                const records::DomainCounts& counts,
                                records::MutableDomains output,
                                std::uint64_t& checksum) noexcept;

} // namespace sunrise::state::build_data::cache::read
