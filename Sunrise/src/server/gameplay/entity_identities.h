#pragma once

#include "../../middleware/gameplay/external/external_entity_codec.h"
#include "../../state/activity_sdk/runtime.h"
#include "../../state/gameplay/external/entity_identity.h"

namespace sunrise::server::gameplay::entity_identities {

namespace identities = state::gameplay::entity_identity;

/** Release before peer or host callbacks; accepted identity batches take the exclusive lock. */
class PublicationLease final {
public:
    PublicationLease() noexcept = default;
    ~PublicationLease();
    PublicationLease(const PublicationLease&) = delete;
    PublicationLease& operator=(const PublicationLease&) = delete;
    void release() noexcept;
    [[nodiscard]] bool held() const noexcept {
        return held_;
    }

private:
    friend identities::Result begin_publication(const identities::Source&,
                                                std::vector<identities::Identity>&,
                                                PublicationLease&) noexcept;
    bool held_{};
};

/** Pins exact-source identities until the publication copies its staged bytes. */
[[nodiscard]] identities::Result begin_publication(const identities::Source& source,
                                                   std::vector<identities::Identity>& output,
                                                   PublicationLease& lease) noexcept;

/** Observations enter only after the complete external packet is accepted. */
void observe(const identities::Source& source,
             const middleware::gameplay::external::EntityBatch& batch,
             const state::activity_sdk::Snapshot& catalog,
             std::uint16_t packetSequence,
             bool hasPacketSequence,
             std::uint64_t ordinal,
             std::uint64_t tick,
             std::uint8_t allocationEpoch = 0,
             bool hasAllocationEpoch = false,
             std::uint64_t allocationDomain = 0) noexcept;

/** Exact-source lookup never selects another peer's entity with the same slot number. */
[[nodiscard]] identities::Result lookup(const identities::Source& source,
                                        identities::Token token,
                                        identities::Identity& output) noexcept;
/** Returns the source count; output storage may hold a bounded prefix. */
[[nodiscard]] std::size_t sources(std::uint64_t activitySessionId,
                                  std::uint64_t activityRevision,
                                  std::span<identities::Source> output) noexcept;
/** Copies all exact-source slots atomically, including tombstones and conflicts. */
[[nodiscard]] identities::Result
snapshot_source(const identities::Source& source,
                std::vector<identities::Identity>& output) noexcept;
/**
 * Walks all exact-source slots in slot order, including tombstones and conflicts.
 * The visitor runs under the registry lock and must not re-enter this registry.
 */
[[nodiscard]] identities::Result visit_source(const identities::Source& source,
                                              identities::Store::SlotVisitor visitor,
                                              void* context) noexcept;
/** Advances only the exact source allocation domain after a committed host purge. */
[[nodiscard]] bool advance_epoch(const identities::Source&,
                                 std::uint8_t expected,
                                 std::uint8_t next,
                                 std::uint64_t nextDomain) noexcept;
void reset_group(std::uint64_t groupSessionId) noexcept;
/** Call only after publication and after releasing the identity lease. */
[[nodiscard]] std::size_t retire(const identities::Source& source,
                                 std::span<const identities::RetiredLifetime> lifetimes) noexcept;
void reset_source(const identities::Source& source) noexcept;
void reset() noexcept;

} // namespace sunrise::server::gameplay::entity_identities
