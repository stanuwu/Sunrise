#include "network_content_derivation.h"

#include <array>

#include "../relative.h"

namespace sunrise::client::targets::game::network::content_derivation {
namespace {

/** Bounds the scan for the fetch wrapper's guarded GET call. */
constexpr std::size_t kFetchScanLimit = 0x600;
/** A near call is 5 bytes: 1 opcode then its signed rel32. */
constexpr std::size_t kNearCallSize = 5;
/** A near call stores its rel32 one byte after its opcode. */
constexpr std::size_t kNearCallDisplacementOffset = 1;
/** Intel's direct near-call opcode. */
constexpr std::byte kNearCallOpcode{0xE8};
/** The compare begins after the matched store and call in the gate signature. */
constexpr std::size_t kGateDisplacementOffset = 15;
/** The compare ends after its immediate byte. */
constexpr std::size_t kGateInstructionEndOffset = 20;

/** Prologue that distinguishes the allocated-buffer GET from its POST sibling. */
constexpr std::array kGetPrologue{
    std::byte{0x48},
    std::byte{0x89},
    std::byte{0x5C},
    std::byte{0x24},
    std::byte{},
    std::byte{0x48},
    std::byte{0x89},
    std::byte{0x74},
    std::byte{0x24},
    std::byte{},
    std::byte{0x57},
    std::byte{0x48},
    std::byte{0x81},
    std::byte{0xEC},
    std::byte{0xB0},
    std::byte{0x02},
    std::byte{0x00},
    std::byte{0x00},
};
/** Stack displacement bytes vary between builds and are skipped by the prologue test. */
constexpr std::array kGetPrologueWildcards{std::size_t{4}, std::size_t{9}};

/** @param address Candidate entry. @return True when the GET prologue is present. */
[[nodiscard]] bool is_get_entry(const std::byte* address) noexcept {
    for (std::size_t index = 0; index < kGetPrologue.size(); ++index) {
        const bool skipped = index == kGetPrologueWildcards[0] || index == kGetPrologueWildcards[1];
        if (!skipped && address[index] != kGetPrologue[index]) {
            return false;
        }
    }
    return true;
}

/**
 * Finds the one call in the fetch wrapper that reaches the allocated-buffer GET.
 * The target's own prologue identifies it, so no fixed call offset is assumed.
 * @param image Executable ranges from the main game image.
 * @param fetch Matched config fetch wrapper.
 * @param target Receives the single matching GET entry point.
 * @return True when exactly one call in range resolves to the GET prologue.
 */
[[nodiscard]] bool find_get_call(std::span<const patterns::ImageRange> image,
                                 std::byte* fetch,
                                 std::byte*& target) noexcept {
    target = nullptr;
    for (std::size_t offset = 0; offset < kFetchScanLimit; ++offset) {
        std::byte* candidate = nullptr;
        if (!relative::contains(image, fetch + offset, kNearCallSize)
            || fetch[offset] != kNearCallOpcode
            || !relative::resolve(
                fetch + offset, kNearCallDisplacementOffset, kNearCallSize, candidate)
            || !relative::contains(image, candidate, kGetPrologue.size())
            || !is_get_entry(candidate)) {
            continue;
        }
        // A second reachable GET call would make the choice ambiguous.
        if (target != nullptr && target != candidate) {
            target = nullptr;
            return false;
        }
        target = candidate;
    }
    return target != nullptr;
}

} // namespace

/** Derives the content-network targets without publishing them. */
bool derive(std::span<const patterns::ImageRange> image,
            std::byte* fetch,
            std::byte* gate,
            Targets& resolved) noexcept {
    // The gate signature already matched the compare, so only its operand is read here.
    return find_get_call(image, fetch, resolved.httpEnqueueGet)
           && relative::resolve(gate,
                                kGateDisplacementOffset,
                                kGateInstructionEndOffset,
                                resolved.contentManifestSignatureGate);
}

} // namespace sunrise::client::targets::game::network::content_derivation
