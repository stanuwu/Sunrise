#include "../account/account_context.h"
#include "store_internal.h"

namespace sunrise::state::investment::store {

sqlite3* g_database{};
// Transactions hold this across nested store/statement operations, so the lock must be recursive.
std::recursive_mutex g_mutex;
Session g_session;
std::uint64_t g_failureSerial{};

/** SQLite requires this sentinel to copy text borrowed from a caller. */
sqlite3_destructor_type copy_text() noexcept {
    return SQLITE_TRANSIENT; // NOLINT(performance-no-int-to-ptr)
}

/** SQL failures remain failures; no in-memory save replaces a failed disk write. */
bool execute(const char* sql) noexcept {
    const bool ready = local_account_access() && g_database != nullptr
                       && sqlite3_exec(g_database, sql, nullptr, nullptr, nullptr) == SQLITE_OK;
    if (!ready) {
        ++g_failureSerial;
    }
    return ready;
}

/** Nested saves belong to the outer transaction until it commits. */
Transaction::Transaction() noexcept
    : lock_(g_mutex), active_(execute("SAVEPOINT investment_write")), before_(g_session),
      failureSerial_(g_failureSerial) {}

/** A refused transaction leaves no partial rows. */
Transaction::~Transaction() {
    if (active_) {
        (void)execute("ROLLBACK TO investment_write");
        (void)execute("RELEASE investment_write");
        g_session = before_;
    }
}

/** @return True only after SQLite has committed every row. */
bool Transaction::commit() noexcept {
    if (!active_ || failureSerial_ != g_failureSerial || !execute("RELEASE investment_write")) {
        return false;
    }
    active_ = false;
    return true;
}

/** The caller holds the database lock until this statement is destroyed. */
Statement::Statement(const char* sql) noexcept {
    if (!local_account_access()) {
        ++g_failureSerial;
        return;
    }
    if (g_database != nullptr
        && sqlite3_prepare_v2(g_database, sql, -1, &statement_, nullptr) != SQLITE_OK) {
        sqlite3_finalize(statement_);
        statement_ = nullptr;
        ++g_failureSerial;
    }
}

Statement::~Statement() {
    sqlite3_finalize(statement_);
}
int Statement::step() noexcept {
    const int result =
        local_account_access() && statement_ != nullptr ? sqlite3_step(statement_) : SQLITE_ERROR;
    if (result != SQLITE_ROW && result != SQLITE_DONE) {
        ++g_failureSerial;
    }
    return result;
}

/** @return Borrowed text valid until the statement advances. */
bool Statement::text(int column, std::string_view& value) const noexcept {
    if (!local_account_access() || statement_ == nullptr
        || sqlite3_column_type(statement_, column) != SQLITE_TEXT) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement_, column));
    if (bytes == nullptr) {
        return false;
    }
    value = {bytes, static_cast<std::size_t>(sqlite3_column_bytes(statement_, column))};
    return true;
}

/**
 * Opens the save, creating it from the bundled SQL or upgrading an older schema in place.
 * A new database gets the schema and every default in one durable transaction.
 * @param path Save file name, or `:memory:`.
 * @param schema Investment schema SQL.
 * @param defaults Investment default rows SQL.
 * @param settingsSchema Account preference schema SQL.
 * @param settingsDefaults Account preference default rows SQL.
 * @param characterStarter Created-character starting tables and rows SQL.
 * @return False when the save does not open, is not ours, or does not upgrade.
 */
