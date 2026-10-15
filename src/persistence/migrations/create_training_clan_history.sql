-- Fifteenth additive migration; administrator only. First CREATE refuses replay.
CREATE TABLE TrainingClanHistory (
    CharacterId BIGINT UNSIGNED NOT NULL,
    LastChangedAtUnixSeconds BIGINT UNSIGNED NOT NULL,
    ExitStatus INT UNSIGNED NOT NULL,
    Graduated TINYINT UNSIGNED NOT NULL,
    PRIMARY KEY (CharacterId),
    CONSTRAINT FK_TrainingClanHistory_Character FOREIGN KEY (CharacterId) REFERENCES Characters(CharacterId) ON DELETE CASCADE,
    CONSTRAINT CK_TrainingClanHistory_Time CHECK (LastChangedAtUnixSeconds > 0 AND LastChangedAtUnixSeconds < 4102444800),
    CONSTRAINT CK_TrainingClanHistory_Status CHECK (ExitStatus IN (0, 4294967276, 4294967236)),
    CONSTRAINT CK_TrainingClanHistory_Graduated CHECK (Graduated IN (0, 1) AND (ExitStatus <> 4294967236 OR Graduated = 1))
) ENGINE=InnoDB;

-- The original training label is three UTF-16 units. Ordinary creation retains
-- its stricter four-unit application rule. MySQL CHECK cannot reference the
-- auto-increment ClanId column, so this structural bound applies to every row.
ALTER TABLE Clans DROP CHECK CK_Clans_Name,
    ADD CONSTRAINT CK_Clans_Name CHECK (OCTET_LENGTH(NameUtf16) BETWEEN 6 AND 24 AND MOD(OCTET_LENGTH(NameUtf16), 2) = 0);
ALTER TABLE ClanMembers ADD CONSTRAINT CK_ClanMembers_TrainingRole CHECK (ClanId <> 99 OR MembershipStatus = 100);
