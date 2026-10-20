-- Build 86657.20.08.23.1800-9 Faction tokenValues and matching turn-in placeholders.
-- Installed vendor rows supply the sale selector, category, quantity, and current item identities.
-- The installed faction rows omit tokenValues, so only that missing relationship remains here.
CREATE TABLE faction_turn_ins (
    faction_hash INTEGER NOT NULL,
    placeholder_hash INTEGER NOT NULL,
    cost_hash INTEGER NOT NULL,
    xp_per_unit INTEGER NOT NULL,
    PRIMARY KEY (faction_hash, placeholder_hash, cost_hash)
) WITHOUT ROWID;

INSERT INTO faction_turn_ins VALUES
    -- Vanguard Tactician Tokens.
    (611314723, 3987308529, 3899548068, 100),
    -- Crucible Tokens.
    (697030790, 265113466, 183980811, 100),
    -- Weapon Telemetry and Gunsmith Materials.
    (1021210278, 3831705402, 685157381, 25),
    (1021210278, 3831705402, 685157383, 30),
    -- EDZ Tokens, Dusklight Crystals, and Dusklight Shards.
    (4235119312, 61430328, 2640973641, 100),
    (4235119312, 1317670974, 478751073, 250),
    (4235119312, 1317670974, 950899352, 50);
