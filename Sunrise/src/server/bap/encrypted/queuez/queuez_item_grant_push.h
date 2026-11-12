#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../internal.h"

namespace sunrise::server::bap::encrypted {

/**
 * One encoded Family-4 frame in scratch and the peer state that applies once it is sent. The frame
 * is sent only after the store savepoint commits.
 */
struct StagedFrame {
    queuez::SessionState after{};
    /** The send nonce after this frame, which the session takes only once the frame is sent. */
    std::array<std::byte, state::kBapNonceSize> sendNonce{};
    std::size_t size{};
};

/** One installed item and the quantity to grant through its reward definition. */
struct ItemGrant {
    std::uint16_t itemDefinitionIndex{};
    std::int32_t quantity{};
};

/** How far one grant attempt got inside the caller's savepoint. */
enum class ItemGrantStage : std::uint8_t {
    /** State committed the grant into the savepoint. The frame may still have failed. */
    committed,
    /** Nothing was written. The response is smaller than a whole frame, so a later poll retries. */
    waiting,
    /** Nothing was written. A catalog or saved read is not ready; the next attempt may succeed. */
    notReady,
    /** Nothing was written. State refuses the grant until saved state changes. */
    deferred,
    /** Nothing was written. State can never grant this item. */
    unresolvable,
    /** Nothing was written. Allocation, the random source or the State commit failed. */
    failed,
};

/** A grant that State committed into the caller's savepoint, and its frame. */
struct StagedItemGrant {
    StagedFrame frame{};
    /** True when any reward was placed. A reward with no room anywhere is dropped. */
    bool placed{};
    /** Why the grant has no frame, or null when the frame is complete. */
    const char* unpublished{};
};

/**
 * Prepares, stages, encodes and commits one item grant inside the caller's open store savepoint,
 * drawing its seed here. A stage or encode failure still commits the grant, because another poll
 * would fail the same way. The caller then resyncs the account.
 * @param response Poll output the frame must fit, though nothing is copied into it here.
 * @param staged Receives the committed grant and its frame.
 * @param reason Receives a static label when nothing was committed, or null while waiting.
 */
[[nodiscard]] ItemGrantStage stage_item_grant(const ItemGrant& grant,
                                              Session& session,
                                              Scratch& scratch,
                                              std::span<std::byte> response,
                                              bool& touchesScratch,
                                              StagedItemGrant& staged,
                                              const char*& reason) noexcept;

/**
 * Copies a committed change's staged frame into the response, moves the session to the frame's
 * after-image and publishes the account mutation to the other peers and to Family 5.
 */
void publish_staged_frame(const StagedFrame& frame,
                          Session& session,
                          Scratch& scratch,
                          std::span<std::byte> response,
                          std::size_t& written) noexcept;

/**
 * Publishes a grant once the caller's savepoint has committed it.
 * @return False when the grant has no frame, in which case every Family-4 peer resyncs the account.
 */
[[nodiscard]] bool publish_item_grant(const StagedItemGrant& staged,
                                      Session& session,
                                      Scratch& scratch,
                                      std::span<std::byte> response,
                                      std::size_t& written) noexcept;

} // namespace sunrise::server::bap::encrypted
