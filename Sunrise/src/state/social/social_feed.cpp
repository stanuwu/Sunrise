#include "social_feed.h"

#include <cstring>

#include "../../middleware/encoding/byte_order.h"

namespace sunrise::state::social::feed {
namespace {

namespace encoding = sunrise::middleware::encoding;

template <std::size_t Capacity>
[[nodiscard]] std::size_t text_length(const std::array<char, Capacity>& text) noexcept {
    std::size_t length = 0;
    while (length + 1 < Capacity && text[length] != '\0') {
        ++length;
    }
    return length;
}

class Writer {
public:
    explicit Writer(std::span<std::byte> output) noexcept : output_(output) {}

    [[nodiscard]] bool ok() const noexcept {
        return ok_;
    }

    [[nodiscard]] std::size_t size() const noexcept {
        return offset_;
    }

    void put_u8(std::uint8_t value) noexcept {
        if (!take(1)) {
            return;
        }
        output_[offset_ - 1] = static_cast<std::byte>(value);
    }

    void put_u16(std::uint16_t value) noexcept {
        if (!take(encoding::kU16Size)) {
            return;
        }
        encoding::write_u16_be(
            output_.subspan(offset_ - encoding::kU16Size).first<encoding::kU16Size>(), value);
    }

    void put_u64(std::uint64_t value) noexcept {
        if (!take(encoding::kU64Size)) {
            return;
        }
        encoding::write_u64_be(
            output_.subspan(offset_ - encoding::kU64Size).first<encoding::kU64Size>(), value);
    }

    void put_bytes(const void* source, std::size_t count) noexcept {
        if (count == 0 || !take(count)) {
            return;
        }
        std::memcpy(output_.data() + (offset_ - count), source, count);
    }

private:
    [[nodiscard]] bool take(std::size_t count) noexcept {
        if (!ok_ || count > output_.size() - offset_) {
            ok_ = false;
            return false;
        }
        offset_ += count;
        return true;
    }

    std::span<std::byte> output_;
    std::size_t offset_{};
    bool ok_{true};
};

class Reader {
public:
    explicit Reader(std::span<const std::byte> input) noexcept : input_(input) {}

    [[nodiscard]] bool ok() const noexcept {
        return ok_;
    }
    [[nodiscard]] bool complete() const noexcept {
        return ok_ && offset_ == input_.size();
    }

    /** Marks the body malformed. A refused record must never be read as a short one. */
    void fail() noexcept {
        ok_ = false;
    }

    [[nodiscard]] std::uint8_t get_u8() noexcept {
        if (!take(1)) {
            return 0;
        }
        return std::to_integer<std::uint8_t>(input_[offset_ - 1]);
    }

    [[nodiscard]] std::uint16_t get_u16() noexcept {
        if (!take(encoding::kU16Size)) {
            return 0;
        }
        return encoding::read_u16_be(
            input_.subspan(offset_ - encoding::kU16Size).first<encoding::kU16Size>());
    }

    [[nodiscard]] std::uint64_t get_u64() noexcept {
        if (!take(encoding::kU64Size)) {
            return 0;
        }
        return encoding::read_u64_be(
            input_.subspan(offset_ - encoding::kU64Size).first<encoding::kU64Size>());
    }

    template <std::size_t Capacity>
    void get_text(std::array<char, Capacity>& destination, std::size_t count) noexcept {
        destination = {};
        if (count + 1 > Capacity || !take(count)) {
            ok_ = false;
            return;
        }
        if (count != 0) {
            std::memcpy(destination.data(), input_.data() + (offset_ - count), count);
        }
    }

    void get_raw(std::span<std::byte> destination) noexcept {
        if (!take(destination.size())) {
            return;
        }
        if (!destination.empty()) {
            std::memcpy(destination.data(),
                        input_.data() + (offset_ - destination.size()),
                        destination.size());
        }
    }

private:
    [[nodiscard]] bool take(std::size_t count) noexcept {
        if (!ok_ || count > input_.size() - offset_) {
            ok_ = false;
            return false;
        }
        offset_ += count;
        return true;
    }

