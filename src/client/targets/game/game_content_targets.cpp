#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "../../patterns/game.h"
#include "content.h"
#include "relative.h"
#include "resolution/internal.h"

namespace sunrise::client::targets::game::content {
namespace {

/** Rel32 begins 3 bytes into the schema-resolver load instruction. */
constexpr std::size_t kHandleTablesDisplacementOffset = 3;
/** The schema-resolver load instruction ends after 7 bytes. */
constexpr std::size_t kHandleTablesInstructionSize = 7;
/** The descriptor family bias is the immediate of the resolver's add instruction. */
constexpr std::size_t kDescriptorFamilyBiasOffset = 18;
/** The object-store getter call begins 22 bytes into the subscription function. */
constexpr std::size_t kObjectStoreCallOffset = 22;
/** A near call stores its signed displacement immediately after its opcode. */
constexpr std::size_t kNearCallDisplacementOffset = kObjectStoreCallOffset + 1;
/** The object-store near call ends after its opcode and 4-byte displacement. */
constexpr std::size_t kObjectStoreCallEndOffset = kObjectStoreCallOffset + 5;
/** Intel's direct near-call opcode guards the decoded target against layout drift. */
constexpr std::byte kNearCallOpcode{0xE8};

Targets g_targets;
std::atomic_bool g_resolved{false};

/** @param id Content signature identity. @return Group-local match index. */
[[nodiscard]] constexpr std::size_t index(patterns::game::Id id) noexcept {
    return static_cast<std::size_t>(id) - resolution::kContentFirstMatch;
}

} // namespace

/** Derives a content target table without publishing it. */
bool derive(std::span<const patterns::ImageRange> image,
            std::span<const patterns::Match> matches,
            Targets& output) noexcept {
    output = {};
    if (matches.size() != resolution::kContentMatchCount) {
        return false;
    }

    Targets resolved;
    resolved.getItemStatValue = matches[index(patterns::game::Id::getItemStatValue)].address;
    resolved.lightValueToScalar = matches[index(patterns::game::Id::lightValueToScalar)].address;
    auto* objectResolver = matches[index(patterns::game::Id::queuezObjectResolver)].address;
    auto* family5Subscribe = matches[index(patterns::game::Id::queuezFamily5Subscribe)].address;
    if (!relative::resolve(objectResolver,
                           kHandleTablesDisplacementOffset,
                           kHandleTablesInstructionSize,
                           resolved.contentHandleTablesSlot)
        || family5Subscribe[kObjectStoreCallOffset] != kNearCallOpcode
        || !relative::resolve(family5Subscribe,
                              kNearCallDisplacementOffset,
                              kObjectStoreCallEndOffset,
                              resolved.queuezObjectStoreGetter)
        || !relative::contains(image, resolved.queuezObjectStoreGetter)) {
        return false;
    }
    std::memcpy(&resolved.queuezDescriptorFamilyBias,
                objectResolver + kDescriptorFamilyBiasOffset,
                sizeof resolved.queuezDescriptorFamilyBias);
    if (resolved.queuezDescriptorFamilyBias == 0) {
        return false;
    }
    output = resolved;
    return true;
}

/** @param targets Fully validated content table published without failure. */
void publish(const Targets& targets) noexcept {
    g_targets = targets;
    g_resolved.store(true, std::memory_order_release);
}

/** Resolves the late content targets and publishes only a complete group. */
bool resolve(std::span<const patterns::ImageRange> image) noexcept {
    g_resolved.store(false, std::memory_order_release);
    g_targets = {};
    const auto definitions = patterns::game::definitions().subspan(resolution::kContentFirstMatch,
                                                                   resolution::kContentMatchCount);
    std::array<patterns::Match, resolution::kContentMatchCount> matches{};
    if (!patterns::resolve_all(image, definitions, matches)) {
        return false;
    }
    for (const patterns::Match match : matches) {
        if (match.status != patterns::MatchStatus::unique) {
            return false;
        }
    }

    Targets resolved;
    if (!derive(image, matches, resolved)) {
        return false;
    }
    publish(resolved);
    return true;
}

/** Clears every published late content target. */
void clear() noexcept {
    g_resolved.store(false, std::memory_order_release);
    g_targets = {};
}

/** @return Current immutable late content target group. */
const Targets& get() noexcept {
    return g_targets;
}

/** @return True after the complete late content group is published. */
bool is_resolved() noexcept {
    return g_resolved.load(std::memory_order_acquire);
}

} // namespace sunrise::client::targets::game::content
