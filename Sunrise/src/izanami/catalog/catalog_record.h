#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "../core/ids.h"
#include "../runtime/runtime_adapter.h"

namespace sunrise::izanami::catalog {

inline constexpr std::uint32_t kStaticMeshClassId = 0x80806D44;
inline constexpr std::uint32_t kPatternClassId = 0x80809AD8;
inline constexpr std::uint32_t kEntityClassId = 0x80809C0F;

enum class AssetCategory : std::uint8_t {
    unknown,
    props,
    structures,
    lights,
    terrain,
    decals,
    water,
    environment,
    interactive,
    complex,
};

struct AssetAlias {
    std::string value{};
    bool userAuthored{};
};

struct CatalogRecord {
    core::ResourceId resource{};
    core::ObjectKind kind{core::ObjectKind::forgeOnly};
    AssetCategory category{AssetCategory::unknown};
    runtime::ResidencyState residency{runtime::ResidencyState::unknown};
    std::string packageFamily{};
    std::vector<AssetAlias> aliases{};
};

[[nodiscard]] core::ObjectKind object_kind_for_class(std::uint32_t classId) noexcept;

} // namespace sunrise::izanami::catalog