#pragma once

#include <cstddef>
#include <cstdint>

namespace sunrise::client::targets::game::package_path {

/** The package path builder, the path object file opener, their layouts, and the tag loader. */
struct Targets {
    /** Entry of the builder the package registration job calls once per file. */
    std::byte* setPath{};
    /** Offset of the path text inside a package file object. */
    std::uint32_t pathOffset{};
    /** Bytes the path field holds, null included. */
    std::uint32_t pathCapacity{};
    /** Entry of the opener that turns a path object into a stdio file. */
    std::byte* openFile{};
    /** Offset of the root id in a path object; the path text fills the bytes before it. */
    std::uint32_t objectRootOffset{};
    /** Path object bytes the resolver reads: text, root id, then the flags word. */
    std::uint32_t objectSize{};
    /** Entry of the dispatcher that runs a loading tag's schema callbacks. */
    std::byte* schemaCallback{};
};

/** Clears the published package path group. */
void clear() noexcept;

/** @return Process-local package path group. */
[[nodiscard]] const Targets& get() noexcept;

/** @return True after the builder, the opener and their operands are published. */
[[nodiscard]] bool is_resolved() noexcept;

} // namespace sunrise::client::targets::game::package_path
