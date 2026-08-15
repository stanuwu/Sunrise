#pragma once

#include <cstddef>
#include <span>
#include <string_view>

#include "../../../state/content_manifest/definition.h"
#include "../../../state/entitlements/definition.h"

namespace sunrise::middleware::content::manifest {

/**
 * Encodes one ContentConfig entitlement definition into caller storage.
 * @param entry Checked authored definition.
 * @param definitionHandle Manifest handle the Client finds the definition by.
 * @param output Fixed caller-owned nested-message storage.
 * @param size Cleared output receiving the nested byte count.
 * @return True when every required field fits.
 */
[[nodiscard]] bool encode_entitlement(const state::entitlements::Entitlement& entry,
                                      std::uint32_t definitionHandle,
                                      std::span<std::byte> output,
                                      std::size_t& size) noexcept;

/**
 * Encodes the raw ContentConfig body: field-2 entitlement definitions, field-3 package rows,
 * and the field-5 public GUID.
 * @param entitlements Authored ownership policy. It defines every declared offer.
 * @param rows Sorted State-owned package rows, one per public package id.
 * @param guid Nonsecret canonical 36-character manifest UUID.
 * @param output Fixed caller-owned protobuf storage.
 * @param size Cleared output receiving the whole encoded byte count.
 * @return True when every required field is valid and fits caller storage.
 */
[[nodiscard]] bool encode(const state::entitlements::Table& entitlements,
                          std::span<const state::content_manifest::Row> rows,
                          std::string_view guid,
                          std::span<std::byte> output,
                          std::size_t& size) noexcept;

} // namespace sunrise::middleware::content::manifest
