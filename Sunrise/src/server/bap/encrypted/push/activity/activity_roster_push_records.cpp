#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>

#include "../../../../../core/logging/log.h"
#include "internal.h"

namespace sunrise::server::bap::encrypted::push::activity {
namespace {

/** A body over this size is never suppressed, only delivered. */
constexpr std::size_t kBodyRecordCapacity = 16 * 1024;

/** One outbound body kept for the byte-identical repeat check. */
struct BodyRecord final {
    std::array<std::byte, kBodyRecordCapacity> bytes{};
    std::uint64_t bindingGeneration{};
    std::uint32_t size{};
    bool valid{};
};

/** Revision delivered to this exact connection, independently of member acknowledgement. */
struct MembershipCursor final {
    std::uint64_t sessionId{};
    std::uint32_t revision{};
};

/** What the last deferral line named, so one held item is reported once and not every pump. */
struct DeferralRecord final {
    std::uint64_t bindingGeneration{};
    std::uint64_t hostStateRevision{};
    std::uint64_t scriptableRevision{};
};

/** Per-connection last-body records for the byte-identical repeat check. */
struct ConnectionRecord final {
    BodyRecord rosterSent{};
    BodyRecord rosterStaged{};
    BodyRecord membershipSent{};
    BodyRecord membershipStaged{};
    MembershipCursor membershipSentCursor{};
    MembershipCursor membershipStagedCursor{};
    DeferralRecord deferred{};
};

// All access runs under the BAP lock, like the Session fields these records extend.
std::array<ConnectionRecord, kSessionCount> g_connectionRecords{};

/** @return This connection's record, or null for an out-of-range connection id. */
[[nodiscard]] ConnectionRecord* connection_record(const Session& session) noexcept {
    return session.id != 0 && session.id <= g_connectionRecords.size()
               ? &g_connectionRecords[session.id - 1]
               : nullptr;
}

/** @return True when the record holds this exact body for this exact binding. */
[[nodiscard]] bool matches_record(const BodyRecord& record,
                                  std::uint64_t bindingGeneration,
                                  std::span<const std::byte> body) noexcept {
    return record.valid && record.bindingGeneration == bindingGeneration
           && record.size == body.size()
           && std::equal(body.begin(), body.end(), record.bytes.begin());
}

/** Copies one staged body into a record; an oversized body clears it instead. */
void fill_record(BodyRecord& record,
                 std::uint64_t bindingGeneration,
                 std::span<const std::byte> body) noexcept {
    record.valid = false;
    if (body.size() > record.bytes.size()) {
        return;
    }
    std::copy(body.begin(), body.end(), record.bytes.begin());
    record.bindingGeneration = bindingGeneration;
    record.size = static_cast<std::uint32_t>(body.size());
    record.valid = true;
}

/** Promotes a staged record to the delivered one when its binding still matches. */
void promote_record(BodyRecord& staged,
                    BodyRecord& sent,
                    std::uint64_t bindingGeneration) noexcept {
    if (staged.valid && staged.bindingGeneration == bindingGeneration) {
        sent = staged;
    }
    staged.valid = false;
}

} // namespace

/** Names where this body first differs from the last one delivered on this connection. */
void report_roster_body_delta(const Session& session, std::span<const std::byte> body) noexcept {
    const ConnectionRecord* const record = connection_record(session);
    if (record == nullptr || !record->rosterSent.valid
        || record->rosterSent.bindingGeneration != session.activity.bindingGeneration) {
        return;
    }
    const std::size_t previous = record->rosterSent.size;
    const std::size_t shared = (std::min)(previous, body.size());
    std::size_t offset = 0;
    while (offset < shared && body[offset] == record->rosterSent.bytes[offset]) {
        ++offset;
    }
    if (offset == shared && previous == body.size()) {
        return;
    }
    const unsigned previousByte =
        offset < previous ? std::to_integer<unsigned>(record->rosterSent.bytes[offset]) : 0U;
    const unsigned currentByte =
        offset < body.size() ? std::to_integer<unsigned>(body[offset]) : 0U;
    std::array<char, core::log::kLineCapacity> line{};
    const int written = std::snprintf(line.data(),
                                      line.size(),
                                      "ev=activity stage=roster_delta first_byte=%zu bit=%zu "
                                      "bytes=%zu was_bytes=%zu old=0x%02X new=0x%02X",
                                      offset,
                                      offset * 8U,
                                      body.size(),
                                      previous,
                                      previousByte,
                                      currentByte);
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** Reports one unsolicited push held back while the client holds no slice set. */
void report_roster_deferral(const Session& session,
                            std::uint64_t hostStateRevision,
                            std::uint64_t scriptableRevision) noexcept {
    ConnectionRecord* const record = connection_record(session);
    const DeferralRecord current{
        session.activity.bindingGeneration, hostStateRevision, scriptableRevision};
    if (record != nullptr && record->deferred.bindingGeneration == current.bindingGeneration
        && record->deferred.hostStateRevision == current.hostStateRevision
        && record->deferred.scriptableRevision == current.scriptableRevision) {
        return;
    }
    if (record != nullptr) {
        record->deferred = current;
    }
    std::array<char, core::log::kLineCapacity> line{};
    const int written =
        std::snprintf(line.data(),
                      line.size(),
                      "ev=activity stage=roster result=deferred reason=client_loading "
                      "host_rev=%llu scriptable_rev=%llu",
                      static_cast<unsigned long long>(hostStateRevision),
                      static_cast<unsigned long long>(scriptableRevision));
    if (written > 0) {
        core::log::write(core::log::Channel::server,
                         core::log::Level::debug,
                         {line.data(), static_cast<std::size_t>(written)});
    }
}

/** @return True when this body equals the last roster body delivered on this connection. */
bool repeats_delivered_roster_body(const Session& session,
                                   std::span<const std::byte> body) noexcept {
    const ConnectionRecord* const record = connection_record(session);
    return record != nullptr
           && matches_record(record->rosterSent, session.activity.bindingGeneration, body);
}

/** Keeps one staged roster body until its frame outcome is known. */
void stage_roster_body_record(const Session& session, std::span<const std::byte> body) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record != nullptr) {
        fill_record(record->rosterStaged, session.activity.bindingGeneration, body);
    }
}

