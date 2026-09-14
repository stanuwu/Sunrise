#include "replicate_membership.h"

namespace sunrise::middleware::bap::activity_message::replicate_membership {
namespace {

/** Region indices use the signed 32-bit midpoint as their wire bias. */
constexpr std::uint32_t kRegionIndexBias = 0x80000000U;
/** Each record carries one transition-token byte for every member slot. */
constexpr std::size_t kRegionTokenCount = 32;
/** Spawn and teleport state fields use 3 bits. */
constexpr std::uint8_t kStateBitWidth = 3;
/** Teleport slice-set indices use 10 bits. */
constexpr std::uint8_t kSliceSetBitWidth = 10;
/** Signed host-state fields add 1 before writing their unsigned value. */
constexpr std::int32_t kSignedFieldBias = 1;

/** Ambassadorship state fields are 2 bits at bias 1, so wire 2 stores the assigned state 1. */
constexpr std::uint64_t kAmbassadorAssigned = 2;
/** Member-slot fields are 6 bits at bias 1. */
constexpr std::uint8_t kSlotBitWidth = 6;
/**
 * Slot a record with no advertisement names. It is reserved: the session-slot allocator never
 * hands it to a real member, so it can never be the recipient's own and an unadvertised record
 * never revokes the recipient's claim on the region it is standing in. Naming the no-member
 * slot -1 there does revoke it, and the client then re-plans out of a region it is already in.
 */
constexpr std::uint8_t kReservedAmbassadorSlot = 1;
/** Region publicity is 2 bits at bias 1, so wire 1 is private and wire 2 is public. */
constexpr std::uint8_t kPublicBitWidth = 2;
constexpr std::uint64_t kRegionPrivate = 1;
constexpr std::uint64_t kRegionPublic = 2;
/** The join-descriptor element count is an unclamped 8-bit field; only 128 is usable. */
constexpr std::uint64_t kDescriptorCount = 128;

/**
 * Writes one bubble's region record.
 * A record carrying a citizen advertisement is 1,024 bits longer than the others.
 * @param writer Fixed-buffer writer sitting at the record start.
 * @param bubble Bubble index, 0 through 63.
 * @param snapshot Transition token and the host directory.
 * @return True when every fixed field fits.
 */
[[nodiscard]] bool write_region(encoding::bits::Writer& writer,
                                std::size_t bubble,
                                const MembershipSnapshot& snapshot) noexcept {
    const CitizenAdvertisement* const citizen = advertisement_in_bubble(snapshot, bubble);
    // The client scans the 64 records for the one whose index equals its target region, so the
    // record for that region's bubble must name it, state ordinal included. Any other bubble
    // keeps its state-zero index, which is what an ordinary transition targets.
    const bool selfHostedHere =
        snapshot.selfHosted && snapshot.selfHostedRegion >= 0
        && static_cast<std::size_t>(snapshot.selfHostedRegion / kRegionStateCount) == bubble;
    const std::uint32_t region =
        citizen != nullptr ? static_cast<std::uint32_t>(citizen->regionIndex)
        : selfHostedHere
            ? static_cast<std::uint32_t>(snapshot.selfHostedRegion)
            : static_cast<std::uint32_t>(bubble) * static_cast<std::uint32_t>(kRegionStateCount);
    // The reserved slot can never be the recipient's own. Asserted rather than assumed: a snapshot
    // that somehow put the local member there falls back to naming slot 0, which is still not the
    // recipient.
    const std::uint64_t unadvertisedSlot =
        (snapshot.localSlot == kReservedAmbassadorSlot ? std::uint64_t{0}
                                                       : std::uint64_t{kReservedAmbassadorSlot})
        + kSignedFieldBias;
    // An advertised record names the admitted ambassador. A self-hosted body names the client's
    // own slot on every record, which makes it claim each region. Every other record names the
    // reserved slot, which leaves an existing claim alone.
    const std::uint64_t ambassadorSlot =
        citizen != nullptr ? static_cast<std::uint64_t>(citizen->ambassadorSlot) + kSignedFieldBias
        : snapshot.selfHosted ? static_cast<std::uint64_t>(snapshot.localSlot) + kSignedFieldBias
                              : unadvertisedSlot;
    const bool isPublic = ((snapshot.regionPublicMask >> bubble) & 1U) != 0;
    bool encoded = writer.write(kRegionIndexBias + region, 32)
                   && writer.write(isPublic ? kRegionPublic : kRegionPrivate, kPublicBitWidth)
                   && writer.write(0, 8) && writer.write(kAmbassadorAssigned, 2)
                   && writer.write(ambassadorSlot, kSlotBitWidth) && writer.write(0, 1)
                   && writer.write(0, 32) && writer.write(0, 8) && writer.write(0, 32);
    // A lane says its member holds this transition token. An empty slot holds nothing.
    const std::uint32_t occupied = occupied_member_mask(snapshot);
    for (std::size_t member = 0; encoded && member < kRegionTokenCount; ++member) {
        const bool ownOrService =
            (member == snapshot.localSlot || member == 1) && ((occupied >> member) & 1U) != 0;
        const auto* peer = peer_at_slot(snapshot, member);
        const auto token = ownOrService                       ? snapshot.transitionToken
                           : peer && peer->hasTransitionToken ? peer->transitionToken
                                                              : 0U;
        encoded = writer.write(token, 8);
    }
    if (citizen == nullptr) {
        return encoded && writer.write(0, 8) && writer.write(0, 64);
    }
    encoded = encoded && writer.write(kDescriptorCount, 8);
    for (const std::byte value : citizen->descriptor) {
        encoded = encoded && writer.write(std::to_integer<std::uint64_t>(value), 8);
    }
    return encoded && writer.write(citizen->onlineSessionId, 64);
}

/**
 * Writes one host key, low byte first.
 * @param writer Fixed-buffer writer sitting after the host presence bit.
 * @param key Host-order key shared with the populated member.
 * @return True when all 8 key bytes fit.
 */
[[nodiscard]] bool write_host_key(encoding::bits::Writer& writer, std::uint64_t key) noexcept {
    for (std::size_t index = 0; index < sizeof key; ++index) {
        if (!writer.write((key >> (index * 8U)) & 0xFFU, 8)) {
            return false;
        }
    }
    return true;
}

/**
 * Writes the current host-state tail after all region records.
 * @param writer Fixed-buffer writer sitting at the first spawn field.
 * @param snapshot Reflected host key, spawn state and teleport state.
 * @return True when the host-present tail fits.
 */
[[nodiscard]] bool write_host_tail(encoding::bits::Writer& writer,
                                   const MembershipSnapshot& snapshot) noexcept {
    // Both state fields are 3 bits at bias 1, so a logical -1 encodes as wire zero. That is fatal
    // in each: the spawn reader then refuses every spawn, and the teleport reader arms a teleport
    // every cycle and de-instantiates the world. So a negative mirror is sent as a neutral zero.
    const auto spawnState = static_cast<std::uint8_t>(
        (snapshot.spawn.state < 0 ? 0 : snapshot.spawn.state) + kSignedFieldBias);
    const auto teleportState = static_cast<std::uint8_t>(
        (snapshot.teleport.state < 0 ? 0 : snapshot.teleport.state) + kSignedFieldBias);
    const auto sliceSetIndex =
        static_cast<std::uint32_t>(snapshot.teleport.sliceSetIndex + kSignedFieldBias);
    return writer.write(snapshot.spawn.opaqueByte, 8) && writer.write(spawnState, kStateBitWidth)
           && writer.write(snapshot.spawn.opaqueValue, 64) && writer.write(0, 4)
           && writer.write(teleportState, kStateBitWidth)
           && writer.write(snapshot.teleport.token, 8)
           && writer.write(sliceSetIndex, kSliceSetBitWidth)
           && writer.write(snapshot.teleport.spawnSetHash, 32) && writer.write(0, 1)
           && writer.write(1, 1) && write_host_key(writer, snapshot.identity.memberKey)
           && writer.write(0, 1) && writer.write(0, 1);
}

} // namespace

/** Writes the 64 region records and the host-present tail. */
bool write_region_block(encoding::bits::Writer& writer,
                        const MembershipSnapshot& snapshot) noexcept {
    bool encoded = writer.bit_count() == region_block_start_bit(snapshot);
    for (std::size_t bubble = 0; encoded && bubble < kRegionRecordCount; ++bubble) {
        encoded = write_region(writer, bubble, snapshot);
    }
    return encoded && write_host_tail(writer, snapshot)
           && writer.bit_count() == region_block_end_bit(snapshot);
}

} // namespace sunrise::middleware::bap::activity_message::replicate_membership
