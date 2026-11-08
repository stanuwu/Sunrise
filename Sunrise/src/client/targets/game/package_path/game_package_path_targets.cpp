#include "game_package_path_targets.h"

#include <array>
#include <cstdint>
#include <cstring>

#include "../../../patterns/game/package_path/package_path_signature_bytes.h"

namespace sunrise::client::targets::game::package_path {
namespace {

/** The flags word follows the root id. */
constexpr std::int32_t kFlagsAfterRoot = sizeof(std::uint32_t);

Targets g_targets;
bool g_resolved{};

/** @return The signed 32-bit operand at one byte offset of a match. */
[[nodiscard]] std::int32_t operand(const patterns::Match& match, std::size_t offset) noexcept {
    std::int32_t value = 0;
    std::memcpy(&value, match.address + offset, sizeof value);
    return value;
}

} // namespace

/** Finds the path builder and the path object opener, and reads the layouts they use. */
bool derive(std::span<const patterns::ImageRange> image, Targets& output) noexcept {
    namespace signatures = patterns::game::package_path;
    output = {};
    const std::array<patterns::Pattern, 4> definitions{
        patterns::Pattern{"package_path_set", signatures::kSetPath},
        patterns::Pattern{"package_path_open", signatures::kOpenFile},
        patterns::Pattern{"package_path_resolve", signatures::kResolvePath},
        patterns::Pattern{"package_schema_callback", signatures::kSchemaCallback},
    };
    std::array<patterns::Match, 4> matches{};
    if (!patterns::resolve_all(image, definitions, matches)) {
        return false;
    }
    for (const patterns::Match& match : matches) {
        if (match.status != patterns::MatchStatus::unique) {
            return false;
        }
    }
    const std::int32_t pathOffset = operand(matches[0], signatures::kPathOffsetOperand);
    const std::int32_t pathCapacity = operand(matches[0], signatures::kPathCapacityOperand);
    const std::int32_t rootOffset = operand(matches[2], signatures::kRootOffsetOperand);
    const std::int32_t flagsOffset = operand(matches[2], signatures::kFlagsOffsetOperand);
    if (pathOffset <= 0 || pathCapacity <= 0 || rootOffset <= 0
        || flagsOffset != rootOffset + kFlagsAfterRoot) {
        return false;
    }
    output.setPath = matches[0].address;
    output.pathOffset = static_cast<std::uint32_t>(pathOffset);
    output.pathCapacity = static_cast<std::uint32_t>(pathCapacity);
    output.openFile = matches[1].address;
    output.objectRootOffset = static_cast<std::uint32_t>(rootOffset);
    output.objectSize = static_cast<std::uint32_t>(flagsOffset + kFlagsAfterRoot);
    output.schemaCallback = matches[3].address;
    return true;
}

/** @param targets Validated package path table published without failure. */
void publish(const Targets& targets) noexcept {
    g_targets = targets;
    g_resolved = true;
}

/** Clears the published package path group. */
void clear() noexcept {
    g_targets = {};
    g_resolved = false;
}

/** @return Process-local package path group. */
const Targets& get() noexcept {
    return g_targets;
}

/** @return True after the builder, the opener and their operands are published. */
bool is_resolved() noexcept {
    return g_resolved;
}

} // namespace sunrise::client::targets::game::package_path