/** Promotes the staged roster body to the delivered record. */
void commit_roster_body_record(const Session& session) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record != nullptr) {
        promote_record(
            record->rosterStaged, record->rosterSent, session.activity.bindingGeneration);
    }
}

/** Drops the staged roster body of a discarded frame. */
void discard_roster_body_record(const Session& session) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record != nullptr) {
        record->rosterStaged.valid = false;
    }
}

/** @return True when this body equals the last membership body delivered on this connection. */
bool repeats_delivered_membership_body(const Session& session,
                                       std::span<const std::byte> body) noexcept {
    const ConnectionRecord* const record = connection_record(session);
    return record != nullptr
           && matches_record(record->membershipSent, session.activity.bindingGeneration, body);
}

/** Keeps one staged membership body until its frame outcome is known. */
void stage_membership_body_record(const Session& session,
                                  std::span<const std::byte> body,
                                  std::uint64_t sessionId,
                                  std::uint32_t revision) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record != nullptr) {
        fill_record(record->membershipStaged, session.activity.bindingGeneration, body);
        record->membershipStagedCursor = {sessionId, revision};
    }
}

/** A discarded frame cannot supply a later membership delivery record. */
void discard_membership_body_record(const Session& session) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record != nullptr) {
        record->membershipStaged = {};
    }
}

/** Promotes the staged membership body to the delivered record. */
void commit_membership_body_record(const Session& session) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record != nullptr) {
        // The cursor moves on the same rule as the body record: a body the client never received
        // was never delivered down this link, so it stays owed.
        if (record->membershipStaged.valid
            && record->membershipStaged.bindingGeneration == session.activity.bindingGeneration) {
            record->membershipSentCursor = record->membershipStagedCursor;
        }
        promote_record(
            record->membershipStaged, record->membershipSent, session.activity.bindingGeneration);
    }
}

bool connection_owes_membership(const Session& session,
                                std::uint64_t sessionId,
                                std::uint32_t revision) noexcept {
    if (sessionId == state::activity::kAbsentSessionId
        || revision == state::activity::membership::kAbsentRevision) {
        return false;
    }
    const ConnectionRecord* const record = connection_record(session);
    return record == nullptr || !record->membershipSent.valid
           || record->membershipSent.bindingGeneration != session.activity.bindingGeneration
           || record->membershipSentCursor.sessionId != sessionId
           || record->membershipSentCursor.revision != revision;
}

void adopt_join_membership_record(const Session& session) noexcept {
    ConnectionRecord* const record = connection_record(session);
    if (record == nullptr || !record->membershipStaged.valid) {
        return;
    }
    // The body was staged before the join commit reserved this generation.
    record->membershipStaged.bindingGeneration = session.activity.bindingGeneration;
    record->membershipSentCursor = record->membershipStagedCursor;
    promote_record(
        record->membershipStaged, record->membershipSent, session.activity.bindingGeneration);
}

} // namespace sunrise::server::bap::encrypted::push::activity
