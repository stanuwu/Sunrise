#include <bit>

#include "store_internal.h"

namespace sunrise::state::investment::store {
namespace {

/** One plug hash per lane, little-endian, 0 for an empty lane; no installed plug hashes to 0. */
constexpr std::size_t kPlugBytes = sizeof(std::uint32_t);
/** Shift step from one plug byte to the next. */
constexpr unsigned kBitsPerByte = 8U;
using PlugBlob = std::array<std::uint8_t, account::inventory::kPlugCapacity * kPlugBytes>;

/** Packs every lane; a lane past the plug count is empty, so it packs as 0. */
[[nodiscard]] PlugBlob encode_plugs(const account::inventory::Sockets& sockets) noexcept {
    PlugBlob blob{};
    for (std::size_t lane = 0; lane < sockets.plugs.size(); ++lane) {
        const std::uint32_t hash = sockets.plugs[lane].value_or(0U);
        for (std::size_t byte = 0; byte < kPlugBytes; ++byte) {
            blob[lane * kPlugBytes + byte] =
                static_cast<std::uint8_t>(hash >> (byte * kBitsPerByte));
        }
    }
    return blob;
}

/** Unpacks every lane; 0 reads back as an empty lane. */
void decode_plugs(const PlugBlob& blob, account::inventory::Sockets& sockets) noexcept {
    for (std::size_t lane = 0; lane < sockets.plugs.size(); ++lane) {
        std::uint32_t hash = 0;
        for (std::size_t byte = 0; byte < kPlugBytes; ++byte) {
            hash |= static_cast<std::uint32_t>(blob[lane * kPlugBytes + byte])
                    << (byte * kBitsPerByte);
        }
        sockets.plugs[lane] = hash == 0 ? std::nullopt : std::optional<std::uint32_t>(hash);
    }
}

} // namespace

/** Keeps every field the account list would have written, plugs and objectives included. */
bool park(const ParkedRow& row) noexcept {
    const std::lock_guard lock(g_mutex);
    const account::inventory::Item& item = row.item;
    std::optional<ObjectiveBlob> objectives;
    if (item.objectives) {
        objectives = std::bit_cast<ObjectiveBlob>(*item.objectives);
    }
    Statement insert(
        "INSERT INTO parked_items(kind,character_slot,location,position,instance_soid,"
        "definition_hash,level,quantity,mutation_serial,flags,socket_policy,plug_count,"
        "plugs,movement_ability,grenade_ability,super_ability,melee_ability,"
        "class_ability,seen,placement,objectives) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    return insert.write(row.kind,
                        row.characterSlot,
                        row.location,
                        row.position,
                        item.instanceSoid,
                        item.definitionHash,
                        item.level,
                        item.quantity,
                        item.mutationSerial,
                        item.flags,
                        item.sockets.policy,
                        item.sockets.plugCount,
                        encode_plugs(item.sockets),
                        item.movementAbilityEntry,
                        item.grenadeAbilityEntry,
                        item.superAbilityEntry,
                        item.meleeAbilityEntry,
                        item.classAbilityEntry,
                        item.seen,
                        item.placement,
                        objectives);
}

/** Reads every parked row, oldest first, so returns keep the order the rows left in. */
bool read_parked(std::vector<ParkedRow>& output) noexcept {
    const std::lock_guard lock(g_mutex);
    output.clear();
    Statement rows(
        "SELECT id,kind,character_slot,location,position,instance_soid,definition_hash,"
        "level,quantity,mutation_serial,flags,socket_policy,plug_count,plugs,"
        "movement_ability,grenade_ability,super_ability,melee_ability,class_ability,seen,"
        "placement,objectives FROM parked_items ORDER BY id");
    int result = rows.step();
    while (result == SQLITE_ROW) {
        ParkedRow row;
        PlugBlob plugs{};
        std::optional<ObjectiveBlob> objectives;
        account::inventory::Item& item = row.item;
        if (!rows.columns(row.id,
                          row.kind,
                          row.characterSlot,
                          row.location,
                          row.position,
                          item.instanceSoid,
                          item.definitionHash,
                          item.level,
                          item.quantity,
                          item.mutationSerial,
                          item.flags,
                          item.sockets.policy,
                          item.sockets.plugCount,
                          plugs,
                          item.movementAbilityEntry,
                          item.grenadeAbilityEntry,
                          item.superAbilityEntry,
                          item.meleeAbilityEntry,
                          item.classAbilityEntry,
                          item.seen,
                          item.placement,
                          objectives)) {
            output.clear();
            return false;
        }
        decode_plugs(plugs, item.sockets);
        if (objectives) {
            item.objectives = std::bit_cast<account::inventory::ItemObjectives>(*objectives);
        }
        output.push_back(row);
        result = rows.step();
    }
    if (result != SQLITE_DONE) {
        output.clear();
        return false;
    }
    return true;
}

bool unpark(std::int64_t id) noexcept {
    const std::lock_guard lock(g_mutex);
    Statement row("DELETE FROM parked_items WHERE id=?");
    return row.write(id) && sqlite3_changes(g_database) == 1;
}

} // namespace sunrise::state::investment::store
