#pragma once

namespace sunrise::client::hooks::network::bubble_authority {

/** @return The roster-prefix decoder replacement body. */
[[nodiscard]] void* decoder_entry_point() noexcept;

/** @return The content-untracked getter replacement body. */
[[nodiscard]] void* content_untracked_entry_point() noexcept;

} // namespace sunrise::client::hooks::network::bubble_authority
