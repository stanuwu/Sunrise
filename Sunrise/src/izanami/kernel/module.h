#pragma once

#include <cstdint>
#include <string_view>

namespace sunrise::izanami::kernel {

struct Version {
    std::uint16_t major{};
    std::uint16_t minor{};
    std::uint16_t patch{};
};

struct ModuleId {
    std::string_view value{};
};

struct Dependency {
    ModuleId module{};
    Version minimumVersion{};
    bool optional{};
};

class ModuleContext;

class IIzanamiModule {
public:
    virtual ~IIzanamiModule() = default;

    [[nodiscard]] virtual ModuleId id() const noexcept = 0;
    [[nodiscard]] virtual Version version() const noexcept = 0;
    [[nodiscard]] virtual bool startup(ModuleContext& context) noexcept = 0;
    virtual void shutdown(ModuleContext& context) noexcept = 0;
};

class ModuleContext final {};

} // namespace sunrise::izanami::kernel