#include <mutex>

#include "../../core/logging/log.h"
#include "../build_data/runtime.h"
#include "../investment/store_internal.h"
#include "bucket_admission.h"
#include "runtime.h"
#include "state_account_transaction_helpers.h"

namespace sunrise::state {
namespace family4_loadout = middleware::datagen::family4::loadout;
namespace item_details = build_data::items::details;
using namespace runtime::detail;

/** The first matching installed transfer rule controls a vendor claim. */
[[nodiscard]] static bool accepted_claim_rule(std::uint16_t vendorIndex,
                                              std::uint8_t sourceBucket) noexcept {
    std::uint8_t destination = 0;
    return build_data::find_vendor_transfer_destination(vendorIndex, sourceBucket, destination)
           && destination == build_data::vendors::kAuthoredDestination;
}

/** Logs why a claim was refused, naming it as the client's request did. */
static void report_claim_refusal(const char* reason,
                                 std::uint16_t vendorIndex,
                                 std::uint8_t sourceBucket,
                                 std::uint16_t definitionIndex,
                                 std::int32_t quantity) noexcept {
    core::log::writef(core::log::Channel::state,
                      core::log::Level::warn,
                      "ev=postmaster_claim stage=prepare result=refused reason=%s vendor=%u "
                      "source=%u item=%u quantity=%d",
                      reason,
                      static_cast<unsigned>(vendorIndex),
                      static_cast<unsigned>(sourceBucket),
                      static_cast<unsigned>(definitionIndex),
                      static_cast<int>(quantity));
}

/** Stages one claim against the saved account without writing it. */
bool prepare_postmaster_claim(std::uint16_t vendorIndex,
                              std::uint8_t sourceBucket,
                              std::uint64_t instanceSoid,
                              std::uint16_t definitionIndex,
                              std::int32_t quantity,
                              PendingPostmasterClaim& mutation) noexcept {
    mutation = {};
    const std::lock_guard lock(investment::store::g_mutex);
    // Only a rule returning the source to each item's authored bucket is a Postmaster claim.
    if (!accepted_claim_rule(vendorIndex, sourceBucket)) {
        report_claim_refusal("claim_rule", vendorIndex, sourceBucket, definitionIndex, quantity);
        return false;
    }
    AccountState working = account_snapshot();
    const std::size_t characterIndex = selected_character_index(working);
    build_data::items::Definition item{};
    item_details::Definition detail{};
    if (!account::valid(working) || !valid_profile_inventory(working)
        || characterIndex >= working.characterCount
        || !build_data::find_item_definition_index(definitionIndex, item)
        || !build_data::find_configured_item_detail(definitionIndex, detail)
        || detail.definitionIndex != item.definitionIndex) {
        return false;
    }
    record_grant_before(working, characterIndex, mutation.grant);
    PlacedRows placed;
    const char* reason = nullptr;
    family4_loadout::ResolvedLoadout loadout{};
    const bool moved = place_claim(working,
                                   characterIndex,
                                   sourceBucket,
                                   item,
                                   detail,
                                   instanceSoid,
                                   quantity,
                                   placed,
                                   reason);
    if (!moved || !account::valid(working) || !valid_profile_inventory(working)
        || !family4_loadout::resolve(working, characterIndex, loadout)) {
        report_claim_refusal(moved ? "claim_after_image" : reason,
                             vendorIndex,
                             sourceBucket,
                             definitionIndex,
                             quantity);
        return false;
    }
    record_grant_after(working, placed, mutation.grant);
    mutation.grant.vendorTransfer = true;
    mutation.instanceSoid = instanceSoid;
    mutation.quantity = quantity;
    mutation.vendorIndex = vendorIndex;
    mutation.definitionIndex = definitionIndex;
    mutation.sourceBucket = sourceBucket;
    mutation.prepared = true;
    return true;
}

/** Writes the claim after re-checking its vendor rule and saved before-image in one transaction. */
bool commit_postmaster_claim(PendingPostmasterClaim& mutation) noexcept {
    const PendingConsumption consume{mutation};
    return mutation.prepared && mutation.grant.vendorTransfer
           && accepted_claim_rule(mutation.vendorIndex, mutation.sourceBucket)
           && commit_record_reward(mutation.grant);
}

} // namespace sunrise::state
