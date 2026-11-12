#include "studio_investment_link.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "../../core/logging/log.h"
#include "../../core/runtime/host_environment.h"
#include "../../core/settings/settings.h"
#include "../../middleware/studio/studio_investment_wire.h"
#include "../../middleware/studio/studio_mapping.h"
#include "../../state/runtime/runtime.h"
#include "../bap/investment_operations.h"
#include "studio_investment_payloads.h"

namespace sunrise::server::studio {
namespace {

namespace wire = middleware::studio::investment_wire;

/** The last status and label one read opcode answered with. */
struct ReadAnswer {
    wire::Status status{};
    std::string_view reason{};
};

HANDLE g_mapping{};
wire::SharedInvestment* g_view{};
std::uint64_t g_nextConnect{};
/** The connected editor's process id, which tags each operation it submits. */
std::uint32_t g_editorPid{};
std::array<ReadAnswer, static_cast<std::size_t>(wire::Opcode::count)> g_lastReads{};

/** Refuses pending operations. Finished receipts outlive the link for the editor to read later. */
void disconnect() noexcept {
    if (g_view != nullptr) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::info,
                         "ev=studio stage=investment result=disconnected");
        bap::refuse_pending_investment_operations("editor_disconnected");
        g_lastReads = {};
    }
    middleware::studio::close_editor_view(g_mapping, g_view);
}

/** @return This process's creation FILETIME, or 0. The time tells a reused process id apart. */
[[nodiscard]] std::uint64_t process_started() noexcept {
    FILETIME creation{};
    FILETIME exit{};
    FILETIME kernel{};
    FILETIME user{};
    if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == FALSE) {
        return 0;
    }
    ULARGE_INTEGER started{};
    started.LowPart = creation.dwLowDateTime;
    started.HighPart = creation.dwHighDateTime;
    return started.QuadPart;
}

/** @return True only when an editor created a mapping of this exact protocol. */
[[nodiscard]] bool connect() noexcept {
    // Every protocol version starts with this control block, so map it alone to read the version.
    wire::Control* control =
        middleware::studio::open_editor_view<wire::Control>(wire::kMappingFormat, g_mapping);
    if (control == nullptr) {
        return false;
    }
    // Studio reads gameVersion to explain a version mismatch.
    if (control->magic == wire::kMagic) {
        control->gamePid = GetCurrentProcessId();
        control->gameVersion = wire::kVersion;
    }
    const bool accepted = middleware::studio::editor_header_ok(
        *control, wire::kMagic, wire::kVersion, wire::kMappingBytes);
    const std::uint32_t editorPid = control->editorPid;
    UnmapViewOfFile(control);
    const std::uint64_t started = accepted ? process_started() : 0;
    if (started != 0) {
        g_view = middleware::studio::map_editor_view<wire::SharedInvestment>(g_mapping);
    }
    if (g_view == nullptr) {
        disconnect();
        return false;
    }
    bap::forget_exited_editor_receipts(core::runtime::process_stopped);
    g_editorPid = editorPid;
    g_view->control.gameStarted = started;
    core::log::write(core::log::Channel::server,
                     core::log::Level::info,
                     "ev=studio stage=investment result=connected");
    return true;
}

/** @return The request or response id, read with a full barrier. */
[[nodiscard]] std::uint32_t read_id(volatile std::int32_t& id) noexcept {
    return static_cast<std::uint32_t>(
        InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&id), 0, 0));
}

/** A mutation must name every part of the context it was read against. */
[[nodiscard]] bool context_complete(const wire::Context& context) noexcept {
    return context.accountSoid != 0 && context.characterSoid != 0 && context.contentHash != 0;
}

/** Requests other than mutations must leave the context empty. */
[[nodiscard]] bool context_empty(const wire::Context& context) noexcept {
    return context.accountSoid == 0 && context.characterSoid == 0 && context.contentHash == 0;
}

/**
 * Checks the shape every request of an advertised opcode shares.
 * @return Null when the block can be served, else the label of an invalid request.
 */
[[nodiscard]] const char* malformed(const wire::Request& request) noexcept {
    if (request.payloadBytes != wire::request_bytes(request.opcode)) {
        return "request_size";
    }
    const auto unused = std::span(request.payload).subspan(request.payloadBytes);
    if (std::any_of(
            unused.begin(), unused.end(), [](std::byte value) { return value != std::byte{}; })) {
        return "request_padding";
    }
    const bool identified =
        wire::is_mutation(request.opcode) || wire::is_receipt_request(request.opcode);
    if ((request.operationId != 0) != identified) {
        return "request_operation";
    }
    if (wire::is_mutation(request.opcode) ? !context_complete(request.expected)
                                          : !context_empty(request.expected)) {
        return "request_context";
    }
    return nullptr;
}

/** Copies a typed payload out of the request copy. */
template <typename Payload>
[[nodiscard]] Payload payload_of(const wire::Request& request) noexcept {
    static_assert(sizeof(Payload) <= wire::kRequestPayloadBytes);
    Payload payload{};
    std::memcpy(&payload, request.payload.data(), sizeof(Payload));
    return payload;
}

