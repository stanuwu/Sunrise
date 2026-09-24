#include "reputation_sale_catalog.h"

#include <Windows.h>

#include <array>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <string>

#include "../../../../resources/resource.h"
#include "../../../core/threading/srw_lock.h"
#include "sqlite3.h"

namespace sunrise::state::build_data::vendors {
namespace {

core::threading::SrwLock g_lock;
std::array<ReputationSale, kReputationSaleCapacity> g_sales{};
std::size_t g_saleCount{};

/**
 * Copy the DLL's SQL resource into a terminated buffer for SQLite.
 * @param module DLL holding the content resource.
 * @param sql Receives SQL; cleared on failure.
 * @return False when the resource is missing or empty.
 */
bool resource_sql(void* module, std::string& sql) noexcept {
    sql.clear();
    const auto loaded = static_cast<HMODULE>(module);
    if (loaded == nullptr) {
        return false;
    }
    const HRSRC found =
        FindResourceW(loaded, MAKEINTRESOURCEW(IDR_VENDOR_REPUTATION_CONTENT), RT_RCDATA);
    if (found == nullptr) {
        return false;
    }
    const DWORD size = SizeofResource(loaded, found);
    const HGLOBAL data = LoadResource(loaded, found);
    const auto* bytes = data != nullptr ? static_cast<const char*>(LockResource(data)) : nullptr;
    if (size == 0 || bytes == nullptr) {
        return false;
    }
    sql.assign(bytes, size);
    return true;
}

/**
 * Reject SQLite integers outside the destination type's range.
 * @tparam Value Integral destination type.
 * @param statement Active row statement.
 * @param index Selected column.
 * @param output Receives the value on success.
 * @param minimum Smallest allowed value.
 * @return False for a null, non-integer or out-of-range value.
 */
template <typename Value>
bool column(sqlite3_stmt* statement, int index, Value& output, std::int64_t minimum) noexcept {
    if (sqlite3_column_type(statement, index) != SQLITE_INTEGER) {
        return false;
    }
    const auto value = sqlite3_column_int64(statement, index);
    if (value < minimum || value > (std::numeric_limits<Value>::max)()) {
        return false;
    }
    output = static_cast<Value>(value);
    return true;
}

/**
 * Accept only a nonempty, sorted, bounded set of complete sale rows.
 * @param database Temporary content database.
 * @param sales Receives rows; never published on failure.
 * @param count Receives the number of rows read.
 * @return False when any row or the query is invalid.
 */
bool read_sales(sqlite3* database,
                std::array<ReputationSale, kReputationSaleCapacity>& sales,
                std::size_t& count) noexcept {
    count = 0;
    constexpr char kQuery[] =
        "SELECT vendor_hash, sale_index, faction_hash, placeholder_hash, cost_hash, "
        "category_index, cost_quantity, xp_per_unit FROM reputation_sales "
        "ORDER BY vendor_hash, sale_index";
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, kQuery, -1, &statement, nullptr) != SQLITE_OK) {
        return false;
    }
    int status = SQLITE_OK;
    bool valid = true;
    while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
        if (count == sales.size()) {
            valid = false;
            break;
        }
        ReputationSale sale{};
        valid = column(statement, 0, sale.vendorHash, 1) && column(statement, 1, sale.saleIndex, 0)
                && column(statement, 2, sale.factionHash, 1)
                && column(statement, 3, sale.placeholderHash, 1)
                && column(statement, 4, sale.costHash, 1)
                && column(statement, 5, sale.categoryIndex, 0)
                && column(statement, 6, sale.costQuantity, 1)
                && column(statement, 7, sale.experiencePerUnit, 1);
        if (!valid
            || (count != 0
                && (sales[count - 1].vendorHash > sale.vendorHash
                    || (sales[count - 1].vendorHash == sale.vendorHash
                        && sales[count - 1].saleIndex >= sale.saleIndex)))) {
            valid = false;
            break;
        }
        sales[count++] = sale;
    }
    const bool complete = valid && status == SQLITE_DONE && count != 0;
    const int finalized = sqlite3_finalize(statement);
    return complete && finalized == SQLITE_OK;
}

} // namespace

/**
 * Publish only complete content sales, independent of player-save SQLite.
 * @param module DLL holding the content resource.
 * @return False when SQL or any sale row is invalid.
 */
bool load_reputation_sales(void* module) noexcept {
    clear_reputation_sales();
    std::string sql;
    if (!resource_sql(module, sql)) {
        return false;
    }
    sqlite3* database = nullptr;
    if (sqlite3_open(":memory:", &database) != SQLITE_OK) {
        sqlite3_close(database);
        return false;
    }
    std::array<ReputationSale, kReputationSaleCapacity> sales{};
    std::size_t count = 0;
    const bool complete =
        sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr) == SQLITE_OK
        && read_sales(database, sales, count);
    const int closed = sqlite3_close(database);
    if (!complete || closed != SQLITE_OK) {
        return false;
    }
    const std::lock_guard guard(g_lock);
    g_sales = sales;
    g_saleCount = count;
    return true;
}

/** Clears the small content catalog under its reader lock. */
void clear_reputation_sales() noexcept {
    const std::lock_guard guard(g_lock);
    g_sales = {};
    g_saleCount = 0;
}

/**
 * Copy one exact vendor sale under the catalog lock.
 * @param vendorHash Installed vendor hash.
 * @param saleIndex Sale selector within that vendor.
 * @param sale Receives the row, or an empty row when absent.
 * @return True when the row exists.
 */
bool find_reputation_sale(std::uint32_t vendorHash,
                          std::uint16_t saleIndex,
                          ReputationSale& sale) noexcept {
    sale = {};
    const std::shared_lock guard(g_lock);
    for (std::size_t index = 0; index < g_saleCount; ++index) {
        if (g_sales[index].vendorHash == vendorHash && g_sales[index].saleIndex == saleIndex) {
            sale = g_sales[index];
            return true;
        }
    }
    return false;
}

/**
 * Keep a known turn-in placeholder out of ordinary item acquisition.
 * @param vendorHash Installed vendor hash.
 * @param placeholderHash Requested item hash.
 * @return True when this vendor uses the item as a turn-in placeholder.
 */
bool is_reputation_placeholder(std::uint32_t vendorHash, std::uint32_t placeholderHash) noexcept {
    const std::shared_lock guard(g_lock);
    for (std::size_t index = 0; index < g_saleCount; ++index) {
        if (g_sales[index].vendorHash == vendorHash
            && g_sales[index].placeholderHash == placeholderHash) {
            return true;
        }
    }
    return false;
}

} // namespace sunrise::state::build_data::vendors
