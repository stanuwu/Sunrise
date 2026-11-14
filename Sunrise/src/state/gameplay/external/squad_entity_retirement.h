#pragma once
#include <array>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

#include "../../activity/definition.h"
#include "entity_identity.h"
namespace sunrise::state::gameplay::squad_entity_retirement {
namespace identities = entity_identity;
using Mask = activity::bubble_authority::EntitySlotMask;
using CellBubbles = std::array<std::int16_t, 256>;
/** A spawned actor names its source: its squad, or the combatant that binds it to one. */
inline constexpr std::uint8_t kSquadSourceType = 1;
inline constexpr std::uint8_t kCombatantSourceType = 2;
/**
 * An authored object entry names its own type-4 slot as its source. The host policy for it: only
 * an entry whose Auth the host retains, because a mission wrote it, is eligible.
 */
inline constexpr std::uint8_t kObjectSourceType = 4;
/** Native map-prop types exclude actors, players, weapons and interactive objects. */
inline constexpr std::array<std::uint8_t, 5> kRetirablePropTypes{1, 2, 3, 7, 8};
/** Retirement eligibility comes from an explicit host policy for an exact class. */
struct Eligibility final {
    identities::SquadReference squad{};
    std::uint32_t rsatTag{};
    std::uint8_t bubble{};
    bool enabled{};
    bool placedProp{};
    std::uint8_t objectType{};
    bool operator==(const Eligibility&) const = default;
};
/** A prepared mask remains pending until its exact transport publication commits. */
struct RetirementPlan final {
    identities::Source source{};
    Mask entities{};
    /** One atomic retirement publication admits at most 128 exact entity lifetimes. */
    static constexpr std::size_t kLifetimeCapacity = 128;
    std::array<identities::RetiredLifetime, kLifetimeCapacity> lifetimes{};
    std::size_t lifetimeCount{};
    [[nodiscard]] std::span<const identities::RetiredLifetime> retired_lifetimes() const noexcept {
        return lifetimeCount <= lifetimes.size() ? std::span(lifetimes).first(lifetimeCount)
                                                 : std::span<const identities::RetiredLifetime>{};
    }
    std::uint64_t revision{};
    std::uint8_t bubble{};
    bool pending{};
    bool placedProps{};
};
static_assert(std::is_trivially_copyable_v<RetirementPlan>);
/** Caller synchronization protects captured releases and their publication revisions. */
class Store final {
public:
    [[nodiscard]] bool capture(const identities::Source&,
                               std::uint8_t bubble,
                               const Mask&,
                               std::span<const identities::Identity>,
                               std::span<const Eligibility>,
                               const CellBubbles&,
                               bool requireComplete = false) noexcept;
    [[nodiscard]] bool prepare(const identities::Source&,
                               std::uint8_t bubble,
                               std::span<const identities::Identity>,
                               RetirementPlan&) const noexcept;
    [[nodiscard]] bool commit(const RetirementPlan&) noexcept;
    [[nodiscard]] bool pending(const identities::Source&, std::uint8_t bubble) const noexcept;
    /** @return A bubble other than `excluded` with a pending release of the exact source. */
    [[nodiscard]] std::optional<std::uint8_t> pending_bubble(const identities::Source&,
                                                             std::uint8_t excluded) const noexcept;
    void invalidate_source(const identities::Source&, std::uint8_t bubble) noexcept;
    void returned_slots(std::uint64_t session, std::uint64_t generation, const Mask&) noexcept;
    void invalidate_target(std::uint64_t session,
                           std::uint64_t generation,
                           identities::SquadReference) noexcept;
    void reset() noexcept;

private:
    struct Release final {
        identities::Source source{};
        std::uint8_t bubble{};
        std::uint64_t revision{};
        bool requireComplete{};
        bool invalidated{};
        struct Group final {
            identities::Token root{};
            Eligibility eligibility{};
            Mask entities{};
            std::vector<identities::Identity> captured;
        };
        std::vector<Group> groups;
    };
    std::vector<Release> releases_;
    std::uint64_t revision_{};
};
} // namespace sunrise::state::gameplay::squad_entity_retirement
