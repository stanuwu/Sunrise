#pragma once

#include "../../middleware/bap/activity_message/damage_monitor_auth.h"
#include "../../middleware/bap/activity_message/ghost_link_auth.h"
#include "../../middleware/bap/activity_message/interactable_object_auth.h"
#include "../../middleware/bap/activity_message/mission_effect_auth.h"
#include "host_runtime_actor_program.h"

namespace sunrise::server::activity::host::counter_auth {

/** Only these known control bodies may have a host-chosen generation substituted. */
[[nodiscard]] inline bool compatible(ScriptableOverrideKind kind,
                                     const ScriptableTarget& target) noexcept {
    namespace auth = middleware::bap::activity_message::scriptable_auth;
    namespace damage = middleware::bap::activity_message::damage_monitor;
    namespace ghost = middleware::bap::activity_message::ghost_link;
    namespace hop_on = middleware::bap::activity_message::mission_effect;
    return ((kind == ScriptableOverrideKind::interactableObject
             || kind == ScriptableOverrideKind::interactableObjectInPlace)
            && target.slotType == auth::kType4SlotType && target.authSchema == auth::kType4Schema)
           || ((kind == ScriptableOverrideKind::combatantControls
                || kind == ScriptableOverrideKind::combatantDelivery)
               && target.slotType == auth::kType2SlotType
               && target.authSchema == auth::kType2Schema)
           || (kind == ScriptableOverrideKind::damageWatch && target.slotType == damage::kSlotType
               && target.authSchema == damage::kAuthSchema)
           || (kind == ScriptableOverrideKind::ghostLink && target.slotType == ghost::kSlotType
               && target.authSchema == ghost::kAuthSchema)
           || (kind == ScriptableOverrideKind::hopOn && target.slotType == hop_on::kSlotType
               && target.authSchema == hop_on::kSchema);
}

/** Reads the signed counter from a transported body; absent or negative state starts at zero. */
[[nodiscard]] inline bool previous_revision(const detail::Instance& instance,
                                            const ScriptableTarget& target,
                                            std::size_t offset,
                                            std::uint32_t& output) noexcept {
    namespace fields = middleware::bap::activity_message::auth_fields;
    output = 0;
    const auto* previous = actor_program::retained(instance, target);
    if (previous == nullptr) {
        return true;
    }
    if (offset + 32 > previous->bitCount) {
        return false;
    }
    middleware::encoding::bits::Reader reader(std::span(previous->body).first(previous->byteCount));
    std::uint64_t value = 0;
    if (!reader.skip(offset) || !reader.read(32, value)) {
        return false;
    }
    if (value > fields::kSigned32Bias) {
        output = static_cast<std::uint32_t>(value - fields::kSigned32Bias);
    }
    return true;
}

/** Substitutes one fresh counter while retaining every requested non-counter bit. */
[[nodiscard]] inline bool encode(const detail::Instance& instance,
                                 const detail::ScriptableRequest& request,
                                 const detail::ScriptableGuard& guard,
                                 PendingScriptableOverride& pending,
                                 std::size_t& written) noexcept {
    namespace fields = middleware::bap::activity_message::auth_fields;
    namespace patch = middleware::bap::activity_message::mission_auth_patch;
    const bool inPlace = request.kind == ScriptableOverrideKind::interactableObjectInPlace;
    const bool object = request.kind == ScriptableOverrideKind::interactableObject || inPlace;
    const bool hopOn = request.kind == ScriptableOverrideKind::hopOn;
    const std::size_t offset = object  ? 0
                               : hopOn ? middleware::bap::activity_message::mission_effect::
                                             kRevisionOffset
                                       : fields::kClientRefBits;
    std::uint32_t previous = 0;
    // Ghost link is compatible but keeps its own encoder, so it must not take the damage offset.
    if ((!object && !hopOn && request.kind != ScriptableOverrideKind::damageWatch)
        || !compatible(request.kind, request.target) || request.authBitCount < offset + 32
        || !previous_revision(instance, request.target, offset, previous)) {
        return false;
    }
    const auto committed = object ? static_cast<std::uint32_t>((std::max)(guard.type4.last, 0))
                           : hopOn ? guard.hopOnRevision
                                   : guard.damageRevision;
    const auto last = (std::max)(previous, committed);
    // An in-place body needs a live entry to update.
    if (last >= fields::kMaximumCounter || (inPlace && last == 0)) {
        return false;
    }
    const auto next = inPlace ? last : last + 1;
    const auto body = std::span(request.authBody).first(request.authByteCount);
    middleware::encoding::bits::Writer writer(pending.body);
    if (!patch::copy_field(writer, body, {0, offset, true})
        || !writer.write(next + fields::kSigned32Bias, 32)
        || !patch::copy_field(writer, body, {offset + 32, request.authBitCount - offset - 32, true})
        || !writer.finish(written)) {
        return false;
    }
    pending.bitCount = request.authBitCount;
    pending.generation = next;
    return true;
}

} // namespace sunrise::server::activity::host::counter_auth
