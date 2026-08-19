#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "../mechanics/host_mechanics.h"
#include "../world/activity_policy.h"

namespace sunrise::server::gameplay::physics::persistence {

/** Fixed checkpoint and policy-state limits keep save/restore allocation-free. */
inline constexpr std::size_t kCheckpointCapacity = 4;
inline constexpr std::size_t kPolicyStateCapacity = 4'096;
inline constexpr std::size_t kInvalidCheckpointSlot = kCheckpointCapacity;

/** Bounded opaque state owned only by an activity policy. */
struct PolicyStateBlob final {
    std::array<std::byte, kPolicyStateCapacity> bytes{};
    std::size_t size{};
};

/** Logical mechanics state with no transport or entity-token ownership. */
struct MechanicsCheckpoint final {
    mechanics::TickTimerServiceSnapshot timers{};
    mechanics::TriggerSystemSnapshot triggers{};
    mechanics::ObjectiveLedgerSnapshot objectives{};
    mechanics::ParticipantCreditLedgerSnapshot participantCredit{};
    mechanics::CombatKernelSnapshot combat{};
    mechanics::WorldEpoch worldEpoch{};
};

/** Immutable logical world checkpoint suitable for cold recovery. */
struct Checkpoint final {
    world::WorldSnapshot world{};
    MechanicsCheckpoint mechanics{};
    PolicyStateBlob policyState{};
    world::BlueprintRef policy{};
    std::uint64_t checkpointId{};
    std::uint64_t logicalWorldId{};
    std::uint64_t contentBuildId{};
    std::uint64_t deterministicRngState{};
    std::uint64_t journalCursor{};
    std::uint64_t canonicalHash{};
    std::uint32_t schemaVersion{};
    std::uint32_t fixedRateHz{};
};

/** Generation-checked handle to one stored checkpoint. */
struct CheckpointHandle final {
    std::uint64_t generation{};
    std::size_t slot{kInvalidCheckpointSlot};
};

enum class StoreResult : std::uint8_t {
    saved,
    duplicate,
    invalid,
    stale,
    capacity,
    exhausted,
    notFound,
    erased,
};

/** Cold-recovery copy rebound to fresh logical owner generations. */
struct RecoveryImage final {
    Checkpoint checkpoint{};
    bool requiresCanonicalHash{};
};

/** Appends bounded activity-policy state into one checkpoint blob. */
class PolicyBlobWriter final : public world::IPolicyStateWriter {
public:
    explicit PolicyBlobWriter(PolicyStateBlob& blob) noexcept;
    [[nodiscard]] bool write(std::span<const std::byte> bytes) noexcept override;

private:
    PolicyStateBlob* blob_{};
};

/** Reads bounded activity-policy state from one checkpoint blob. */
class PolicyBlobReader final : public world::IPolicyStateReader {
public:
    explicit PolicyBlobReader(const PolicyStateBlob& blob) noexcept;
    [[nodiscard]] bool read(std::span<std::byte> bytes) noexcept override;
    [[nodiscard]] bool consumed_all() const noexcept;

private:
    const PolicyStateBlob* blob_{};
    std::size_t offset_{};
};

/** Fixed checkpoint store with monotonic per-world replacement. */
class CheckpointStore final {
public:
    void reset() noexcept;
    [[nodiscard]] StoreResult save(const Checkpoint& checkpoint, CheckpointHandle& handle) noexcept;
    [[nodiscard]] bool load(CheckpointHandle handle, Checkpoint& checkpoint) const noexcept;
    [[nodiscard]] bool latest(std::uint64_t logicalWorldId,
                              CheckpointHandle& handle) const noexcept;
    [[nodiscard]] StoreResult erase(CheckpointHandle handle) noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

private:
    struct Slot final {
        Checkpoint checkpoint{};
        std::uint64_t generation{1};
        bool occupied{};
    };

    std::array<Slot, kCheckpointCapacity> slots_{};
    std::size_t size_{};
};

/** Validates one pointer-free logical checkpoint. */
[[nodiscard]] bool valid_checkpoint(const Checkpoint& checkpoint) noexcept;

/** Computes the canonical hash while ignoring the destination hash field. */
[[nodiscard]] std::uint64_t compute_checkpoint_hash(const Checkpoint& checkpoint) noexcept;

/** Rebinds one checkpoint for cold recovery without retaining live wire state. */
[[nodiscard]] bool prepare_recovery(const Checkpoint& checkpoint,
                                    std::uint64_t newWorldGeneration,
                                    std::uint64_t newOwnerGeneration,
                                    mechanics::WorldEpoch newMechanicsEpoch,
                                    RecoveryImage& image) noexcept;

} // namespace sunrise::server::gameplay::physics::persistence
