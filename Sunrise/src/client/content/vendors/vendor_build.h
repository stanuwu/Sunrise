#pragma once

#include <cstddef>
#include <span>

#include "../../../middleware/content/packages/reader/reader.h"
#include "../../../middleware/content/packages/tables/definition_index_table.h"

namespace sunrise::client::content::items::packages {
class RewardConditions;
}

namespace sunrise::client::content::vendors {

/**
 * Extracts the vendor catalog from the installed packages, once.
 * The whole index is read, and a definition for every row it names.
 * @param source Package directory and borrowed block keys.
 * @param scratch Lock-owned block storage shared with the other content passes.
 * @param conditions Loaded condition tables each sale row's availability binds through.
 * @return True when State already holds the catalog or a full pass publishes it.
 */
[[nodiscard]] bool build(const middleware::content::packages::reader::Source& source,
                         middleware::content::packages::reader::Scratch& scratch,
                         const items::packages::RewardConditions& conditions) noexcept;

/**
 * Reads supported faction package gear leaves from the installed nested reward lists.
 * @param source Installed package directory and borrowed keys.
 * @param scratch Lock-owned package block storage.
 * @param root Resolved investment root.
 * @param itemTable Resolved item index table bytes.
 * @param itemRows Item index array in itemTable.
 * @return True when all supported packages have published their candidate pools.
 */
[[nodiscard]] bool
build_rewards(const middleware::content::packages::reader::Source& source,
              middleware::content::packages::reader::Scratch& scratch,
              std::span<const std::byte> root,
              std::span<const std::byte> itemTable,
              const middleware::content::packages::tables::Array& itemRows) noexcept;

} // namespace sunrise::client::content::vendors
