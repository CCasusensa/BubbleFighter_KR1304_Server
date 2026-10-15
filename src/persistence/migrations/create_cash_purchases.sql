-- Migration18: purchase ownership for atomic whole-purchase cash refunds.
-- Existing slots stay claimable and have no inferred refund ownership.
CREATE TABLE CashPurchases (
    PurchaseId INT UNSIGNED NOT NULL PRIMARY KEY,
    CharacterId BIGINT UNSIGNED NOT NULL,
    StockId INT UNSIGNED NOT NULL,
    ItemId INT UNSIGNED NOT NULL,
    UsageKind TINYINT UNSIGNED NOT NULL,
    Amount INT UNSIGNED NOT NULL,
    MaxAmount INT UNSIGNED NOT NULL,
    CashPrice INT UNSIGNED NOT NULL,
    PurchasedAtUnixSeconds BIGINT UNSIGNED NOT NULL,
    SlotCount INT UNSIGNED NOT NULL,
    PurchaseState TINYINT UNSIGNED NOT NULL DEFAULT 0,
    RefundedAtUnixSeconds BIGINT UNSIGNED NULL,
    KEY CashPurchaseOwner (CharacterId, PurchaseId),
    CONSTRAINT CashPurchaseCharacter FOREIGN KEY (CharacterId) REFERENCES Characters (CharacterId),
    CONSTRAINT CashPurchaseIdRange CHECK (PurchaseId BETWEEN 1 AND 99999999),
    CONSTRAINT CashPurchaseFields CHECK (StockId > 0 AND ItemId > 0 AND UsageKind IN (1,2,3) AND Amount > 0 AND MaxAmount > 0 AND CashPrice <= 2147483647 AND SlotCount BETWEEN 1 AND 1024),
    CONSTRAINT CashPurchaseTime CHECK (PurchasedAtUnixSeconds <= 253402300799),
    CONSTRAINT CashPurchaseState CHECK (PurchaseState IN (0,1,2) AND
        ((PurchaseState <> 2 AND RefundedAtUnixSeconds IS NULL) OR
         (PurchaseState = 2 AND RefundedAtUnixSeconds IS NOT NULL AND
          RefundedAtUnixSeconds BETWEEN PurchasedAtUnixSeconds AND 253402300799)))
) ENGINE=InnoDB;
CREATE TABLE CashPurchaseSlots (
    SlotId INT UNSIGNED NOT NULL PRIMARY KEY,
    PurchaseId INT UNSIGNED NOT NULL,
    KEY CashPurchaseParts (PurchaseId, SlotId),
    CONSTRAINT CashPurchaseSlot FOREIGN KEY (SlotId) REFERENCES CashInventorySlots (SlotId) ON DELETE CASCADE,
    CONSTRAINT CashPurchaseReceipt FOREIGN KEY (PurchaseId) REFERENCES CashPurchases (PurchaseId)
) ENGINE=InnoDB;
