#include "matchmaking_request_parser.h"

#include "field_selection.h"

namespace sunrise::middleware::bap::matchmaking::request {

/** Parses one service-42 protobuf body. It keeps no reference to the storage. */
Request parse(std::span<const std::byte> input) noexcept {
    Root root;
    if (!parse_root(input, root)) {
        return {};
    }

    Request parsed;
    parsed.kind = root.kind;
    if (parsed.kind != RequestKind::advertisementUpdate || !root.hasAdvertisementMessage) {
        return parsed;
    }
    if (!parse_update(root.advertisementMessage, parsed.advertisement)) {
        return {};
    }
    return parsed;
}

} // namespace sunrise::middleware::bap::matchmaking::request
