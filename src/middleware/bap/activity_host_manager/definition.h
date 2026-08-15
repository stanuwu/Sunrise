#pragma once

#include <cstddef>
#include <span>

namespace sunrise::middleware::bap::activity_host_manager {

/** Checked service-6 request fields, borrowed from the caller's body. */
struct Request final {
    /** Holds credentials. Never keep, log, capture or save this view. */
    std::span<const std::byte> protobuf{};
};

} // namespace sunrise::middleware::bap::activity_host_manager
