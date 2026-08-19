#pragma once

#include <array>
#include <cstdint>

namespace sunrise::izanami::core {

/** Stable object identity serialized by Izanami projects. */
struct ForgeUUID {
    std::array<std::uint8_t, 16> bytes{};

    [[nodiscard]] bool is_nil() const noexcept;

    friend bool operator==(const ForgeUUID&, const ForgeUUID&) noexcept = default;
};

/** Stable reference to a locally installed Destiny resource, not a native pointer. */
struct ResourceId {
    std::uint32_t classId{};
    std::uint32_t tagHash{};
    std::uint64_t wideHash{};

    [[nodiscard]] bool is_valid() const noexcept;

    friend bool operator==(const ResourceId&, const ResourceId&) noexcept = default;
};

[[nodiscard]] ForgeUUID make_forge_uuid(std::uint64_t high, std::uint64_t low) noexcept;

/** Logical authored object kind. Runtime adapters may map one object to many native objects. */
enum class ObjectKind : std::uint8_t {
    forgeOnly,
    staticInstance,
    patternInstance,
    entityInstance,
    folder,
};

} // namespace sunrise::izanami::core
