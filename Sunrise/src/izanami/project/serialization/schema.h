#pragma once

#include <cstdint>
#include <string>

namespace sunrise::izanami::project::serialization {

inline constexpr std::uint32_t kProjectFormatVersion = 1;
inline constexpr std::uint32_t kSceneSchemaVersion = 1;
inline constexpr std::uint32_t kCatalogSchemaVersion = 1;
inline constexpr std::uint32_t kFateModuleAbiVersion = 1;
inline constexpr char kProjectExtension[] = ".izanami";

struct ProjectHeader {
    std::uint32_t projectFormatVersion{kProjectFormatVersion};
    std::uint32_t minimumIzanamiVersion{};
    std::string projectName{};
};

[[nodiscard]] bool is_supported_project_version(std::uint32_t version) noexcept;

} // namespace sunrise::izanami::project::serialization