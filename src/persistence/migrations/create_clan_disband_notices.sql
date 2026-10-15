-- Additive migration 14. Run during maintenance after the first 13 migrations.
-- Deliberately no FK to Clans: notices survive the clan's deletion.
CREATE TABLE ClanDisbandNotices (
    CharacterId BIGINT UNSIGNED NOT NULL,
    ClanId INT UNSIGNED NOT NULL,
    NameUtf16 VARBINARY(24) NOT NULL,
    DisbandedAtUnixSeconds BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (CharacterId),
    CONSTRAINT FK_ClanDisbandNotices_Character FOREIGN KEY (CharacterId)
        REFERENCES Characters(CharacterId) ON DELETE CASCADE,
    CONSTRAINT CK_ClanDisbandNotices_Clan CHECK (ClanId > 0 AND ClanId <> 99),
    CONSTRAINT CK_ClanDisbandNotices_Name CHECK
        (OCTET_LENGTH(NameUtf16) BETWEEN 8 AND 24 AND MOD(OCTET_LENGTH(NameUtf16), 2) = 0),
    CONSTRAINT CK_ClanDisbandNotices_Date CHECK
        (DisbandedAtUnixSeconds > 0 AND DisbandedAtUnixSeconds < 4102444800)
) ENGINE=InnoDB;
