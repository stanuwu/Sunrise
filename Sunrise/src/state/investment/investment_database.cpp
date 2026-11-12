#include "../account/account_context.h"
#include "store_internal.h"

namespace sunrise::state::investment::store {

sqlite3* g_database{};
// Transactions hold this across nested store/statement operations, so the lock must be recursive.
std::recursive_mutex g_mutex;
Session g_session;
std::uint64_t g_failureSerial{};

/**
 * Rows whose definition the installed content lacks. A new save and the version 5 migration both
 * create the table from this text, so the two cannot differ.
 */
constexpr const char* kParkedItemsTable =
    "CREATE TABLE parked_items ("
    "id INTEGER PRIMARY KEY AUTOINCREMENT,"
    "kind INTEGER NOT NULL CHECK (kind IN (0, 1, 2)),"
    "character_slot INTEGER NOT NULL CHECK (character_slot BETWEEN -1 AND 2),"
    "location INTEGER NOT NULL CHECK (location IN (0, 1)),"
    "position INTEGER NOT NULL CHECK (position >= 0),"
    "instance_soid INTEGER NOT NULL,"
    "definition_hash INTEGER NOT NULL CHECK (definition_hash BETWEEN 1 AND 4294967295),"
    "level INTEGER NOT NULL,"
    "quantity INTEGER NOT NULL CHECK (quantity > 0),"
    "mutation_serial INTEGER NOT NULL CHECK (mutation_serial >= 0),"
    "flags INTEGER NOT NULL,"
    "socket_policy INTEGER NOT NULL CHECK (socket_policy IN (0, 1)),"
    "plug_count INTEGER NOT NULL CHECK (plug_count BETWEEN 0 AND 12),"
    "plugs BLOB NOT NULL CHECK (length(plugs) = 48),"
    "movement_ability INTEGER NOT NULL,"
    "grenade_ability INTEGER NOT NULL,"
    "super_ability INTEGER NOT NULL,"
    "melee_ability INTEGER NOT NULL,"
    "class_ability INTEGER NOT NULL,"
    "seen INTEGER NOT NULL CHECK (seen IN (0, 1))"
    ") STRICT;";

/** Records which bucket a parked row left; a new save gets the column at creation. */
constexpr const char* kParkedPlacementColumn =
    "ALTER TABLE parked_items ADD COLUMN placement INTEGER NOT NULL DEFAULT 0 "
    "CHECK (placement IN (0, 1));";

/**
 * Saves each item's per-instance values, 4 little-endian bytes per lane; an item
 * without a row publishes none. A new save and the migration both run these statements.
 */
constexpr const char* kItemObjectivesTable =
    "CREATE TABLE item_objectives ("
    "instance_soid INTEGER PRIMARY KEY REFERENCES items(instance_soid) ON DELETE CASCADE,"
    "lanes BLOB NOT NULL CHECK (length(lanes) = 32)"
    ") STRICT;";
constexpr const char* kParkedObjectivesColumn =
    "ALTER TABLE parked_items ADD COLUMN objectives BLOB "
    "CHECK (objectives IS NULL OR length(objectives) = 32);";
static_assert(sizeof(ObjectiveBlob) == 32);

/**
 * Saves each character's Gambit Prime helmet tier per role. A new save and the
 * migration both run this statement.
 */
constexpr const char* kCharacterHelmetTiersTable =
    "CREATE TABLE character_helmet_tiers ("
    "character_slot INTEGER PRIMARY KEY REFERENCES characters(slot),"
    "reaper INTEGER NOT NULL CHECK (reaper BETWEEN 0 AND 3),"
    "invader INTEGER NOT NULL CHECK (invader BETWEEN 0 AND 3),"
    "collector INTEGER NOT NULL CHECK (collector BETWEEN 0 AND 3),"
    "sentry INTEGER NOT NULL CHECK (sentry BETWEEN 0 AND 3)"
    ") STRICT;";

/**
 * Removes the default Family-5 row that overrides every character's Synthesizer tier
 * (kind 1, slot 5159, value 3) and closes the gap. A row edited to another value stays.
 */
constexpr const char* kSynthesizerTierOverrideRemoval =
    "CREATE TEMP TABLE family5_kept AS SELECT kind,position,slot,value FROM family5 "
    "WHERE kind<>1 OR slot<>5159 OR value<>3;"
    "DELETE FROM family5;"
    "INSERT INTO family5 SELECT kind,ROW_NUMBER() OVER (PARTITION BY kind ORDER BY position)-1,"
    "slot,value FROM family5_kept;"
    "DROP TABLE family5_kept;";

/**
 * Saves the Store wallet sync generation, which outlives every account row rewrite.
 * A new save and the migration both run this statement.
 */
constexpr const char* kWalletSyncTable =
    "CREATE TABLE eververse_wallet_sync ("
    "account_soid INTEGER PRIMARY KEY CHECK (account_soid <> 0),"
    "revision INTEGER NOT NULL CHECK (revision BETWEEN 1 AND 4294967295)"
    ") STRICT, WITHOUT ROWID;";

/**
 * Gives each profile row that still has serial 0 its own, from 1 in saved order, and moves the
 * rows numbered since above them.
 */
constexpr const char* kProfileSerialNumbering =
    "CREATE TEMP TABLE profile_unnumbered AS SELECT position,"
    "ROW_NUMBER() OVER (ORDER BY position) AS serial FROM profile_items WHERE mutation_serial=0;"
    "UPDATE profile_items SET mutation_serial=mutation_serial"
    "+(SELECT COUNT(*) FROM profile_unnumbered) WHERE mutation_serial>0;"
    "UPDATE profile_items SET mutation_serial=(SELECT serial FROM profile_unnumbered "
    "WHERE profile_unnumbered.position=profile_items.position) WHERE mutation_serial=0;"
    "DROP TABLE profile_unnumbered;";

namespace {

/** Newest save schema; open() upgrades an older save one version at a time. */
constexpr int kSchemaVersion = 7;
/** Application id every Sunrise save carries; the bundled schema sets it. */
constexpr int kApplicationId = 1397902921;

/** The development save already has these counters. Preserve its rows during the upgrade. */
constexpr const char* kCharacterObjectivesTable =
    "CREATE TABLE IF NOT EXISTS character_objective_values ("
    "character_soid INTEGER NOT NULL REFERENCES characters(soid) DEFERRABLE INITIALLY DEFERRED,"
    "slot INTEGER NOT NULL CHECK (slot BETWEEN 0 AND 32767),"
    "value INTEGER NOT NULL CHECK (value BETWEEN 0 AND 2147483647),"
    "PRIMARY KEY (character_soid,slot)) STRICT;";

/** Version 2 and the objective-only version 3 both predate character appearance headers. */
constexpr const char* kAddAppearanceColumns =
    "ALTER TABLE characters ADD COLUMN appearance_header BLOB NOT NULL "
    "DEFAULT (X'000000000000000000000000000000000000000000000000000000000000000000000000') "
    "CHECK (length(appearance_header) = 36);"
    "ALTER TABLE characters ADD COLUMN appearance_header_valid INTEGER "
    "NOT NULL DEFAULT 0 CHECK (appearance_header_valid IN (0, 1));";

/** Give characters without a captured header the starter appearance before retiring the flag. */
constexpr const char* kNormalizeAppearanceHeader =
    "UPDATE characters SET appearance_header=X'250005000100EA0201034602480349034A03FFFFFFFF1303"
    "C7020000C59D1C8101000000' WHERE appearance_header_valid=0;"
    "ALTER TABLE characters DROP COLUMN appearance_header_valid;";

/** Finish the schema query before a migration alters the inspected table. */
[[nodiscard]] bool schema_flag(const char* sql, bool& value) noexcept {
    Statement query(sql);
    return query.step() == SQLITE_ROW && query.column(0, value) && query.step() == SQLITE_DONE;
}

/**
 * Reads the open save's stamp.
 * @param version Receives the save's schema version.
 * @return False when the save is not ours or the stamp does not read.
 */
[[nodiscard]] bool read_stamp(int& version) noexcept {
    int application = 0;
    Statement id("PRAGMA application_id");
    Statement user("PRAGMA user_version");
    return id.step() == SQLITE_ROW && id.column(0, application) && application == kApplicationId
           && user.step() == SQLITE_ROW && user.column(0, version);
}

/** SQLite calls this only when a write transaction commits. Returning zero lets it proceed. */
int count_committed_write(void* /*context*/) noexcept {
    ++g_session.revision;
    return 0;
}

} // namespace

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

// The version 6 tables bound item and stack positions by these capacities, written as literals.
static_assert(account::inventory::kCharacterItemCapacity == 333);
static_assert(account::inventory::kCharacterStackCapacity == 333);

/**
 * Widens saved row bounds to the character ABI, adds item, stack and parked-row placement, and
 * drops the world reward kind: one grant path places every reward. Adds per-item objectives,
 * helmet tiers and the wallet sync generation, removes the Synthesizer tier override and numbers
 * the profile rows.
 */
static bool migrate_investment_storage() noexcept {
    // SQLite cannot change foreign_keys inside a savepoint. The open-time mutex is held and
    // no account readers exist yet; restore enforcement after the savepoint ends on either path.
    if (!execute("PRAGMA foreign_keys=OFF")) {
        return false;
    }
    const bool migrated = []() noexcept {
        Transaction transaction;
        if (!transaction.ready() || !execute(R"sql(
CREATE TABLE items_v6 (
    character_slot INTEGER NOT NULL REFERENCES characters(slot),
    location INTEGER NOT NULL CHECK (location IN (0, 1)),
    position INTEGER NOT NULL CHECK (position >= 0),
    instance_soid INTEGER NOT NULL UNIQUE,
    definition_hash INTEGER NOT NULL CHECK (definition_hash BETWEEN 1 AND 4294967295),
    level INTEGER NOT NULL,
    quantity INTEGER NOT NULL CHECK (quantity > 0),
    mutation_serial INTEGER NOT NULL CHECK (mutation_serial >= 0),
    flags INTEGER NOT NULL CHECK (flags BETWEEN 0 AND 7),
    socket_policy INTEGER NOT NULL CHECK (socket_policy IN (0, 1)),
    plug_count INTEGER NOT NULL CHECK (plug_count BETWEEN 0 AND 12),
    movement_ability INTEGER NOT NULL,
    grenade_ability INTEGER NOT NULL,
    super_ability INTEGER NOT NULL,
    melee_ability INTEGER NOT NULL,
    class_ability INTEGER NOT NULL,
    seen INTEGER NOT NULL CHECK (seen IN (0, 1)),
    placement INTEGER NOT NULL CHECK (placement IN (0, 1)),
    PRIMARY KEY (character_slot, location, position),
    CHECK ((location = 0 AND position < 17 AND placement = 0)
           OR (location = 1 AND position < 333)),
    CHECK (placement = 0 OR quantity = 1)
) STRICT;
INSERT INTO items_v6 SELECT items.*, 0 FROM items;
DROP TABLE items;
ALTER TABLE items_v6 RENAME TO items;
CREATE TABLE character_stacks_v6 (
    character_slot INTEGER NOT NULL REFERENCES characters(slot),
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 332),
    definition_hash INTEGER NOT NULL,
    quantity INTEGER NOT NULL CHECK (quantity > 0),
    mutation_serial INTEGER NOT NULL CHECK (mutation_serial >= 0),
    placement INTEGER NOT NULL CHECK (placement IN (0, 1)),
    PRIMARY KEY (character_slot, position)
) STRICT;
INSERT INTO character_stacks_v6 SELECT character_stacks.*, 0 FROM character_stacks;
DROP TABLE character_stacks;
ALTER TABLE character_stacks_v6 RENAME TO character_stacks;
CREATE TABLE pending_rewards_v6 (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    character_slot INTEGER NOT NULL CHECK (character_slot BETWEEN 0 AND 2),
    definition_hash INTEGER NOT NULL CHECK (definition_hash BETWEEN 1 AND 4294967295),
    quantity INTEGER NOT NULL CHECK (quantity > 0)
) STRICT;
INSERT INTO pending_rewards_v6 (id, character_slot, definition_hash, quantity)
    SELECT id, character_slot, definition_hash, quantity FROM pending_rewards;
DROP TABLE pending_rewards;
ALTER TABLE pending_rewards_v6 RENAME TO pending_rewards;
)sql") || !execute(kParkedPlacementColumn)
            || !execute(kItemObjectivesTable) || !execute(kParkedObjectivesColumn)
            || !execute(kCharacterHelmetTiersTable) || !execute(kSynthesizerTierOverrideRemoval)
            || !execute(kWalletSyncTable) || !execute(kProfileSerialNumbering)
            || !execute("PRAGMA user_version=6")) {
            return false;
        }
        // Only the rebuilt tables and the sockets that reference them are rechecked.
        Statement items("PRAGMA foreign_key_check(items)");
        Statement stacks("PRAGMA foreign_key_check(character_stacks)");
        Statement sockets("PRAGMA foreign_key_check(sockets)");
        return items.step() == SQLITE_DONE && stacks.step() == SQLITE_DONE
               && sockets.step() == SQLITE_DONE && transaction.commit();
    }();
    const bool enforced = execute("PRAGMA foreign_keys=ON");
    return migrated && enforced;
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
    sqlite3_commit_hook(g_database, count_committed_write, nullptr);
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
        // The default characters copy the starter unlock rows, so the starter tables come first.
        ready = transaction.ready() && !schema.empty() && !defaults.empty()
                && !characterStarter.empty() && execute(schemaText.c_str())
                && execute(kParkedItemsTable) && execute(kParkedPlacementColumn)
                && execute(kItemObjectivesTable) && execute(kParkedObjectivesColumn)
                && execute(kCharacterHelmetTiersTable) && execute(starterText.c_str())
                && execute(kWalletSyncTable) && execute(kCharacterObjectivesTable)
                && execute(defaultText.c_str()) && !settingsSchema.empty()
                && !settingsDefaults.empty() && execute(preferenceSchema.c_str())
                && execute(preferenceDefaults.c_str()) && transaction.commit();
        // The bundled schema stamps the save; it must name the version this build writes.
        int created = 0;
        ready = ready && read_stamp(created) && created == kSchemaVersion;
    } else if (ready) {
        int stamped = 0;
        ready = version >= 1 && version <= kSchemaVersion && read_stamp(stamped);
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
        ready = transaction.ready() && execute(kAddAppearanceColumns)
                && execute("PRAGMA user_version=3") && transaction.commit();
        if (ready) {
            version = 3;
        }
    }
    if (ready && version == 3) {
        Transaction transaction;
        bool objectiveOnly = false;
        ready = transaction.ready() && !characterStarter.empty()
                && schema_flag("SELECT EXISTS(SELECT 1 FROM sqlite_schema WHERE type='table' AND "
                               "name='character_objective_values') AND NOT EXISTS(SELECT 1 FROM "
                               "pragma_table_info('characters') WHERE name IN "
                               "('appearance_header','appearance_header_valid')) AND NOT EXISTS("
                               "SELECT 1 FROM sqlite_schema WHERE type='table' AND name IN "
                               "('starter_character','starter_items'))",
                               objectiveOnly)
                && (!objectiveOnly || execute(kAddAppearanceColumns))
                && execute(kNormalizeAppearanceHeader) && execute(starterText.c_str())
                && execute("PRAGMA user_version=4") && transaction.commit();
        if (ready) {
            version = 4;
        }
    }
    if (ready && version == 4) {
        Transaction transaction;
        ready = transaction.ready() && execute(kParkedItemsTable)
                && execute("PRAGMA user_version=5") && transaction.commit();
        if (ready) {
            version = 5;
        }
    }
    if (ready && version == 5) {
        ready = migrate_investment_storage();
        if (ready) {
            version = 6;
        }
    }
    if (ready && version == 6) {
        // A vendor reputation branch save is stamped 6 without the version 6 storage tables.
        bool branchSave = false;
        ready = schema_flag("SELECT NOT EXISTS(SELECT 1 FROM sqlite_schema WHERE type='table' AND "
                            "name='item_objectives')",
                            branchSave)
                && (!branchSave || migrate_investment_storage());
    }
    if (ready && version == 6) {
        Transaction transaction;
        // Development version 4 kept the validity flag and predates the starter unlock rows.
        bool legacyHeader = false;
        ready = transaction.ready()
                && schema_flag("SELECT EXISTS(SELECT 1 FROM pragma_table_info('characters') "
                               "WHERE name='appearance_header_valid')",
                               legacyHeader)
                && (!legacyHeader
                    || (!characterStarter.empty() && execute(kNormalizeAppearanceHeader)
                        && execute(starterText.c_str())))
                && execute(kCharacterObjectivesTable) && execute("PRAGMA user_version=7")
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
    sqlite3_commit_hook(memory, count_committed_write, nullptr);
    sqlite3_close_v2(g_database);
    g_database = memory;
    return true;
}

/** The commit hook and selection writes move it under this same lock. */
std::uint64_t revision() noexcept {
    const std::lock_guard lock(g_mutex);
    return g_session.revision;
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
