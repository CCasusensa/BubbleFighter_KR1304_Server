-- Tenth local migration; no account, inventory or balance data is rewritten.
CREATE TABLE SocialFriendships (
    FirstCharacterId BIGINT UNSIGNED NOT NULL,
    SecondCharacterId BIGINT UNSIGNED NOT NULL,
    RequestedBy BIGINT UNSIGNED NOT NULL,
    IsAccepted TINYINT UNSIGNED NOT NULL DEFAULT 0,
    CreatedAt TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (FirstCharacterId, SecondCharacterId),
    KEY IX_SocialFriendships_Second (SecondCharacterId),
    CONSTRAINT FK_SocialFriendships_First FOREIGN KEY (FirstCharacterId) REFERENCES Characters(CharacterId) ON DELETE CASCADE,
    CONSTRAINT FK_SocialFriendships_Second FOREIGN KEY (SecondCharacterId) REFERENCES Characters(CharacterId) ON DELETE CASCADE,
    CONSTRAINT CK_SocialFriendships_Order CHECK (FirstCharacterId < SecondCharacterId),
    CONSTRAINT CK_SocialFriendships_Requester CHECK (RequestedBy = FirstCharacterId OR RequestedBy = SecondCharacterId),
    CONSTRAINT CK_SocialFriendships_State CHECK (IsAccepted IN (0, 1))
);
CREATE TABLE SocialChatBlocks (
    OwnerCharacterId BIGINT UNSIGNED NOT NULL,
    TargetCharacterId BIGINT UNSIGNED NOT NULL,
    CreatedAt TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    PRIMARY KEY (OwnerCharacterId, TargetCharacterId),
    KEY IX_SocialChatBlocks_Target (TargetCharacterId),
    CONSTRAINT FK_SocialChatBlocks_Owner FOREIGN KEY (OwnerCharacterId) REFERENCES Characters(CharacterId) ON DELETE CASCADE,
    CONSTRAINT FK_SocialChatBlocks_Target FOREIGN KEY (TargetCharacterId) REFERENCES Characters(CharacterId) ON DELETE CASCADE,
    CONSTRAINT CK_SocialChatBlocks_Different CHECK (OwnerCharacterId <> TargetCharacterId)
);
