#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace sunrise::state::build_data::material_requirements {

/** Installed build currently carries 229 dense sets; keep bounded headroom. */
inline constexpr std::size_t kDefinitionCapacity = 512;
/** Widest installed material-requirement set contains six rows. */
inline constexpr std::size_t kRequirementCapacity = 6;
/** All item-index bits set mean the row does not name an installed item. */
inline constexpr std::uint16_t kUnavailableItemDefinitionIndex = 0xFFFFU;
/** All requirement-index bits set mean an item/action declares no requirement set. */
inline constexpr std::uint16_t kUnavailableSetIndex = 0xFFFFU;
/**
 * All condition bits set mark a row the native cost reader counts in inventory. Any other value is
 * a value slot, whose account value the reader counts and the cost debit decrements instead.
 */
inline constexpr std::uint16_t kUnconditionalRequirement = 0xFFFFU;
/** All bits set mark a row whose value slot maps to no account value row. */
inline constexpr std::uint16_t kUnavailableValueIndex = 0xFFFFU;

/** One exact native material row. Quantity zero is authored by free cosmetic requirements. */
struct Requirement {
    std::uint32_t quantity{};
    std::uint16_t itemDefinitionIndex{kUnavailableItemDefinitionIndex};
    std::uint16_t condition{kUnconditionalRequirement};
    bool deleteOnAction{};
    bool omitFromRequirements{};
    /** Account value row the condition's value slot maps to, for a row counted outside inventory. */
    std::uint16_t valueIndex{kUnavailableValueIndex};
};

/** One dense native requirement set, addressed by its installed ordinal. Count zero is free. */
struct Definition {
    std::uint32_t requirementSetHash{};
    std::uint16_t requirementSetIndex{kUnavailableSetIndex};
    std::uint8_t requirementCount{};
    std::array<Requirement, kRequirementCapacity> requirements{};
};

/** The shipped build declares 13 currency scalars on 7 of its 765 socket types; keep headroom. */
inline constexpr std::size_t kCurrencyScalarCapacity = 64;

/** One socket type's multiplier for one currency's quantity in an inserted plug's costs. */
struct CurrencyScalar {
    std::uint16_t socketType{};
    std::uint16_t itemDefinitionIndex{kUnavailableItemDefinitionIndex};
    std::uint32_t multiplier{};
};

void clear() noexcept;
/** Scalars are sorted by socket type, then currency, each pair once. */
[[nodiscard]] bool valid(std::span<const Definition> definitions,
                         std::span<const CurrencyScalar> scalars) noexcept;
[[nodiscard]] bool replace(std::span<const Definition> definitions,
                           std::span<const CurrencyScalar> scalars) noexcept;
[[nodiscard]] bool find(std::uint16_t requirementSetIndex, Definition& definition) noexcept;
/** @return The socket type's multiplier for the currency, or 1 when it names none. */
[[nodiscard]] std::uint32_t currency_multiplier(std::uint16_t socketType,
                                                std::uint16_t itemDefinitionIndex) noexcept;
[[nodiscard]] bool snapshot(std::span<Definition> output, std::size_t& count) noexcept;
/** Copies every currency scalar; count is zero when the output is too small. */
[[nodiscard]] bool snapshot_currency_scalars(std::span<CurrencyScalar> output,
                                             std::size_t& count) noexcept;
[[nodiscard]] std::size_t count() noexcept;

} // namespace sunrise::state::build_data::material_requirements
