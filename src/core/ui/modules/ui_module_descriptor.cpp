#include "ui_module_descriptor.h"

#include <algorithm>

namespace sunrise::core::ui::modules {
namespace {

/** @return True for a defined owner. */
[[nodiscard]] bool is_valid_owner(Owner owner) noexcept {
    return owner == Owner::client || owner == Owner::server || owner == Owner::core;
}

/** @return True for a byte the stable ID alphabet allows. */
[[nodiscard]] bool is_stable_id_byte(char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.'
           || value == '_';
}

/**
 * Checks the module ID grammar. The ID is saved, so its alphabet stays fixed.
 * @param stableId Candidate ID, lowercase.
 * @return True when the ID fits the size limit and starts with a lowercase letter.
 */
[[nodiscard]] bool is_valid_stable_id(std::string_view stableId) noexcept {
    if (stableId.empty() || stableId.size() > kStableIdCapacity) {
        return false;
    }
    if (stableId.front() < 'a' || stableId.front() > 'z') {
        return false;
    }
    return std::all_of(stableId.begin(), stableId.end(), is_stable_id_byte);
}

/**
 * Checks one UI label. It sets no encoding rule for the renderer.
 * @return True when the label is not empty, fits the size limit and has no null byte.
 */
[[nodiscard]] bool is_valid_display_name(std::string_view displayName) noexcept {
    return !displayName.empty() && displayName.size() <= kDisplayNameCapacity
           && displayName.find('\0') == std::string_view::npos;
}

} // namespace

/** @return The domain that owns the module. */
Owner Descriptor::owner() const noexcept {
    return owner_;
}

/** @return The stable lowercase module ID. */
std::string_view Descriptor::stable_id() const noexcept {
    return {stableId_.data(), stableIdLength_};
}

/** @return The user-facing module name. */
std::string_view Descriptor::display_name() const noexcept {
    return {displayName_.data(), displayNameLength_};
}

/** @return The frame entry. It is called only while its owner panel is selected. */
FrameCallback Descriptor::frame_callback() const noexcept {
    return frameCallback_;
}

/**
 * Builds one descriptor by value.
 * @param stableId Lowercase id, used for registration and for saved state.
 * @param frameCallback Static entry, valid until unregister or shutdown.
 * @param output Receives the descriptor. Left alone when a field is rejected.
 * @return True when every field is valid and fits the fixed storage.
 */
bool create_descriptor(Owner owner,
                       std::string_view stableId,
                       std::string_view displayName,
                       FrameCallback frameCallback,
                       Descriptor& output) noexcept {
    if (!is_valid_owner(owner) || !is_valid_stable_id(stableId)
        || !is_valid_display_name(displayName) || frameCallback == nullptr) {
        return false;
    }

    Descriptor candidate;
    candidate.owner_ = owner;
    std::copy(stableId.begin(), stableId.end(), candidate.stableId_.begin());
    candidate.stableIdLength_ = stableId.size();
    std::copy(displayName.begin(), displayName.end(), candidate.displayName_.begin());
    candidate.displayNameLength_ = displayName.size();
    candidate.frameCallback_ = frameCallback;
    output = candidate;
    return true;
}

/** @return True when every field is complete. */
bool is_valid(const Descriptor& descriptor) noexcept {
    return is_valid_owner(descriptor.owner_) && is_valid_stable_id(descriptor.stable_id())
           && is_valid_display_name(descriptor.display_name())
           && descriptor.frameCallback_ != nullptr;
}

} // namespace sunrise::core::ui::modules
