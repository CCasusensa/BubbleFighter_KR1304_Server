-- Migration19: durable gifts. Hiding one's record never removes the peer's gift.
-- Policies: 30-day claim period, 200 visible records per owner/direction,
-- 128 UTF16 message units, 24 thanks units, signed-positive native identities.
CREATE TABLE Gifts (
    GiftId INT UNSIGNED NOT NULL PRIMARY KEY,
    SenderCharacterId BIGINT UNSIGNED NOT NULL,
    RecipientCharacterId BIGINT UNSIGNED NOT NULL,
    SenderNameUtf16 VARBINARY(32) NOT NULL,
    RecipientNameUtf16 VARBINARY(32) NOT NULL,
    StockId INT UNSIGNED NOT NULL,
    ItemId INT UNSIGNED NOT NULL,
    UsageKind TINYINT UNSIGNED NOT NULL,
    Amount INT UNSIGNED NOT NULL,
    MaxAmount INT UNSIGNED NOT NULL,
    Price INT UNSIGNED NOT NULL,
    IsCash TINYINT UNSIGNED NOT NULL,
    SentAtUnixSeconds BIGINT UNSIGNED NOT NULL,
    HandledAtUnixSeconds BIGINT UNSIGNED NOT NULL DEFAULT 0,
    GiftState TINYINT UNSIGNED NOT NULL DEFAULT 0,
    SenderHidden TINYINT UNSIGNED NOT NULL DEFAULT 0,
    RecipientHidden TINYINT UNSIGNED NOT NULL DEFAULT 0,
    MessageUtf16 VARBINARY(256) NOT NULL,
    ThanksPayload MEDIUMBLOB NULL,
    KEY GiftSender (SenderCharacterId, SenderHidden, GiftId),
    KEY GiftRecipient (RecipientCharacterId, RecipientHidden, GiftId),
    CONSTRAINT GiftSenderCharacter FOREIGN KEY (SenderCharacterId) REFERENCES Characters (CharacterId),
    CONSTRAINT GiftRecipientCharacter FOREIGN KEY (RecipientCharacterId) REFERENCES Characters (CharacterId),
    CONSTRAINT GiftIdentity CHECK (GiftId BETWEEN 1 AND 2147483647 AND
        SenderCharacterId BETWEEN 1 AND 2147483647 AND RecipientCharacterId BETWEEN 1 AND 2147483647 AND SenderCharacterId <> RecipientCharacterId),
    CONSTRAINT GiftNames CHECK (OCTET_LENGTH(SenderNameUtf16) BETWEEN 2 AND 32 AND MOD(OCTET_LENGTH(SenderNameUtf16), 2) = 0 AND
        OCTET_LENGTH(RecipientNameUtf16) BETWEEN 2 AND 32 AND MOD(OCTET_LENGTH(RecipientNameUtf16), 2) = 0),
    CONSTRAINT GiftTerms CHECK (StockId > 0 AND ItemId > 0 AND UsageKind IN (1,2,3) AND Amount > 0 AND MaxAmount > 0 AND Price <= 2147483647 AND IsCash IN (0,1)),
    CONSTRAINT GiftTime CHECK (SentAtUnixSeconds BETWEEN 1 AND 4099852800 AND HandledAtUnixSeconds <= 4102444800 AND
        ((GiftState = 0 AND HandledAtUnixSeconds = 0) OR (GiftState IN (1,2) AND HandledAtUnixSeconds >= SentAtUnixSeconds))),
    CONSTRAINT GiftHidden CHECK (SenderHidden IN (0,1) AND RecipientHidden IN (0,1)),
    CONSTRAINT GiftText CHECK (MOD(OCTET_LENGTH(MessageUtf16), 2) = 0),
    CONSTRAINT GiftThanks CHECK (ThanksPayload IS NULL OR (GiftState = 1 AND OCTET_LENGTH(ThanksPayload) BETWEEN 48 AND 65536))
) ENGINE=InnoDB;
CREATE TABLE GiftSequence (
    SequenceId TINYINT UNSIGNED NOT NULL PRIMARY KEY,
    NextGiftId INT UNSIGNED NOT NULL,
    CONSTRAINT GiftSequenceKey CHECK (SequenceId = 1),
    CONSTRAINT GiftSequenceRange CHECK (NextGiftId BETWEEN 1 AND 2147483648)
) ENGINE=InnoDB;
INSERT INTO GiftSequence (SequenceId, NextGiftId) VALUES (1, 1);
