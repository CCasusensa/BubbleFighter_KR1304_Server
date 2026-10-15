-- Twelfth additive migration. Main C806 confirms an already committed clan
-- creation; it must never consume the card a second time.
CREATE TABLE ClanCreationReceipts (
    ClanId INT UNSIGNED NOT NULL,
    CharacterId BIGINT UNSIGNED NOT NULL,
    ItemId INT UNSIGNED NOT NULL,
    InventoryRevision BIGINT UNSIGNED NOT NULL,
    CreatedAtUnixSeconds BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (ClanId),
    KEY IX_ClanCreationReceipts_OwnerItem (CharacterId, ItemId),
    CONSTRAINT FK_ClanCreationReceipts_Character FOREIGN KEY (CharacterId)
        REFERENCES Characters(CharacterId) ON DELETE CASCADE,
    CONSTRAINT CK_ClanCreationReceipts_Item CHECK (ItemId = 100819),
    CONSTRAINT CK_ClanCreationReceipts_Revision CHECK (InventoryRevision > 0),
    CONSTRAINT CK_ClanCreationReceipts_Time CHECK (CreatedAtUnixSeconds < 4102444800)
) ENGINE=InnoDB;
-- No foreign key to Clans: historical consumption receipts survive a later
-- disband, so a delayed confirmation still cannot initiate another debit.
