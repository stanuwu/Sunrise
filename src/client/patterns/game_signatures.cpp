#include <array>
#include <string_view>

#include "game.h"
#include "game/retail_log/retail_log_signature_bytes.h"
#include "game/signon/signon_readiness_signature_bytes.h"
#include "signature_text.h"

namespace sunrise::client::patterns::game {
namespace {

// Matches the selector that distinguishes socket transport from SDR transport.
// Every displacement is wildcarded so the match carries no position-dependent bytes.
constexpr std::string_view kTransportKindText =
    "40 53 48 83 EC ? 80 3D ? ? ? ? 00 BB 01 00 00 00 74 ? 48 8B 0D ? ? ? ? 48 85 C9 74 ? 48 8B "
    "01 FF 50 60 84 C0 B9 02 00 00 00 0F 44 D9 8B C3 48 83 C4 ? 5B C3";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kTransportKind = signature<signature_length(kTransportKindText)>(kTransportKindText);

// Matches the executor that receives every queued HTTP request descriptor.
constexpr std::string_view kHttpExecuteRequestText =
    "48 8B C4 55 57 48 8D 68 ? 48 81 EC ? ? ? ? 48 89 70 ? 33 FF 48 8B F2 4C 89 70 ? 4C "
    "8B F1 40 38 7A 0D";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kHttpExecuteRequest =
    signature<signature_length(kHttpExecuteRequestText)>(kHttpExecuteRequestText);

// Matches the config fetch wrapper whose guarded call reaches the allocated-buffer GET path.
constexpr std::string_view kContentConfigFetchText =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 41 0F B6 F8 48 8B DA";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kContentConfigFetch =
    signature<signature_length(kContentConfigFetchText)>(kContentConfigFetchText);

// Matches the content boot tick that owns the phase field at byte offset 48.
// The prologue alone is not unique once the frame values are wildcarded, so the match runs
// on through the stack-cookie store and the first two register saves.
constexpr std::string_view kContentConfigTickText =
    "4C 8B DC 55 56 57 49 8D AB ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 48 89 85 "
    "? ? ? ? 48 8B F9 49 89 5B";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kContentConfigTick =
    signature<signature_length(kContentConfigTickText)>(kContentConfigTickText);

// Matches the guarded read of the signature-enable byte: the store, its preceding call,
// the rip-relative compare against zero, and the branch that skips verification.
// Every displacement is wildcarded so the match carries no position-dependent bytes.
constexpr std::string_view kContentManifestGateText =
    "4C 89 B4 24 ? ? ? ? E8 ? ? ? ? 80 3D ? ? ? ? 00 74";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kContentManifestGate =
    signature<signature_length(kContentManifestGateText)>(kContentManifestGateText);

// Matches the roster-prefix decoder that applies all 65 bubble authority lanes.
// The match runs on past the stack-cookie store because the prologue alone is not unique.
constexpr std::string_view kBubbleAuthorityDecoderText =
    "40 55 53 56 41 55 48 8D AC 24 ? ? ? ? 48 81 EC ? ? ? ? 48 8B 05 ? ? ? ? 48 33 C4 "
    "48 89 85 ? ? ? ? 4C 8B E9 48 89 BC 24";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kBubbleAuthorityDecoder =
    signature<signature_length(kBubbleAuthorityDecoderText)>(kBubbleAuthorityDecoderText);

// Matches the process build-state getter used by each bubble authority lane.
// Every displacement is wildcarded so the match carries no position-dependent bytes.
constexpr std::string_view kContentUntrackedGetterText =
    "48 83 EC ? C7 44 24 ? 00 00 00 00 0F B6 05 ? ? ? ? 85 C0 74 ? 0F B6 05 ? ? ? ?";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kContentUntrackedGetter =
    signature<signature_length(kContentUntrackedGetterText)>(kContentUntrackedGetterText);

// Matches the 6-slot object resolver whose first instruction names the schema tables.
constexpr std::string_view kQueuezObjectResolverText =
    "4C 8B 1D ? ? ? ? 45 33 C9 48 63 C2 45 8B D0 48 05 1A 25 00 00 48 8D 14 40 48 C1 E2 05";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kQueuezObjectResolver =
    signature<signature_length(kQueuezObjectResolverText)>(kQueuezObjectResolverText);

// Matches the family-5 subscription path whose second call returns the object store.
constexpr std::string_view kQueuezFamily5SubscribeText =
    "40 57 48 83 EC ? 48 8B F9 E8 ? ? ? ? 0F B6 47 50 84 C0 75 ? E8 ? ? ? ?";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kQueuezFamily5Subscribe =
    signature<signature_length(kQueuezFamily5SubscribeText)>(kQueuezFamily5SubscribeText);

// Matches the config-init site that loads the bootstrap content-id token. The token address
// comes from the wildcarded rip-relative operand, so no token bytes live in Sunrise.
constexpr std::string_view kContentIdTokenLoadText =
    "0F 10 44 24 ? 41 B8 14 00 00 00 48 8D 15 ? ? ? ? 0F 10 4C 24 ? 48 8D 8C 24 ? ? ? ?";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kContentIdTokenLoad =
    signature<signature_length(kContentIdTokenLoadText)>(kContentIdTokenLoadText);

// Matches the item-stat lookup entry used by equipment scoring. The prologue alone is not
// unique once the frame size is wildcarded, so the match runs on into the argument moves.
constexpr std::string_view kGetItemStatValueText =
    "40 53 56 41 54 41 56 41 57 48 83 EC ? 45 33 E4 41 0F B6 D9 4C 8B 8C 24 ? ? ? ? 4D 8B F0 "
    "44 89 21";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kGetItemStatValue =
    signature<signature_length(kGetItemStatValueText)>(kGetItemStatValueText);

// Matches the light-level conversion entry used by equipment scoring.
constexpr std::string_view kLightValueToScalarText =
    "48 89 5C 24 ? 48 89 74 24 ? 57 48 83 EC ? 0F 29 74 24 ? 41 0F B6 D8";
/** Compiled pattern bytes of the signature text above. */
constexpr auto kLightValueToScalar =
    signature<signature_length(kLightValueToScalarText)>(kLightValueToScalarText);

/** Game signatures in the exact target-slot order. */
constexpr std::array kDefinitions{
    patterns::Pattern{"transport_kind", kTransportKind},
    patterns::Pattern{"http_execute_request", kHttpExecuteRequest},
    patterns::Pattern{"signon_readiness_failure", signon::kReadinessFailure},
    patterns::Pattern{"signon_readiness_ready", signon::kReadinessReady},
    patterns::Pattern{"content_config_fetch", kContentConfigFetch},
    patterns::Pattern{"content_config_tick", kContentConfigTick},
    patterns::Pattern{"content_manifest_gate", kContentManifestGate},
    patterns::Pattern{"bubble_authority_decoder", kBubbleAuthorityDecoder},
    patterns::Pattern{"content_untracked_getter", kContentUntrackedGetter},
    patterns::Pattern{"content_id_token_load", kContentIdTokenLoad},
    patterns::Pattern{"queuez_object_resolver", kQueuezObjectResolver},
    patterns::Pattern{"queuez_family5_subscribe", kQueuezFamily5Subscribe},
    patterns::Pattern{"get_item_stat_value", kGetItemStatValue},
    patterns::Pattern{"light_value_to_scalar", kLightValueToScalar},
    patterns::Pattern{"retail_log_enqueue", retail_log::kEnqueue},
    patterns::Pattern{"retail_log_set_category_verbosity", retail_log::kSetCategoryVerbosity},
};

static_assert(kDefinitions.size() == static_cast<std::size_t>(Id::count));

} // namespace

/** @return Immutable Game signature definitions in target-slot order. */
std::span<const patterns::Pattern> definitions() noexcept {
    return kDefinitions;
}

} // namespace sunrise::client::patterns::game
