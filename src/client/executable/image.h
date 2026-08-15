#pragma once

#include <array>
#include <cstddef>
#include <span>

namespace sunrise::client::executable {

/** Maximum section count accepted by the PE/COFF format. */
inline constexpr std::size_t kPeSectionLimit = 96;

/** Non-writable executable spans extracted from one mapped PE image. */
struct ExecutableImage {
    std::array<std::span<std::byte>, kPeSectionLimit> sections{};
    std::size_t count{};
};

/** Reads executable spans from mapped PE headers. */
[[nodiscard]] bool inspect(std::byte* base, ExecutableImage& output) noexcept;

/** Reads executable spans from the main process module. */
[[nodiscard]] bool inspect_main_module(ExecutableImage& output) noexcept;

} // namespace sunrise::client::executable
