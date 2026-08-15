#include <array>
#include <cstdio>

#include "../../../../core/logging/log.h"
#include "../internal.h"

namespace sunrise::server::bap::encrypted::diagnostics {

/**
 * Logs one request the Server could not answer as meant.
 * A reply-mode request left unanswered jams the Client's pending ring for good, and the next
 * reply on any service is the one it rejects. Nothing on this path may fail silently.
 * @param service Numeric request service from the decrypted inner header.
 * @param stage Short name of the step that refused.
 */
void report_failure(std::uint16_t service, std::string_view stage) noexcept {
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=bap svc=%u stage=%.*s result=fail",
                                      static_cast<unsigned>(service),
                                      static_cast<int>(stage.size()),
                                      stage.data());
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::warn,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

} // namespace sunrise::server::bap::encrypted::diagnostics
