-- Seventeenth migration. Depends on create_social_relationships.sql.
-- Durable companion chat. Acknowledgement means adapter handoff, not human read.
CREATE TABLE SocialMessages (
    MessageId BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    SenderCharacterId BIGINT UNSIGNED NOT NULL,
    RecipientCharacterId BIGINT UNSIGNED NOT NULL,
    FirstCharacterId BIGINT UNSIGNED NOT NULL,
    SecondCharacterId BIGINT UNSIGNED NOT NULL,
    SentAtUnixSeconds BIGINT UNSIGNED NOT NULL,
    TextUtf16 VARBINARY(512) NOT NULL,
    PRIMARY KEY (MessageId),
    KEY IX_SocialMessages_Recipient (RecipientCharacterId, MessageId),
    KEY IX_SocialMessages_Pair (FirstCharacterId, SecondCharacterId),
    CONSTRAINT FK_SocialMessages_Friends FOREIGN KEY (FirstCharacterId, SecondCharacterId)
        REFERENCES SocialFriendships(FirstCharacterId, SecondCharacterId) ON DELETE CASCADE,
    CONSTRAINT CK_SocialMessages_Pair CHECK (
        (SenderCharacterId = FirstCharacterId AND RecipientCharacterId = SecondCharacterId)
        OR (SenderCharacterId = SecondCharacterId AND RecipientCharacterId = FirstCharacterId)),
    CONSTRAINT CK_SocialMessages_Time CHECK (SentAtUnixSeconds BETWEEN 1 AND 4102444800),
    CONSTRAINT CK_SocialMessages_Text CHECK (OCTET_LENGTH(TextUtf16) BETWEEN 2 AND 512 AND MOD(OCTET_LENGTH(TextUtf16), 2) = 0)
) ENGINE=InnoDB;
