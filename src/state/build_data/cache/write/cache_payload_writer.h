#pragma once

#include <Windows.h>

#include <cstdint>

#include "../records/domains.h"

namespace sunrise::state::build_data::cache::writer {

/**
 * Computes the checksum before the header is written.
 * @param domains Complete sorted mapping domains.
 * @param checksum Receives the checksum over every packed record.
 * @return True when every row has a valid disk form.
 */
[[nodiscard]] bool payload_checksum(records::Domains domains, std::uint64_t& checksum) noexcept;

/**
 * Encodes and writes every record array in the fixed domain order.
 * @param file Open temporary cache handle, positioned after its header.
 * @param domains Complete sorted mapping domains.
 * @return True when every packed record is written in full.
 */
[[nodiscard]] bool write_payload(HANDLE file, records::Domains domains) noexcept;

} // namespace sunrise::state::build_data::cache::writer
