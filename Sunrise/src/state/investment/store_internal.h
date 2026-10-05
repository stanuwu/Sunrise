#pragma once

#include <bit>
#include <cmath>
#include <limits>
#include <mutex>
#include <sqlite3.h>
#include <string>
#include <type_traits>

#include "../account/account_context.h"
#include "store.h"

namespace sunrise::state::investment::store {

extern sqlite3* g_database;
extern std::recursive_mutex g_mutex;
extern std::uint64_t g_failureSerial;

/** Only session fields survive between database calls. */
struct Session {
    std::array<bool, kCharacterCapacity> selected{};
    std::array<std::uint16_t, kCharacterCapacity> activities{};
    std::uint64_t signInSeconds{};
};
extern Session g_session;

[[nodiscard]] bool execute(const char* sql) noexcept;
[[nodiscard]] sqlite3_destructor_type copy_text() noexcept;

/** A failed write rolls back before releasing the database lock. */
class Transaction {
public:
    Transaction() noexcept;
    ~Transaction();
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;
    [[nodiscard]] bool ready() const noexcept {
        return active_;
    }
    [[nodiscard]] bool commit() noexcept;

private:
    std::unique_lock<std::recursive_mutex> lock_;
    bool active_{};
    Session before_;
    std::uint64_t failureSerial_{};
};

/** Statements never outlive the locked database call that owns them. */
class Statement {
public:
    explicit Statement(const char* sql) noexcept;
    ~Statement();
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    [[nodiscard]] int step() noexcept;
    [[nodiscard]] bool text(int column, std::string_view& value) const noexcept;

    /** Reads one typed SQL scalar without narrowing an out-of-range value. */
    template <typename T> [[nodiscard]] bool column(int index, T& value) const noexcept {
        if (!local_account_access() || statement_ == nullptr) {
            return false;
        }
        if constexpr (std::is_enum_v<T>) {
            std::underlying_type_t<T> raw{};
            if (!column(index, raw)) {
                return false;
            }
            value = static_cast<T>(raw);
        } else if constexpr (std::is_floating_point_v<T>) {
            const double raw = sqlite3_column_double(statement_, index);
            if (!std::isfinite(raw) || std::abs(raw) > (std::numeric_limits<T>::max)()) {
                return false;
            }
            value = static_cast<T>(raw);
        } else {
            if (sqlite3_column_type(statement_, index) != SQLITE_INTEGER) {
                return false;
            }
            const auto raw = sqlite3_column_int64(statement_, index);
            if constexpr (std::is_same_v<T, std::uint64_t>) {
                value = std::bit_cast<std::uint64_t>(static_cast<std::int64_t>(raw));
            } else {
                if (raw < static_cast<sqlite3_int64>((std::numeric_limits<T>::min)())
                    || raw > static_cast<sqlite3_int64>((std::numeric_limits<T>::max)())) {
                    return false;
                }
                value = static_cast<T>(raw);
            }
        }
        return true;
    }

    template <typename... T> [[nodiscard]] bool columns(T&... values) const noexcept {
        int index = 0;
        return (column(index++, values) && ...);
    }

    /** Reuses one prepared insert for a bounded set of typed rows. */
    template <typename... T> [[nodiscard]] bool parameters(const T&... values) noexcept {
        if (statement_ == nullptr) {
            return false;
        }
        sqlite3_reset(statement_);
        sqlite3_clear_bindings(statement_);
        int index = 1;
        return (bind(index++, values) && ...);
    }

    template <typename... T> [[nodiscard]] bool write(const T&... values) noexcept {
        return parameters(values...) && step() == SQLITE_DONE;
    }

private:
    /** SQL integers preserve all 64 identity bits; text bindings copy their input. */
    template <typename T> [[nodiscard]] bool bind(int index, T value) noexcept {
        if constexpr (std::is_convertible_v<T, std::string_view>) {
            const std::string_view textValue(value);
            return sqlite3_bind_text64(statement_,
                                       index,
                                       textValue.data(),
                                       textValue.size(),
                                       copy_text(),
                                       SQLITE_UTF8)
                   == SQLITE_OK;
        } else if constexpr (std::is_floating_point_v<T>) {
            return sqlite3_bind_double(statement_, index, value) == SQLITE_OK;
        } else if constexpr (std::is_same_v<T, std::uint64_t>) {
            return sqlite3_bind_int64(statement_, index, std::bit_cast<std::int64_t>(value))
                   == SQLITE_OK;
        } else {
            return sqlite3_bind_int64(statement_, index, static_cast<sqlite3_int64>(value))
                   == SQLITE_OK;
        }
    }
    sqlite3_stmt* statement_{};
};

[[nodiscard]] bool read_inventory(AccountState& output) noexcept;
[[nodiscard]] bool write_inventory(const AccountState& value) noexcept;

} // namespace sunrise::state::investment::store
