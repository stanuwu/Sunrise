#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace sunrise::core::ui::modules {

/** Ownership keeps Core, Client and Server pages in separate menu groups. */
enum class Owner : std::uint8_t {
    client,
    server,
    core,
};

/** Main-menu pages stay in the upstream sidebar; launched tools occupy workspace tabs. */
enum class Presentation : std::uint8_t {
    mainMenu,
    workspaceTab,
};

/** 48 bytes fit a namespaced stable ID and keep every slot the same size. */
inline constexpr std::size_t kStableIdCapacity = 48;
/** 64 bytes hold one short menu label with no dynamic storage. */
inline constexpr std::size_t kDisplayNameCapacity = 64;

/** Frame entry of one statically linked UI module. It does not depend on the renderer. */
using FrameCallback = void (*)() noexcept;

/** UI module details, copied into the fixed registry and never changed after. */
class Descriptor final {
public:
    Descriptor() noexcept = default;

    /** @return The domain that owns the module. */
    [[nodiscard]] Owner owner() const noexcept;

    /** @return The stable lowercase module ID. */
    [[nodiscard]] std::string_view stable_id() const noexcept;

    /** @return The user-facing module name. */
    [[nodiscard]] std::string_view display_name() const noexcept;

    /** @return The frame entry. It is called only while its owner panel is selected. */
    [[nodiscard]] FrameCallback frame_callback() const noexcept;

    /** @return The optional companion-window entry called on every visible UI frame. */
    [[nodiscard]] FrameCallback companion_frame_callback() const noexcept;

    /** Relinquishes state on main-menu deselection, or on a workspace tool's close/removal. */
    [[nodiscard]] Descriptor with_deactivation(FrameCallback callback) const noexcept;
    [[nodiscard]] FrameCallback deactivation_callback() const noexcept;

    /** Copies the descriptor with its workspace placement. */
    [[nodiscard]] Descriptor with_presentation(Presentation presentation) const noexcept;

    /** @return Whether this page belongs to the main menu or a launched workspace tab. */
    [[nodiscard]] Presentation presentation() const noexcept;

private:
    friend bool create_descriptor(Owner owner,
                                  std::string_view stableId,
                                  std::string_view displayName,
                                  FrameCallback frameCallback,
                                  FrameCallback companionFrameCallback,
                                  Descriptor& output) noexcept;
    /** @return True when every field is complete. */
    friend bool is_valid(const Descriptor& descriptor) noexcept;

    Owner owner_{Owner::client};
    std::array<char, kStableIdCapacity> stableId_{};
    std::size_t stableIdLength_{};
    std::array<char, kDisplayNameCapacity> displayName_{};
    std::size_t displayNameLength_{};
    FrameCallback frameCallback_{};
    FrameCallback companionFrameCallback_{};
    FrameCallback deactivationCallback_{};
    Presentation presentation_{Presentation::mainMenu};
};

/**
 * Builds one descriptor by value.
 * @param stableId Lowercase id, used for registration and for saved state.
 * @param frameCallback Static entry, valid until unregister or shutdown.
 * @param output Receives the descriptor. Left alone when a field is rejected.
 * @return True when every field is valid and fits the fixed storage.
 */
[[nodiscard]] bool create_descriptor(Owner owner,
                                     std::string_view stableId,
                                     std::string_view displayName,
                                     FrameCallback frameCallback,
                                     Descriptor& output) noexcept;

/** Builds one descriptor with an optional companion-window frame entry. */
[[nodiscard]] bool create_descriptor(Owner owner,
                                     std::string_view stableId,
                                     std::string_view displayName,
                                     FrameCallback frameCallback,
                                     FrameCallback companionFrameCallback,
                                     Descriptor& output) noexcept;

/** @return True when every field is complete. */
[[nodiscard]] bool is_valid(const Descriptor& descriptor) noexcept;

} // namespace sunrise::core::ui::modules
