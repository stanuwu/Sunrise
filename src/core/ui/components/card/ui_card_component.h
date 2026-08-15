#pragma once

#include <imgui.h>

namespace sunrise::core::ui::components::card {

/** Stack scope that always balances a successfully started Dear ImGui card child. */
class [[nodiscard]] Scope final {
public:
    /**
     * Starts a padded card with a bounded hover transition.
     * @param id Stable non-null Dear ImGui child ID.
     * @param size Final framebuffer size; a zero axis takes the space left.
     */
    explicit Scope(const char* id, const ImVec2& size = {}) noexcept;

    /** Ends the child when construction started it in a current Dear ImGui context. */
    ~Scope() noexcept;

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

    /** @return True when Dear ImGui requests content submission for this card. */
    [[nodiscard]] bool visible() const noexcept;

private:
    bool started_{};
    bool visible_{};
};

} // namespace sunrise::core::ui::components::card
