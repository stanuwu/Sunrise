#include "public_event_schedule_table.h"

#include "public_event_schedule_records.h"

namespace sunrise::middleware::content::packages::tables::public_event_schedule {
namespace {
namespace store = state::build_data::public_event_schedule;
using records::block_rows;
using records::EntryRecord;
using records::read;
using records::RowRecord;
using records::seconds;
} // namespace

bool decode(std::span<const std::byte> payload, std::uint32_t tag, store::Table& table) noexcept {
    std::uint32_t declared{};
    if (!read(payload, 0, declared) || declared != payload.size()) {
        return false;
    }
    std::size_t rowCount{}, rowsAt{}, groupCount{}, groupsAt{};
    if (!block_rows(payload, records::kRowsBlockOffset, sizeof(RowRecord), rowCount, rowsAt)
        || !block_rows(
            payload, records::kGroupsBlockOffset, sizeof(std::uint32_t), groupCount, groupsAt)
        || rowCount == 0 || rowCount > store::kRowCapacity || groupCount > store::kGroupCapacity) {
        return false;
    }
    table = store::Table{};
    table.tag = tag;
    table.payloadBytes = declared;
    for (std::size_t index = 0; index < groupCount; ++index) {
        if (!read(payload, groupsAt + index * sizeof(std::uint32_t), table.groups[index])) {
            return false;
        }
    }
    table.groupCount = groupCount;
    for (std::size_t index = 0; index < rowCount; ++index) {
        const std::size_t rowAt = rowsAt + index * sizeof(RowRecord);
        RowRecord record{};
        std::size_t entryCount{}, entriesAt{};
        if (!read(payload, rowAt, record)
            || !block_rows(payload,
                           rowAt + offsetof(RowRecord, entries),
                           sizeof(EntryRecord),
                           entryCount,
                           entriesAt)
            || table.entryCount + entryCount > store::kEntryCapacity) {
            return false;
        }
        store::Row& row = table.rows[index];
        row.key = record.key;
        row.salt = record.salt;
        row.periodSeconds = seconds(record.periodMinutes);
        row.spanSeconds = seconds(record.spanMinutes);
        row.offsetSeconds = seconds(record.offsetMinutes);
        row.windowSeconds = seconds(record.windowMinutes);
        if (row.periodSeconds <= 0 || row.periodSeconds > store::kRotationSeconds
            || row.spanSeconds < 0 || row.spanSeconds > store::kRotationSeconds
            || row.offsetSeconds < 0 || row.offsetSeconds > store::kRotationSeconds
            || row.windowSeconds < 0 || row.windowSeconds > store::kRotationSeconds) {
            return false;
        }
        row.firstEntry = static_cast<std::uint16_t>(table.entryCount);
        row.entryCount = static_cast<std::uint16_t>(entryCount);
        for (std::size_t e = 0; e < entryCount; ++e) {
            EntryRecord authored{};
            if (!read(payload, entriesAt + e * sizeof(EntryRecord), authored)) {
                return false;
            }
            if (!std::isfinite(authored.weight) || authored.weight < 0.0f) {
                return false;
            }
            store::Entry& entry = table.entries[table.entryCount++];
            entry.weight = authored.weight;
            entry.gate = authored.gate;
            entry.group = authored.group;
            entry.site = authored.site;
            entry.rally = authored.rally;
            entry.node = authored.node;
        }
    }
    table.rowCount = rowCount;
    return true;
}
} // namespace sunrise::middleware::content::packages::tables::public_event_schedule