/** @return False for a scope value the wire does not define. */
[[nodiscard]] bool edit_scope(wire::InventoryScope scope, state::GrantedRowArray& output) noexcept {
    switch (scope) {
    case wire::InventoryScope::profile:
        output = state::GrantedRowArray::profile;
        return true;
    case wire::InventoryScope::characterItem:
        output = state::GrantedRowArray::characterItem;
        return true;
    case wire::InventoryScope::characterStack:
        output = state::GrantedRowArray::characterStack;
        return true;
    }
    return false;
}

/** @return False when a reserved or enumerated field holds a value the opcode never sends. */
[[nodiscard]] bool decode_operation(const wire::Request& request,
                                    bap::InvestmentOperation& operation) noexcept {
    operation.id = request.operationId;
    operation.target = {
        request.expected.accountSoid, request.expected.characterSoid, request.expected.contentHash};
    switch (request.opcode) {
    case wire::Opcode::grantItem: {
        const auto grant = payload_of<wire::GrantItemRequest>(request);
        operation.request = bap::GrantItemOperation{grant.itemHash, grant.quantity};
        return true;
    }
    case wire::Opcode::setQuantity: {
        const auto edit = payload_of<wire::SetQuantityRequest>(request);
        state::GrantedRowArray scope{};
        if (!edit_scope(edit.scope, scope) || edit.reserved != decltype(edit.reserved){}) {
            return false;
        }
        operation.request = state::QuantityEdit{scope,
                                                request.expected.characterSoid,
                                                edit.instanceSoid,
                                                edit.definitionHash,
                                                edit.expectedSerial,
                                                edit.expectedQuantity,
                                                edit.quantity};
        return true;
    }
    case wire::Opcode::setSeasonalExperience: {
        const auto experience = payload_of<wire::SetSeasonalExperienceRequest>(request);
        operation.request =
            bap::SeasonalExperienceOperation{experience.expected, experience.replacement};
        return true;
    }
    case wire::Opcode::claimSeasonPassReward: {
        const auto claim = payload_of<wire::SeasonPassRowRequest>(request);
        if (claim.reserved != 0) {
            return false;
        }
        operation.request = bap::SeasonPassClaimOperation{claim.rowIndex, claim.itemHash};
        return true;
    }
    case wire::Opcode::resetSeasonPassClaim: {
        const auto reset = payload_of<wire::SeasonPassRowRequest>(request);
        if (reset.reserved != 0) {
            return false;
        }
        operation.request = bap::SeasonPassResetOperation{reset.rowIndex, reset.itemHash};
        return true;
    }
    case wire::Opcode::setPursuitObjective: {
        const auto edit = payload_of<wire::SetPursuitObjectiveRequest>(request);
        if (edit.ordinal >= wire::kPursuitObjectiveCapacity || edit.reserved != 0
            || edit.reserved2 != 0) {
            return false;
        }
        operation.request = bap::ObjectiveProgressOperation{edit.instanceSoid,
                                                            edit.itemHash,
                                                            edit.expectedSerial,
                                                            edit.objectiveHash,
                                                            edit.ordinal,
                                                            edit.expected,
                                                            edit.value};
        return true;
    }
    case wire::Opcode::completePursuitObjectives: {
        const auto completion = payload_of<wire::CompletePursuitObjectivesRequest>(request);
        operation.request = bap::ObjectiveCompletionOperation{
            completion.instanceSoid, completion.itemHash, completion.expectedSerial};
        return true;
    }
    default:
        return false;
    }
}

/** Answers with the retained receipt, or refuses a submission the game did not retain. */
[[nodiscard]] InvestmentAnswer submit(const bap::InvestmentOperation& operation,
                                      InvestmentPayload payload) noexcept {
    bap::InvestmentReceipt receipt{};
    switch (bap::submit_investment_operation(operation, g_editorPid, receipt)) {
    case bap::InvestmentSubmission::accepted:
        break;
    case bap::InvestmentSubmission::reused:
        return refused("operation_reused");
    case bap::InvestmentSubmission::refused:
        return refused(receipt.reason);
    }
    return encode_receipt(receipt, payload);
}

/** An unknown id leaves the receipt empty, which encodes as unknown. */
[[nodiscard]] InvestmentAnswer read_receipt(std::uint64_t operationId,
                                            InvestmentPayload payload) noexcept {
    bap::InvestmentReceipt receipt{};
    static_cast<void>(bap::investment_receipt(operationId, receipt));
    return encode_receipt(receipt, payload);
}

/** A pending receipt stays retained, so acknowledging it is refused. */
[[nodiscard]] InvestmentAnswer acknowledge_receipt(std::uint64_t operationId,
                                                   InvestmentPayload payload) noexcept {
    bap::InvestmentReceipt receipt{};
    if (!bap::acknowledge_investment_receipt(operationId, receipt) && receipt.operationId != 0) {
        return refused("receipt_pending");
    }
    return encode_receipt(receipt, payload);
}

