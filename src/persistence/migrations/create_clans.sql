-- Eleventh additive migration. Applied only by an administrator.
-- This schema stores trusted membership, never client-reported authority.
CREATE TABLE Clans (
    ClanId INT UNSIGNED NOT NULL AUTO_INCREMENT,
    NameUtf16 VARBINARY(24) NOT NULL,
    IntroductionUtf16 VARBINARY(192) NOT NULL,
    MemberCapacity INT UNSIGNED NOT NULL,
    MarkId INT UNSIGNED NOT NULL DEFAULT 0,
    ClanLevel INT UNSIGNED NOT NULL DEFAULT 1,
    ClanPoints INT UNSIGNED NOT NULL DEFAULT 0,
    IsRecruiting TINYINT UNSIGNED NOT NULL DEFAULT 0,
    CreatedAtUnixSeconds BIGINT UNSIGNED NOT NULL,
    DisbandAtUnixSeconds BIGINT UNSIGNED NOT NULL DEFAULT 0,
    PRIMARY KEY (ClanId),
    UNIQUE KEY UX_Clans_Name (NameUtf16),
    CONSTRAINT CK_Clans_Name CHECK (OCTET_LENGTH(NameUtf16) BETWEEN 8 AND 24 AND MOD(OCTET_LENGTH(NameUtf16), 2) = 0),
    CONSTRAINT CK_Clans_Introduction CHECK (OCTET_LENGTH(IntroductionUtf16) BETWEEN 2 AND 192 AND MOD(OCTET_LENGTH(IntroductionUtf16), 2) = 0),
    CONSTRAINT CK_Clans_Capacity CHECK (MemberCapacity BETWEEN 1 AND 65535),
    CONSTRAINT CK_Clans_Level CHECK (ClanLevel > 0),
    CONSTRAINT CK_Clans_Recruiting CHECK (IsRecruiting IN (0, 1)),
    CONSTRAINT CK_Clans_Created CHECK (CreatedAtUnixSeconds < 4102444800),
    CONSTRAINT CK_Clans_Disband CHECK (DisbandAtUnixSeconds = 0 OR (DisbandAtUnixSeconds >= CreatedAtUnixSeconds AND DisbandAtUnixSeconds < 4102444800))
) ENGINE=InnoDB AUTO_INCREMENT = 1000;

CREATE TABLE ClanMembers (
    CharacterId BIGINT UNSIGNED NOT NULL,
    ClanId INT UNSIGNED NOT NULL,
    MembershipStatus INT UNSIGNED NOT NULL,
    OfficerSlot TINYINT UNSIGNED GENERATED ALWAYS AS
        (CASE WHEN MembershipStatus = 101 THEN 1 WHEN MembershipStatus = 102 THEN 2 ELSE NULL END) STORED,
    PRIMARY KEY (CharacterId),
    KEY IX_ClanMembers_Clan (ClanId),
    UNIQUE KEY UX_ClanMembers_Officer (ClanId, OfficerSlot),
    CONSTRAINT FK_ClanMembers_Character FOREIGN KEY (CharacterId) REFERENCES Characters(CharacterId) ON DELETE CASCADE,
    CONSTRAINT FK_ClanMembers_Clan FOREIGN KEY (ClanId) REFERENCES Clans(ClanId) ON DELETE CASCADE,
    CONSTRAINT CK_ClanMembers_Status CHECK (MembershipStatus IN (1, 100, 101, 102))
) ENGINE=InnoDB;
