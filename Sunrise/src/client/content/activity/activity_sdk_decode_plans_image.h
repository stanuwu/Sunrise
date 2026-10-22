#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <vector>

#include "activity_sdk_decode_plans.h"

namespace sunrise::client::content::activity::sdk_generation::decode_plans {

/**
 * The executable's reflection records and slot index.
 * The shipped file is packed and its slot table exists only in memory, so a live build reads the
 * mapped module and hashes the file; an offline build reads and hashes an unpacked dump.
 */
class Image final {
public:
    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image() noexcept;

    /** Maps the file at `path`; a non-null `module` base makes the reads come from memory. */
    [[nodiscard]] Status open(const wchar_t* path, const void* module) noexcept;
    /** @return The whole file on disk, for the evidence digest. */
    [[nodiscard]] std::span<const std::byte> file() const noexcept;
    /** @return The bytes the reflection reads go through: the file or the mapped module. */
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept;
    /** Bound handle to reflection record address, first slot wins. */
    [[nodiscard]] const std::map<std::uint32_t, std::uint64_t>& slots() const noexcept {
        return slots_;
    }
    /** Address of the 64-entry primitive size table, one u32 per field type code. */
    [[nodiscard]] std::uint64_t primitive_sizes() const noexcept {
        return primitiveSizes_;
    }
    [[nodiscard]] bool u8(std::uint64_t address, std::uint8_t& value) const noexcept;
    [[nodiscard]] bool u16(std::uint64_t address, std::uint16_t& value) const noexcept;
    [[nodiscard]] bool u32(std::uint64_t address, std::uint32_t& value) const noexcept;
    [[nodiscard]] bool i32(std::uint64_t address, std::int32_t& value) const noexcept;
    [[nodiscard]] bool f32(std::uint64_t address, float& value) const noexcept;

private:
    struct Section final {
        std::uint64_t virtualAddress{};
        std::uint64_t virtualSize{};
        std::uint64_t rawOffset{};
        std::uint64_t rawSize{};
        bool data{};
        bool code{};
    };

    [[nodiscard]] bool
    offset(std::uint64_t address, std::size_t size, std::size_t& at) const noexcept;
    [[nodiscard]] bool parse_headers(bool mapped) noexcept;
    [[nodiscard]] bool index_slots() noexcept;
    [[nodiscard]] bool resolve_primitive_sizes() noexcept;

    HANDLE file_{INVALID_HANDLE_VALUE};
    HANDLE mapping_{};
    const std::byte* view_{};
    std::size_t viewSize_{};
    /** Read source: the file view, or the module memory with its SizeOfImage. */
    const std::byte* data_{};
    std::size_t size_{};
    std::uint64_t imageBase_{};
    std::uint64_t vmp0Low_{};
    std::uint64_t vmp0High_{};
    std::uint64_t primitiveSizes_{};
    std::vector<Section> sections_{};
    std::map<std::uint32_t, std::uint64_t> slots_{};
};

} // namespace sunrise::client::content::activity::sdk_generation::decode_plans
