#pragma once

#include <array>
#include <cstdio>

#include "../../../../middleware/bap/activity_host_manager/response/activity_manager_response.h"

namespace sunrise::server::bap::encrypted::activity_host_manager {

/** Publish the allocated host's native ActivityClient name in the opaque service-7 data. */
[[nodiscard]] inline bool make_establish_response(
    std::uint64_t sessionId,
    middleware::bap::activity_host_manager::response::Response& response) noexcept {
    response = {};
    if (sessionId == 0) {
        return false;
    }
    std::array<char, middleware::bap::activity_host_manager::response::kActivityDataSize> name{};
    const int count = std::snprintf(name.data(),
                                    name.size(),
                                    "%08X:%08X@sunrise-activity-host",
                                    static_cast<unsigned>(sessionId >> 32),
                                    static_cast<unsigned>(sessionId & 0xFFFFFFFFULL));
    if (count <= 0 || static_cast<std::size_t>(count) >= name.size()) {
        return false;
    }
    response.sessionId = sessionId;
    for (std::size_t index = 0; index < static_cast<std::size_t>(count); ++index) {
        response.activityData[index] = static_cast<std::byte>(name[index]);
    }
    return true;
}

} // namespace sunrise::server::bap::encrypted::activity_host_manager
