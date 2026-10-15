-- Ninth project migration. Existing pages and inventory remain unchanged.
-- Equipped inventory imports the active page's new slots on the next load.
ALTER TABLE WeaponPresetSlots
    ADD COLUMN GrenadeItemId INT UNSIGNED NOT NULL DEFAULT 0,
    ADD COLUMN CrystalItemId INT UNSIGNED NOT NULL DEFAULT 0,
    ADD CONSTRAINT CK_WeaponPresetSlots_Grenade CHECK (GrenadeItemId = 0 OR GrenadeItemId BETWEEN 200001 AND 299999),
    ADD CONSTRAINT CK_WeaponPresetSlots_Crystal CHECK (CrystalItemId = 0 OR CrystalItemId BETWEEN 100001 AND 199999);
