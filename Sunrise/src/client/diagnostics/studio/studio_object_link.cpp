#include "studio_object_link.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <span>

#include "../../../middleware/studio/studio_mapping.h"
#include "studio_object_protocol.h"
#include "studio_pose_link.h"
#include "studio_protocol.h"
#include "studio_world_link.h"
#include "studio_world_reader.h"

namespace sunrise::client::diagnostics::studio {
namespace {

namespace wire = object_wire;
static_assert(kObjectDatumBytes <= wire::kDatumCapacity);
/** Native parent handles use all bits set for no parent. */
constexpr std::uint32_t kNone = 0xFFFFFFFFU;

HANDLE g_mapping{};
wire::SharedObject* g_view{};
std::uint64_t g_nextConnect{};

/** Only the core diagnostic lock may call the object mapping helpers. */
void disconnect_object() noexcept {
    middleware::studio::close_editor_view(g_mapping, g_view);
}

/** @return True only when an editor created the matching object protocol. */
[[nodiscard]] bool connect_object() noexcept {
    g_view =
        middleware::studio::open_editor_view<wire::SharedObject>(wire::kMappingFormat, g_mapping);
    if (g_view == nullptr) {
        return false;
    }
    // Studio reads clientVersion to explain a version mismatch.
    if (g_view->magic == wire::kMagic) {
        g_view->clientVersion = wire::kVersion;
    }
    if (!middleware::studio::editor_header_ok(
            *g_view, wire::kMagic, wire::kVersion, wire::kMappingBytes)) {
        disconnect_object();
        return false;
    }
    return true;
}

/**
 * Builds one full record for an object of the last published scan.
 * @param answer Receives the record fields.
 * @param status Receives the answer status.
 */
void describe(std::uint32_t handle,
              std::uint32_t generation,
              wire::SharedObject& answer,
              wire::Status& status) noexcept {
    PublishedObject object{};
    std::array<std::array<std::uint32_t, 2>, wire::kChildCapacity> children{};
    std::size_t childCount = 0;
    const Lookup lookup = find_published_object(handle, generation, object, children, childCount);
    if (lookup != Lookup::found) {
        status = lookup == Lookup::absent ? wire::Status::absent : wire::Status::unavailable;
        return;
    }
    PlacementSnapshot sample{};
    std::array<std::byte, kObjectDatumBytes> raw{};
    const bool posed = object.dynamic
                           ? read_dynamic({object.identity, object.classTag}, sample, &raw)
                           : read_placement(object.identity, sample, &raw);
    if (sample.gone) {
        status = wire::Status::absent;
        return;
    }
    answer.identity = object.identity.placementIdentity;
    answer.objectListTag = object.identity.objectListTag;
    answer.entryIndex = object.identity.entryIndex;
    answer.classTag = object.classTag;
    answer.flags = object.dynamic ? wire::dynamic : 0U;
    answer.parentHandle = kNone;
    if (sample.flagsValid) {
        answer.datumFlags = sample.datumFlags;
        answer.parentHandle = sample.parentHandle;
        answer.flags |= wire::datumValid;
        std::memcpy(answer.datum.data(), raw.data(), raw.size());
        answer.datumBytes = static_cast<std::uint32_t>(raw.size());
    }
    if (sample.stateKnown) {
        answer.rootHandle = sample.rootHandle;
        answer.rootFlags = sample.rootFlags;
        answer.chainFlags = sample.chainFlags;
        answer.flags |= wire::stateKnown | (sample.placedNotHidden ? wire::placedNotHidden : 0U);
    }
    if (posed) {
        answer.position = sample.position;
        answer.orientation = sample.orientation;
        answer.scale = sample.scale;
        answer.flags |= wire::transformValid;
    }
    const std::size_t written = std::min(childCount, wire::kChildCapacity);
    for (std::size_t i = 0; i < written; ++i) {
        answer.children[i] = {children[i][0], children[i][1]};
    }
    answer.childCount = static_cast<std::uint32_t>(written);
    answer.flags |= childCount > written ? wire::childrenTruncated : 0U;
    const std::size_t models = published_models(handle, generation, answer.models, answer.poseTime);
    answer.modelCount = static_cast<std::uint32_t>(std::min(models, wire::kModelCapacity));
    answer.flags |= answer.poseTime != 0 ? wire::poseCaptured : 0U;
    status = wire::Status::complete;
}

/** Copies the record and its status under one sequence. */
void publish(std::uint32_t request,
             wire::Status status,
             const wire::SharedObject& answer) noexcept {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_view->sequence));
    std::memcpy(reinterpret_cast<std::byte*>(g_view) + wire::kRecordOffset,
                reinterpret_cast<const std::byte*>(&answer) + wire::kRecordOffset,
                wire::kMappingBytes - wire::kRecordOffset);
    g_view->response = request;
    g_view->status = status;
    g_view->gamePid = GetCurrentProcessId();
    g_view->time = GetTickCount64();
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_view->sequence));
}

} // namespace

/** An answered request id is never answered again. */
void poll_object_link() noexcept {
    const std::uint64_t now = GetTickCount64();
    if (g_view == nullptr && now >= g_nextConnect) {
        g_nextConnect = now + kConnectInterval;
        (void)connect_object();
    }
    if (g_view == nullptr) {
        return;
    }
    if (!middleware::studio::editor_alive(g_view->heartbeat, now, kHeartbeatTimeout)) {
        disconnect_object();
        return;
    }
    auto* const requestWord = reinterpret_cast<volatile LONG*>(&g_view->request);
    const auto request = static_cast<std::uint32_t>(InterlockedCompareExchange(requestWord, 0, 0));
    if (request == 0 || request == g_view->response) {
        return;
    }
    const std::uint32_t handle = g_view->handle;
    const std::uint32_t generation = g_view->generation;
    // The editor writes the object before the id; a newer id means the object may be torn.
    if (static_cast<std::uint32_t>(InterlockedCompareExchange(requestWord, 0, 0)) != request) {
        return;
    }
    wire::SharedObject answer{};
    wire::Status status = wire::Status::pending;
    describe(handle, generation, answer, status);
    publish(request, status, answer);
}

void drop_object_link() noexcept {
    disconnect_object();
}

void close_object_link() noexcept {
    disconnect_object();
    g_nextConnect = 0;
}

} // namespace sunrise::client::diagnostics::studio