bool open(std::string_view path,
          std::string_view schema,
          std::string_view defaults,
          std::string_view settingsSchema,
          std::string_view settingsDefaults,
          std::string_view characterStarter) noexcept {
    const std::lock_guard lock(g_mutex);
    if (!local_account_access() || g_database != nullptr) {
        return false;
    }
    const std::string filename(path);
    if (sqlite3_open_v2(filename.c_str(),
                        &g_database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr)
        != SQLITE_OK) {
        shutdown();
        return false;
    }
    // One short busy wait permits a local database editor to finish its transaction.
    constexpr int kBusyMilliseconds = 5000;
    sqlite3_busy_timeout(g_database, kBusyMilliseconds);
    bool ready = execute("PRAGMA foreign_keys=ON; PRAGMA synchronous=FULL;");
    int version = -1;
    {
        Statement query("PRAGMA user_version");
        ready = ready && query.step() == SQLITE_ROW && query.column(0, version);
    }
    const std::string preferenceSchema(settingsSchema);
    const std::string preferenceDefaults(settingsDefaults);
    const std::string starterText(characterStarter);
    if (ready && version == 0) {
        Transaction transaction;
        const std::string schemaText(schema);
        const std::string defaultText(defaults);
        ready = transaction.ready() && !schema.empty() && !defaults.empty()
                && execute(schemaText.c_str()) && execute(defaultText.c_str())
                && !settingsSchema.empty() && !settingsDefaults.empty()
                && execute(preferenceSchema.c_str()) && execute(preferenceDefaults.c_str())
                && !characterStarter.empty() && execute(starterText.c_str())
                && transaction.commit();
    } else if (ready) {
        // Version 3 adds the appearance header and the starter tables. Version 2 added account
        // preferences and per-item seen state.
        constexpr int kSchemaVersion = 3;
        constexpr int kApplicationId = 1397902921;
        int application = 0;
        Statement query("PRAGMA application_id");
        ready = (version == 1 || version == 2 || version == kSchemaVersion)
                && query.step() == SQLITE_ROW && query.column(0, application)
                && application == kApplicationId;
    }
    if (ready && version == 1) {
        Transaction transaction;
        ready =
            transaction.ready() && !settingsSchema.empty() && !settingsDefaults.empty()
            && execute(
                "ALTER TABLE items ADD COLUMN seen INTEGER NOT NULL DEFAULT 0 CHECK(seen IN(0,1));"
                "ALTER TABLE profile_items ADD COLUMN seen INTEGER NOT NULL DEFAULT 1 CHECK(seen "
                "IN(0,1));")
            && execute(preferenceSchema.c_str()) && execute(preferenceDefaults.c_str())
            && execute("PRAGMA user_version=2") && transaction.commit();
        if (ready) {
            version = 2;
        }
    }
    if (ready && version == 2) {
        Transaction transaction;
        // Older characters keep the fixed default header: their zero blob is marked invalid.
        // ADD COLUMN refuses a function default such as zeroblob(), so the blob is a literal.
        ready = transaction.ready() && !characterStarter.empty()
                && execute("ALTER TABLE characters ADD COLUMN appearance_header BLOB NOT NULL "
                           "DEFAULT (X'000000000000000000000000000000000000000000000000000000000000"
                           "000000000000') CHECK (length(appearance_header) = 36);"
                           "ALTER TABLE characters ADD COLUMN appearance_header_valid INTEGER "
                           "NOT NULL DEFAULT 0 CHECK (appearance_header_valid IN (0, 1));")
                && execute(starterText.c_str()) && execute("PRAGMA user_version=3")
                && transaction.commit();
    }
    if (!ready) {
        shutdown();
    }
    return ready;
}

/**
 * Copies the open save into a `:memory:` database with the backup API, then swaps the handle.
 * Statements never outlive a locked call, so none holds the old handle across the swap.
 * @return False when the copy failed; the file handle then stays in use.
 */
bool use_session_copy() noexcept {
    // SQLite names an in-memory database by this file name, and a database's main schema so.
    constexpr char kMemoryDatabase[] = ":memory:";
    constexpr char kMainSchema[] = "main";
    // A negative page count copies every page in one step.
    constexpr int kAllPages = -1;
    const std::lock_guard lock(g_mutex);
    if (g_database == nullptr) {
        return false;
    }
    sqlite3* memory = nullptr;
    if (sqlite3_open_v2(kMemoryDatabase,
                        &memory,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                        nullptr)
        != SQLITE_OK) {
        sqlite3_close_v2(memory);
        return false;
    }
    sqlite3_backup* const backup =
        sqlite3_backup_init(memory, kMainSchema, g_database, kMainSchema);
    const bool copied = backup != nullptr && sqlite3_backup_step(backup, kAllPages) == SQLITE_DONE;
    // Finish runs for every started backup; it releases the source read lock.
    const bool finished = sqlite3_backup_finish(backup) == SQLITE_OK;
    // Foreign keys are a connection setting, so the copy needs them switched on again.
    const bool ready =
        copied && finished
        && sqlite3_exec(memory, "PRAGMA foreign_keys=ON;", nullptr, nullptr, nullptr) == SQLITE_OK;
    if (!ready) {
        sqlite3_close_v2(memory);
        return false;
    }
    sqlite3_close_v2(g_database);
    g_database = memory;
    return true;
}

/** Saves are synchronous, so shutdown only closes the handle and discards session fields. */
void shutdown() noexcept {
    const std::lock_guard lock(g_mutex);
    // This process owns the database even when teardown runs under a public request scope.
    sqlite3_close_v2(g_database);
    g_database = nullptr;
    g_session = {};
}

} // namespace sunrise::state::investment::store
