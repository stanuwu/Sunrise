#pragma once

#include <cstdint>

#include "../../../middleware/gameplay/external/external_empty_profile.h"
#include "../../../middleware/gameplay/peer/established_packet.h"
#include "../../../state/gameplay/external/definition.h"

namespace sunrise::server::gameplay::peer {

/** Result of preparing one receive-only external observation. */
enum class ShadowPrepareResult : std::uint8_t {
    ready,
    missingCommon,
    unsupportedCommon,
};

/** Fully validated receive-only external body and its outer trailer. */
struct EmptyFrame {
    middleware::gameplay::external::EmptyProfile profile{};
    middleware::gameplay::peer::FillerTrailer filler{};
};

/**
 * Reads one complete empty-channel frame without changing output on failure.
 * TODO: no caller yet. Both entry points here wait on the `gameplay_external_body` gate.
 */
[[nodiscard]] middleware::gameplay::external::EmptyProfileResult
read_empty_frame(middleware::encoding::bits::Reader& reader, EmptyFrame& output) noexcept;

/** Prepares the next shadow without changing current state. */
[[nodiscard]] ShadowPrepareResult
prepare_external_shadow(const state::gameplay::external::ExternalShadowKey& currentContext,
                        const middleware::gameplay::external::EmptyProfile& decoded,
                        const state::gameplay::external::ExternalShadow& current,
                        state::gameplay::external::ExternalShadow& next) noexcept;

} // namespace sunrise::server::gameplay::peer
