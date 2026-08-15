#pragma once

namespace sunrise::state::build_data::runtime {

/** Ability bucket domain publish marker. */
namespace ability_buckets {

/** Clears the marker. */
void clear() noexcept;

/** Marks the domain complete. An empty domain counts as complete. */
void publish() noexcept;

/** @return True once a complete domain has been published. */
[[nodiscard]] bool ready() noexcept;

} // namespace ability_buckets

/** Configured item detail domain publish marker. */
namespace details {

/** Clears the marker. */
void clear() noexcept;

/** Marks the domain complete. An empty domain counts as complete. */
void publish() noexcept;

/** @return True once a complete domain has been published. */
[[nodiscard]] bool ready() noexcept;

} // namespace details

/** Bubble-name table publish marker. */
namespace name_catalog {

/** Clears the marker. */
void clear() noexcept;

/** Marks the domain complete. An empty domain counts as complete. */
void publish() noexcept;

/** @return True once a complete domain has been published. */
[[nodiscard]] bool ready() noexcept;

} // namespace name_catalog

/** Spawn-set catalog publish marker. */
namespace spawn_catalog {

/** Clears the marker. */
void clear() noexcept;

/** Marks the domain complete. An empty domain counts as complete. */
void publish() noexcept;

/** @return True once a complete domain has been published. */
[[nodiscard]] bool ready() noexcept;

} // namespace spawn_catalog

/** Named catalog publish marker. */
namespace named {

/** Clears the marker. */
void clear() noexcept;

/** Marks the already-populated catalog complete. */
void publish() noexcept;

/** @return True when the current catalog is published as complete. */
[[nodiscard]] bool ready() noexcept;

} // namespace named

} // namespace sunrise::state::build_data::runtime