/** Answers one copied request without reading the shared request block again. */
[[nodiscard]] InvestmentAnswer serve(const wire::Request& request) noexcept {
    if (request.opcode == wire::Opcode::none || request.opcode >= wire::Opcode::count) {
        return invalid("request_opcode");
    }
    if (const char* reason = malformed(request)) {
        return invalid(reason);
    }
    const InvestmentPayload payload(g_view->response.payload);
    switch (request.opcode) {
    case wire::Opcode::hello:
        return encode_hello(payload);
    case wire::Opcode::inventorySnapshot:
        return encode_inventory_snapshot(payload);
    case wire::Opcode::seasonPassSnapshot:
        return encode_season_pass_snapshot(payload);
    case wire::Opcode::pursuitSnapshot:
        return encode_pursuit_snapshot(payload);
    case wire::Opcode::readReceipt:
        return read_receipt(request.operationId, payload);
    case wire::Opcode::acknowledgeReceipt:
        return acknowledge_receipt(request.operationId, payload);
    case wire::Opcode::unlockValues:
        return encode_unlock_values(payload_of<wire::UnlockValuesRequest>(request), payload);
    case wire::Opcode::grantItem:
    case wire::Opcode::setQuantity:
    case wire::Opcode::setSeasonalExperience:
    case wire::Opcode::claimSeasonPassReward:
    case wire::Opcode::resetSeasonPassClaim:
    case wire::Opcode::setPursuitObjective:
    case wire::Opcode::completePursuitObjectives: {
        bap::InvestmentOperation operation{};
        if (!decode_operation(request, operation)) {
            return invalid("request_field");
        }
        return submit(operation, payload);
    }
    case wire::Opcode::none:
    case wire::Opcode::count:
        break;
    }
    return invalid("request_opcode");
}

/**
 * Open Studio views repeat hello, snapshot, unlock and receipt requests every second, so a read
 * logs at info only when it is refused for a new reason. Mutations and opcodes outside the wire's
 * range, which have no read slot, always log at info.
 */
[[nodiscard]] core::log::Level
answer_level(wire::Opcode opcode, wire::Status status, std::string_view reason) noexcept {
    if (opcode >= wire::Opcode::count || wire::is_mutation(opcode)) {
        return core::log::Level::info;
    }
    ReadAnswer& last = g_lastReads[static_cast<std::size_t>(opcode)];
    const bool changed = last.status != status || last.reason != reason;
    last = {status, reason};
    return changed && status != wire::Status::ok ? core::log::Level::info : core::log::Level::debug;
}

/** Writes the response block, then the response id, so Studio never reads half an answer. */
void answer(std::uint32_t request,
            const wire::Request& copy,
            const InvestmentAnswer& result) noexcept {
    wire::Response& response = g_view->response;
    response.status = result.status;
    response.reserved = 0;
    response.operationId = copy.operationId;
    response.opcode = copy.opcode;
    response.payloadBytes = result.payloadBytes;
    response.context = result.context;
    response.reason = {};
    const std::string_view reason(result.reason != nullptr ? result.reason : "");
    std::copy_n(
        reason.data(), (std::min)(reason.size(), wire::kReasonBytes - 1), response.reason.data());
    MemoryBarrier();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_view->control.response),
                        static_cast<LONG>(request));
    core::log::writef(core::log::Channel::server,
                      answer_level(copy.opcode, result.status, reason),
                      "ev=studio stage=investment result=answered request=%u opcode=%u status=%u "
                      "op=%llu reason=%s",
                      request,
                      static_cast<unsigned>(copy.opcode),
                      static_cast<unsigned>(result.status),
                      static_cast<unsigned long long>(copy.operationId),
                      reason.empty() ? "none" : reason.data());
}

} // namespace

/** Each request id gets exactly one answer, in the slice that copies it. */
void service_investment(std::uint64_t now) noexcept {
    // Studio attach is off by default, and while it is off no mapping is opened.
    if (!core::settings::get().studioAttach) {
        if (g_view != nullptr) {
            disconnect();
        }
        return;
    }
    if (g_view == nullptr && now >= g_nextConnect) {
        g_nextConnect = now + wire::kConnectIntervalMs;
        static_cast<void>(connect());
    }
    if (g_view == nullptr) {
        return;
    }
    // Read the clock again: `now` can trail a slow transport stage while the editor keeps beating.
    if (!middleware::studio::editor_alive(
            g_view->control.heartbeat, GetTickCount64(), wire::kHeartbeatTimeoutMs)) {
        disconnect();
        return;
    }
    const std::uint32_t request = read_id(g_view->control.request);
    if (request == 0 || request == read_id(g_view->control.response)) {
        return;
    }
    MemoryBarrier();
    const wire::Request copy = g_view->request;
    MemoryBarrier();
    // Studio replaced the request during the copy, so the next slice serves the new one.
    if (read_id(g_view->control.request) != request) {
        return;
    }
    answer(request, copy, serve(copy));
}

/** Runs after the server thread stops, so no slice still uses the view. */
void shutdown_investment() noexcept {
    disconnect();
    g_nextConnect = 0;
    g_editorPid = 0;
}

} // namespace sunrise::server::studio
