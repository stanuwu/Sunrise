#pragma once
#include "../../state/activity/runtime.h"
#include "../../state/gameplay/external/squad_entity_retirement.h"
namespace sunrise::server::gameplay::entity_identities {
class PublicationLease;
}
namespace sunrise::server::activity::host {
struct PendingScriptableOverride;
}
namespace sunrise::state::activity_sdk {
struct BoundView;
}
namespace sunrise::state::build_data::scriptables {
struct Snapshot;
}
namespace sunrise::server::gameplay::squad_entity_retirement {
using RetirementPlan = state::gameplay::squad_entity_retirement::RetirementPlan;
enum class TransitionStatus : std::uint8_t { refused, pending, ready };
/** A prop reset must finish publication before its script can move to the next state. */
[[nodiscard]] TransitionStatus
begin_placed_transition(const state::activity_sdk::BoundView&,
                        const state::build_data::scriptables::Snapshot&,
                        std::uint64_t transition,
                        std::int32_t fromRegion,
                        std::int32_t toRegion) noexcept;
/** A pending host-owned prop lifetime change owes a retirement publication. */
[[nodiscard]] bool placed_transition_pending(const state::activity::SessionBinding&,
                                             std::uint64_t generation) noexcept;
/** Cancelling a mission withdraws only its pending prop lifetime decision. */
void cancel_placed_transition(const state::activity::SessionBinding&,
                              std::uint64_t generation,
                              std::uint64_t transition = 0) noexcept;
/** The authenticated abdication freezes exact identities before a later renewal can retire them. */
void observe_abdication(const state::activity::SessionBinding&,
                        std::uint64_t generation,
                        std::uint8_t bubble,
                        const state::activity::bubble_authority::EntitySlotMask&) noexcept;
/** Returned slots cannot remain eligible for an earlier release. */
void returned_slots(const state::activity::SessionBinding&,
                    std::uint64_t generation,
                    const state::activity::bubble_authority::EntitySlotMask&) noexcept;
/** Renewal preparation never consumes retained entities. */
[[nodiscard]] bool prepare_retirement(const state::activity::SessionBinding&,
                                      std::uint64_t generation,
                                      std::uint8_t bubble,
                                      RetirementPlan&) noexcept;
/** Publication must revalidate the exact staged source, mask, and revision. */
[[nodiscard]] bool validate_retirement(const state::activity::SessionBinding&,
                                       std::uint64_t generation,
                                       const RetirementPlan&) noexcept;
/** Pins exact identity state through transport publication after the last policy validation. */
[[nodiscard]] bool begin_retirement_publication(const state::activity::SessionBinding&,
                                                std::uint64_t generation,
                                                const RetirementPlan&,
                                                entity_identities::PublicationLease&) noexcept;
/** Commits only after the complete carrying transport publication succeeds. */
void commit_retirement(const RetirementPlan&) noexcept;
/** Authored opt-in becomes eligible only after its positive Auth body was delivered. */
void record_delivered_target(const state::activity::SessionBinding&,
                             std::uint64_t generation,
                             const activity::host::PendingScriptableOverride&) noexcept;
/**
 * Moves a squad's target to the link that delivered its body while that link is live, else to the
 * link whose report was admitted for the squad, so that link's next abdication captures it.
 * @param live The route and every joined private link of the binding.
 */
void follow_squad(const state::activity::SessionBinding&,
                  std::uint64_t generation,
                  state::gameplay::entity_identity::SquadReference,
                  std::span<const std::uint64_t> live) noexcept;
void reset() noexcept;
} // namespace sunrise::server::gameplay::squad_entity_retirement
