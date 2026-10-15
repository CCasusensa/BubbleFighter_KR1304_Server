-- Sixteenth migration. Depends on create_social_relationships.sql.
-- Personal presentation groups never grant friendship or messaging permission.
CREATE TABLE SocialFriendGroups (
    GroupId BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    OwnerCharacterId BIGINT UNSIGNED NOT NULL,
    NameUtf16 VARBINARY(62) NOT NULL,
    CreatedAt TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (GroupId),
    UNIQUE KEY UX_SocialFriendGroups_OwnerId (OwnerCharacterId, GroupId),
    UNIQUE KEY UX_SocialFriendGroups_OwnerName (OwnerCharacterId, NameUtf16),
    CONSTRAINT FK_SocialFriendGroups_Owner FOREIGN KEY (OwnerCharacterId)
        REFERENCES Characters(CharacterId) ON DELETE CASCADE,
    CONSTRAINT CK_SocialFriendGroups_Name CHECK (OCTET_LENGTH(NameUtf16) BETWEEN 2 AND 62 AND MOD(OCTET_LENGTH(NameUtf16), 2) = 0)
) ENGINE=InnoDB;

CREATE TABLE SocialFriendGroupAssignments (
    OwnerCharacterId BIGINT UNSIGNED NOT NULL,
    FriendCharacterId BIGINT UNSIGNED NOT NULL,
    GroupId BIGINT UNSIGNED NOT NULL,
    FirstCharacterId BIGINT UNSIGNED NOT NULL,
    SecondCharacterId BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (OwnerCharacterId, FriendCharacterId),
    KEY IX_SocialFriendGroupAssignments_Pair (FirstCharacterId, SecondCharacterId),
    CONSTRAINT FK_SocialFriendGroupAssignments_Group FOREIGN KEY (OwnerCharacterId, GroupId)
        REFERENCES SocialFriendGroups(OwnerCharacterId, GroupId) ON DELETE CASCADE,
    CONSTRAINT FK_SocialFriendGroupAssignments_Friends FOREIGN KEY (FirstCharacterId, SecondCharacterId)
        REFERENCES SocialFriendships(FirstCharacterId, SecondCharacterId) ON DELETE CASCADE,
    CONSTRAINT CK_SocialFriendGroupAssignments_Pair CHECK (
        (OwnerCharacterId = FirstCharacterId AND FriendCharacterId = SecondCharacterId)
        OR (OwnerCharacterId = SecondCharacterId AND FriendCharacterId = FirstCharacterId))
) ENGINE=InnoDB;
