-- Sunrise's own starting state for a created character, matching the default characters.
-- Starter items come before the packages' new-character table, which names no subclass.
-- Starter unlocks are the per-character unlock rows every character starts with.
-- Classes: 0 Titan, 1 Hunter, 2 Warlock. Unlock banks 2, 4, 5 and 7 are per-character.
-- Every statement is idempotent, so an upgrade may run it on a save that has some tables.
CREATE TABLE IF NOT EXISTS starter_character (
    id INTEGER PRIMARY KEY CHECK (id = 1),
    level INTEGER NOT NULL CHECK (level BETWEEN 0 AND 255),
    preview_available INTEGER NOT NULL CHECK (preview_available IN (0, 1)),
    appearance_value REAL NOT NULL,
    last_orbited_destination INTEGER NOT NULL,
    content_bypass INTEGER NOT NULL CHECK (content_bypass IN (0, 1))
) STRICT;

CREATE TABLE IF NOT EXISTS starter_items (
    class INTEGER NOT NULL CHECK (class BETWEEN 0 AND 2),
    position INTEGER NOT NULL CHECK (position >= 0),
    definition_hash INTEGER NOT NULL CHECK (definition_hash BETWEEN 1 AND 4294967295),
    PRIMARY KEY (class, position)
) STRICT;

CREATE TABLE IF NOT EXISTS starter_unlocks (
    bank INTEGER NOT NULL CHECK (bank IN (2, 4, 5, 7)),
    slot INTEGER NOT NULL CHECK (slot >= 0),
    lane INTEGER NOT NULL CHECK (lane BETWEEN 0 AND 2),
    value INTEGER NOT NULL CHECK (value BETWEEN -2147483648 AND 2147483647),
    PRIMARY KEY (bank, slot, lane)
) STRICT;

INSERT OR IGNORE INTO starter_character VALUES (1, 50, 1, 1.0, 308080871, 1);
INSERT OR IGNORE INTO starter_items VALUES (0, 0, 3105935002);
INSERT OR IGNORE INTO starter_items VALUES (1, 0, 3635991036);
INSERT OR IGNORE INTO starter_items VALUES (2, 0, 3481861797);
INSERT OR IGNORE INTO starter_unlocks VALUES (2, 16, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (2, 17, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (2, 18, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (2, 19, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (2, 82, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (2, 90, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 61, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 120, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 130, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 135, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 138, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 139, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 141, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 200, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 223, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 273, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 275, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 276, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 312, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 360, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 409, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 410, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 411, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 412, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 416, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 418, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 419, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 420, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 421, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 423, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 424, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 425, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 426, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 427, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 428, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 446, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 447, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 458, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 459, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 470, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 524, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 581, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 582, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 583, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 584, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 659, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 661, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 699, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 707, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 731, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 744, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 791, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 794, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 817, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 821, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 828, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 833, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 838, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 849, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 850, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 852, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 859, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 865, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 924, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 938, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 946, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 957, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 959, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 964, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 966, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 971, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 980, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 994, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1013, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1055, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1058, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1065, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1066, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1067, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1068, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1131, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1134, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1136, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1137, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1139, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1143, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1145, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1146, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1171, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1178, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1182, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1183, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1195, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1197, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1212, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1213, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1214, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1232, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1234, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1235, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1236, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1247, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1252, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1253, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1256, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1259, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1264, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1326, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1422, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1428, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1429, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1514, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1516, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1518, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1520, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1524, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1542, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1544, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1662, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1663, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1664, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1665, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1666, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1667, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1678, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1679, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1680, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1685, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1783, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1790, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1791, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1837, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1838, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1839, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1840, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1841, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1842, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1848, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1849, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1853, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1854, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1855, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1858, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1859, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1860, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1861, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1880, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1881, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1882, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1883, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1884, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 1885, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 2253, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 2254, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 2255, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 2256, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 2257, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 2258, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (4, 2269, 0, 2);
INSERT OR IGNORE INTO starter_unlocks VALUES (5, 443, 0, -1);
INSERT OR IGNORE INTO starter_unlocks VALUES (5, 502, 0, -1);
