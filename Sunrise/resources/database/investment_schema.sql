PRAGMA application_id = 1397902921;
PRAGMA user_version = 3;

CREATE TABLE account (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    soid INTEGER NOT NULL,
    profile_setup_completed INTEGER NOT NULL CHECK (profile_setup_completed IN (0, 1))
) STRICT;

CREATE TABLE characters (
    slot INTEGER PRIMARY KEY CHECK (slot BETWEEN 0 AND 2),
    soid INTEGER NOT NULL UNIQUE,
    race INTEGER NOT NULL CHECK (race BETWEEN 0 AND 2),
    gender INTEGER NOT NULL CHECK (gender BETWEEN 0 AND 1),
    class INTEGER NOT NULL CHECK (class BETWEEN 0 AND 2),
    level INTEGER NOT NULL CHECK (level BETWEEN 0 AND 255),
    preview_available INTEGER NOT NULL CHECK (preview_available IN (0, 1)),
    appearance_value REAL NOT NULL,
    last_orbited_destination INTEGER NOT NULL,
    content_bypass INTEGER NOT NULL CHECK (content_bypass IN (0, 1)),
    equipped_title INTEGER NOT NULL CHECK (equipped_title BETWEEN 0 AND 65535),
    acquired_subclass_mask INTEGER NOT NULL,
    next_inventory_serial INTEGER NOT NULL CHECK (next_inventory_serial BETWEEN 0 AND 4294967295)
) STRICT;

CREATE TABLE items (
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
    seen INTEGER NOT NULL CHECK(seen IN(0,1)),
    PRIMARY KEY (character_slot, location, position),
    CHECK ((location = 0 AND position < 17) OR (location = 1 AND position < 135))
) STRICT;

CREATE TABLE sockets (
    instance_soid INTEGER NOT NULL REFERENCES items(instance_soid) ON DELETE CASCADE,
    lane INTEGER NOT NULL CHECK (lane BETWEEN 0 AND 11),
    plug_hash INTEGER NOT NULL CHECK (plug_hash BETWEEN 1 AND 4294967295),
    PRIMARY KEY (instance_soid, lane)
) STRICT;

CREATE TABLE profile_items (
    position INTEGER PRIMARY KEY CHECK (position BETWEEN 0 AND 700),
    instance_soid INTEGER NOT NULL,
    definition_hash INTEGER NOT NULL,
    quantity INTEGER NOT NULL CHECK (quantity > 0),
    mutation_serial INTEGER NOT NULL CHECK (mutation_serial >= 0),
    seen INTEGER NOT NULL CHECK(seen IN(0,1))
) STRICT;

CREATE TABLE character_stacks (
    character_slot INTEGER NOT NULL REFERENCES characters(slot),
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 31),
    definition_hash INTEGER NOT NULL,
    quantity INTEGER NOT NULL CHECK (quantity > 0),
    mutation_serial INTEGER NOT NULL CHECK (mutation_serial >= 0),
    PRIMARY KEY (character_slot, position)
) STRICT;

CREATE TABLE dismantle_rewards (
    position INTEGER PRIMARY KEY CHECK (position BETWEEN 0 AND 31),
    definition_hash INTEGER NOT NULL,
    quantity INTEGER NOT NULL CHECK (quantity > 0),
    tier_mask INTEGER NOT NULL,
    class_mask INTEGER NOT NULL,
    masterwork INTEGER NOT NULL CHECK (masterwork BETWEEN 0 AND 2)
) STRICT;

CREATE TABLE unlocks (
    character_slot INTEGER NOT NULL CHECK (character_slot BETWEEN -1 AND 2),
    bank INTEGER NOT NULL CHECK (bank BETWEEN 0 AND 7),
    slot INTEGER NOT NULL CHECK (slot >= 0),
    lane INTEGER NOT NULL CHECK (lane BETWEEN 0 AND 2),
    value INTEGER NOT NULL CHECK (value BETWEEN -2147483648 AND 2147483647),
    PRIMARY KEY (character_slot, bank, slot, lane),
    CHECK ((bank IN (0, 1, 3, 6) AND character_slot = -1)
        OR (bank IN (2, 4, 5, 7) AND character_slot >= 0))
) STRICT;

CREATE TABLE family5 (
    kind INTEGER NOT NULL CHECK (kind IN (0, 1)),
    position INTEGER NOT NULL CHECK (position BETWEEN 0 AND 99),
    slot INTEGER NOT NULL CHECK (slot BETWEEN 0 AND 65535),
    value INTEGER NOT NULL CHECK (value BETWEEN -2147483648 AND 2147483647),
    PRIMARY KEY (kind, position),
    UNIQUE (kind, slot)
) STRICT;

CREATE TABLE character_objective_values (
    character_soid INTEGER NOT NULL REFERENCES characters(soid) DEFERRABLE INITIALLY DEFERRED,
    slot INTEGER NOT NULL CHECK (slot BETWEEN 0 AND 32767),
    value INTEGER NOT NULL CHECK (value BETWEEN 0 AND 2147483647),
    PRIMARY KEY (character_soid, slot)
) STRICT;

CREATE TABLE entitlements (
    position INTEGER PRIMARY KEY,
    name TEXT NOT NULL,
    ownership INTEGER NOT NULL CHECK (ownership BETWEEN 0 AND 2)
) STRICT;

CREATE TABLE bootstrap (
    name TEXT PRIMARY KEY,
    completed INTEGER NOT NULL CHECK (completed IN (0, 1))
) STRICT;

CREATE TABLE pending_rewards (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    character_slot INTEGER NOT NULL CHECK (character_slot BETWEEN 0 AND 2),
    kind INTEGER NOT NULL CHECK (kind IN (0, 1)),
    definition_hash INTEGER NOT NULL CHECK (definition_hash BETWEEN 1 AND 4294967295),
    quantity INTEGER NOT NULL CHECK (quantity > 0)
) STRICT;
