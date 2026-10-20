#pragma once

#include <cstdint>
#include <span>

#include "../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::vendors {

/**
 * Extracts the vendor catalog from the installed packages, once.
 * The whole index is read, and a definition for every row it names.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage shared with the other content passes.
 * @return True when State already holds the catalog or a full pass publishes it.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch) noexcept;

/** Installed unlock-slot maps used to bind native vendor expression operands to saved rows. */
struct GateMaps {
    std::span<const std::uint16_t> accountFlag;
    std::span<const std::uint16_t> profileFlag;
    std::span<const std::uint16_t> characterFlag;
    std::span<const std::uint16_t> accountValue;
    std::span<const std::uint16_t> characterValue;
};

/**
 * Extracts faction-package gates each process, including when vendor rows came from cache.
 * @param source Installed package directory and borrowed keys.
 * @param scratch Caller-owned package reader storage.
 * @param maps Native unlock-slot mappings retained from the investment root.
 * @return True after the complete derived gate catalog is published.
 */
[[nodiscard]] bool build_gates(const middleware::content::packages::reader::Source& source,
                               middleware::content::packages::reader::Scratch& scratch,
                               const GateMaps& maps) noexcept;

} // namespace sunrise::client::content::vendors
