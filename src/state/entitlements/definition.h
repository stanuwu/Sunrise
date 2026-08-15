#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace sunrise::state::entitlements {

/** The Client allocates manifest entitlement handles upward from this fixed base. */
inline constexpr std::uint32_t kHandleBase = 0xE0200001;
/** Every ContentConfig package row uses this one offer key. */
inline constexpr std::uint32_t kOfferKey = 0xD0000448;
/** Entitlement names are short ASCII tokens. The field reserves one null. */
inline constexpr std::size_t kNameCapacity = 32;
/** The authored ownership table is sized well above what a local account needs. */
inline constexpr std::size_t kCapacity = 128;

/** How one defined entitlement is declared to the Client. */
enum class Ownership : std::uint8_t {
    /** Defined in the manifest and deliberately not owned. */
    none,
    /** Owned and declared by its manifest handle. */
    handle,
    /** Owned and declared by the numeric application id its name holds. */
    application,
};

/** One authored entitlement definition. Its handle is its index plus the fixed base. */
struct Entitlement final {
    /** Application id text, or a manifest-defined literal name. */
    std::array<char, kNameCapacity> name{};
    /** Used bytes in the name field, not counting the null. */
    std::uint8_t nameLength{};
    /** Declaration form used by the SignOn ownership list. */
    Ownership ownership{Ownership::none};
};

/** Immutable authored ownership policy shared by SignOn, ContentConfig, and Steam. */
struct Table final {
    std::array<Entitlement, kCapacity> entries{};
    std::size_t count{};
};

/**
 * @param index Zero-based table index.
 * @return The manifest handle the Client finds this definition by.
 */
[[nodiscard]] constexpr std::uint32_t handle(std::size_t index) noexcept {
    return kHandleBase + static_cast<std::uint32_t>(index);
}

/** @param entry Authored definition. @return Its name without the unused tail bytes. */
[[nodiscard]] constexpr std::string_view name_of(const Entitlement& entry) noexcept {
    return {entry.name.data(), entry.nameLength};
}

/** @param table Authored policy. @return Only the used leading entries. */
[[nodiscard]] constexpr std::span<const Entitlement> used(const Table& table) noexcept {
    return std::span(table.entries).first(table.count);
}

} // namespace sunrise::state::entitlements
