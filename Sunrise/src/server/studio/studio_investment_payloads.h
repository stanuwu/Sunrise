#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "../../middleware/studio/studio_investment_wire.h"
#include "../bap/investment_operations.h"

namespace sunrise::server::studio {

/** The response payload area every investment encoder writes into. */
using InvestmentPayload =
    std::span<std::byte, middleware::studio::investment_wire::kResponsePayloadBytes>;

/** Everything one investment answer carries besides its payload bytes. */
struct InvestmentAnswer {
    middleware::studio::investment_wire::Status status{};
    middleware::studio::investment_wire::Context context{};
    /** Static ASCII label: why the request was refused, or the label its receipt carries. */
    const char* reason{""};
    /** The opcode's fixed payload size once the payload is written, else zero. */
    std::uint32_t payloadBytes{};
};

/** @return An invalid-request answer with one static label and no context or payload. */
[[nodiscard]] InvestmentAnswer invalid(const char* reason) noexcept;

/** @return A refusal with one static label and no context or payload. */
[[nodiscard]] InvestmentAnswer refused(const char* reason) noexcept;

/** Encodes the live account, catalog readiness and publication peer an editor binds to. */
[[nodiscard]] InvestmentAnswer encode_hello(InvestmentPayload payload) noexcept;

/** Encodes the profile and the selected character's items from one saved account copy. */
[[nodiscard]] InvestmentAnswer encode_inventory_snapshot(InvestmentPayload payload) noexcept;

/** Encodes one coherent Season pass view, with every catalog row evaluated without a draw. */
[[nodiscard]] InvestmentAnswer encode_season_pass_snapshot(InvestmentPayload payload) noexcept;

/** Encodes the selected character's held pursuits and their saved completion status. */
[[nodiscard]] InvestmentAnswer encode_pursuit_snapshot(InvestmentPayload payload) noexcept;

/**
 * Encodes the selected character's saved flag and value rows the request names.
 * @return Invalid with request_field when a source is malformed.
 */
[[nodiscard]] InvestmentAnswer
encode_unlock_values(const middleware::studio::investment_wire::UnlockValuesRequest& request,
                     InvestmentPayload payload) noexcept;

/** Encodes a receipt bound to its operation's target. An empty receipt encodes as unknown. */
[[nodiscard]] InvestmentAnswer encode_receipt(const bap::InvestmentReceipt& receipt,
                                              InvestmentPayload payload) noexcept;

} // namespace sunrise::server::studio