    std::span<const std::byte> input_;
    std::size_t offset_{};
    bool ok_{true};
};

void put_invites(Writer& writer,
                 const std::array<WireInvite, kInviteCapacity>& invites,
                 std::size_t count) noexcept {
    writer.put_u8(static_cast<std::uint8_t>(count));
    for (std::size_t index = 0; index < count; ++index) {
        const WireInvite& invite = invites[index];
        const std::size_t length = text_length(invite.connect);
        writer.put_u64(invite.sequence);
        writer.put_u64(invite.targetSoid);
        writer.put_u64(invite.inviterSoid);
        writer.put_u16(static_cast<std::uint16_t>(length));
        writer.put_bytes(invite.connect.data(), length);
    }
}

void get_invites(Reader& reader,
                 std::array<WireInvite, kInviteCapacity>& invites,
                 std::size_t& count) noexcept {
    const std::size_t declared = reader.get_u8();
    if (declared > kInviteCapacity) {
        // A count past capacity is a malformed body, not a truncation point: reading part of it
        // would leave the reader mid-record and mis-frame everything after it.
        reader.fail();
        count = 0;
        return;
    }
    for (std::size_t index = 0; index < declared; ++index) {
        WireInvite& invite = invites[index];
        invite = {};
        invite.sequence = reader.get_u64();
        invite.targetSoid = reader.get_u64();
        invite.inviterSoid = reader.get_u64();
        reader.get_text(invite.connect, reader.get_u16());
    }
    count = reader.ok() ? declared : 0;
}

void put_messages(Writer& writer,
                  const std::array<lobby::Message, lobby::kBatchCapacity>& messages,
                  std::size_t count) noexcept {
    writer.put_u8(static_cast<std::uint8_t>(count));
    for (std::size_t i = 0; i < count; ++i) {
        const auto& message = messages[i];
        writer.put_u64(message.sequence);
        writer.put_u64(message.lobby);
        writer.put_u64(message.sender);
        writer.put_u16(message.size);
        writer.put_bytes(message.body.data(), message.size);
    }
}
bool valid_messages(const std::array<lobby::Message, lobby::kBatchCapacity>& messages,
                    std::size_t count) noexcept {
    if (count > messages.size()) {
        return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        if (messages[i].size > lobby::kPayloadCapacity) {
            return false;
        }
    }
    return true;
}
void get_messages(Reader& reader,
                  std::array<lobby::Message, lobby::kBatchCapacity>& messages,
                  std::size_t& count) noexcept {
    count = reader.get_u8();
    if (count > messages.size()) {
        reader.fail();
        count = 0;
        return;
    }
    for (std::size_t i = 0; i < count; ++i) {
        auto& message = messages[i];
        message.sequence = reader.get_u64();
        message.lobby = reader.get_u64();
        message.sender = reader.get_u64();
        message.size = reader.get_u16();
        if (message.size > message.body.size()) {
            reader.fail();
            return;
        }
        reader.get_raw(std::span(message.body).first(message.size));
    }
}
} // namespace

bool encode_sync(const Sync& sync, std::span<std::byte> output, std::size_t& written) noexcept {
    written = 0;
    if (sync.inviteCount > kInviteCapacity || sync.lobby.membershipCount > lobby::kLobbyCapacity
        || !valid_messages(sync.lobby.messages, sync.lobby.messageCount)) {
        return false;
    }
    Writer writer{output};
    writer.put_u8(kVersion);
    writer.put_u64(sync.epoch);
    writer.put_u64(sync.acceptedThrough);
    writer.put_u64(sync.receivedThrough);
    put_invites(writer, sync.invites, sync.inviteCount);
    writer.put_u64(sync.lobby.epoch);
    writer.put_u64(sync.lobby.membershipRevision);
    writer.put_u64(sync.lobby.acceptedThrough);
    writer.put_u64(sync.lobby.receivedThrough);
    writer.put_u8(static_cast<std::uint8_t>(sync.lobby.membershipCount));
    for (std::size_t i = 0; i < sync.lobby.membershipCount; ++i) {
        writer.put_u64(sync.lobby.memberships[i]);
    }
    put_messages(writer, sync.lobby.messages, sync.lobby.messageCount);
    if (!writer.ok()) {
        return false;
    }
    written = writer.size();
    return true;
}

bool decode_sync(std::span<const std::byte> body, Sync& sync) noexcept {
    sync = {};
    Reader reader{body};
    if (reader.get_u8() != kVersion) {
        return false;
    }
    sync.epoch = reader.get_u64();
    sync.acceptedThrough = reader.get_u64();
    sync.receivedThrough = reader.get_u64();
    get_invites(reader, sync.invites, sync.inviteCount);
    sync.lobby.epoch = reader.get_u64();
    sync.lobby.membershipRevision = reader.get_u64();
    sync.lobby.acceptedThrough = reader.get_u64();
    sync.lobby.receivedThrough = reader.get_u64();
    sync.lobby.membershipCount = reader.get_u8();
    if (sync.lobby.membershipCount > lobby::kLobbyCapacity) {
        sync = {};
        return false;
    }
    for (std::size_t i = 0; i < sync.lobby.membershipCount; ++i) {
        sync.lobby.memberships[i] = reader.get_u64();
    }
    get_messages(reader, sync.lobby.messages, sync.lobby.messageCount);
    if (!reader.complete()) {
        sync = {};
        return false;
    }
    return true;
}

bool encode_feed(const Feed& value, std::span<std::byte> output, std::size_t& written) noexcept {
    written = 0;
    if (value.rowCount > kRowCapacity || value.inviteCount > kInviteCapacity
        || value.routeCount > value.routes.size()
        || !valid_messages(value.lobby.messages, value.lobby.messageCount)) {
        return false;
    }
    Writer writer{output};
    writer.put_u8(kVersion);
    writer.put_u64(value.epoch);
    writer.put_u64(value.acceptedThrough);
    writer.put_u64(value.revision);
    writer.put_u8(static_cast<std::uint8_t>(value.rowCount));
    for (std::size_t index = 0; index < value.rowCount; ++index) {
        const WireRow& row = value.rows[index];
        const std::size_t nameLength = text_length(row.personaName);
        writer.put_u64(row.primarySoid);
        writer.put_u64(row.steamId);
        writer.put_u8(static_cast<std::uint8_t>(nameLength));
        writer.put_bytes(row.personaName.data(), nameLength);
    }
    put_invites(writer, value.invites, value.inviteCount);
    writer.put_u64(value.lobby.epoch);
    writer.put_u64(value.lobby.membershipRevision);
    writer.put_u64(value.lobby.acceptedThrough);
    writer.put_u64(value.lobby.receivedThrough);
    put_messages(writer, value.lobby.messages, value.lobby.messageCount);
    writer.put_u8(static_cast<std::uint8_t>(value.routeCount));
    for (std::size_t index = 0; index < value.routeCount; ++index) {
        const auto endpoint = value.routes[index];
        if (!middleware::gameplay::descriptor::unicast_endpoint(endpoint)) {
            return false;
        }
        writer.put_u16(static_cast<std::uint16_t>(endpoint.address >> 16U));
        writer.put_u16(static_cast<std::uint16_t>(endpoint.address));
        writer.put_u16(endpoint.port);
    }
    if (!writer.ok()) {
        return false;
    }
    written = writer.size();
    return true;
}

bool decode_feed(std::span<const std::byte> body, Feed& value) noexcept {
    value = {};
    Reader reader{body};
    if (reader.get_u8() != kVersion) {
        return false;
    }
    value.epoch = reader.get_u64();
    value.acceptedThrough = reader.get_u64();
    value.revision = reader.get_u64();
    const std::size_t rows = reader.get_u8();
    if (rows > kRowCapacity) {
        value = {};
        return false;
    }
    for (std::size_t index = 0; index < rows; ++index) {
        WireRow& row = value.rows[index];
        row = {};
        row.primarySoid = reader.get_u64();
        row.steamId = reader.get_u64();
        reader.get_text(row.personaName, reader.get_u8());
    }
    value.rowCount = rows;
    get_invites(reader, value.invites, value.inviteCount);
    value.lobby.epoch = reader.get_u64();
    value.lobby.membershipRevision = reader.get_u64();
    value.lobby.acceptedThrough = reader.get_u64();
    value.lobby.receivedThrough = reader.get_u64();
    get_messages(reader, value.lobby.messages, value.lobby.messageCount);
    value.routeCount = reader.get_u8();
    if (value.routeCount > value.routes.size()) {
        value = {};
        return false;
    }
    for (std::size_t index = 0; index < value.routeCount; ++index) {
        auto& endpoint = value.routes[index];
        const auto high = reader.get_u16();
        const auto low = reader.get_u16();
        endpoint.address = (static_cast<std::uint32_t>(high) << 16U) | low;
        endpoint.port = reader.get_u16();
        if (!middleware::gameplay::descriptor::unicast_endpoint(endpoint)) {
            reader.fail();
        }
    }
    if (!reader.complete()) {
        value = {};
        return false;
    }
    return true;
}

} // namespace sunrise::state::social::feed
