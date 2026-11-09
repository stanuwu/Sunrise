#pragma once

#include <cstddef>
#include <span>

#include "../../../../middleware/content/packages/reader/reader.h"

namespace sunrise::client::content::items::packages {

struct SlotMaps;

/**
 * Reads the dense objective table from the investment root and publishes it to State. A table
 * the installed data refuses settles the pursuit domains as unsupported, while a failed read is
 * retried on a later pass.
 * @param root Investment root bytes.
 * @param maps Unlock slot maps the objective programs bind through.
 * @return True when the objective table is in State.
 */
[[nodiscard]] bool build_objectives(const middleware::content::packages::reader::Source& source,
                                    middleware::content::packages::reader::Scratch& scratch,
                                    std::span<const std::byte> root,
                                    const SlotMaps& maps) noexcept;

} // namespace sunrise::client::content::items::packages
