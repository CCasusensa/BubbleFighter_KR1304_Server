-- Migration20: immutable whole-bundle receipts. No historical rows inferred.
-- CashInventorySequence allocates IDs for ordinary slots and bundle receipts.
CREATE TABLE CashBundles (
    SlotId INT UNSIGNED NOT NULL PRIMARY KEY,
    CharacterId BIGINT UNSIGNED NOT NULL,
    StockSetId INT UNSIGNED NOT NULL,
    CashPrice INT UNSIGNED NOT NULL,
    BonusLucci INT UNSIGNED NOT NULL,
    PurchasedAtUnixSeconds BIGINT UNSIGNED NOT NULL,
    Refundable BOOLEAN NOT NULL,
    ComponentCount TINYINT UNSIGNED NOT NULL,
    BundleState TINYINT UNSIGNED NOT NULL DEFAULT 0,
    HandledAtUnixSeconds BIGINT UNSIGNED NULL,
    KEY CashBundleOwner (CharacterId, BundleState, SlotId),
    CONSTRAINT CashBundleCharacter FOREIGN KEY (CharacterId) REFERENCES Characters (CharacterId),
    CONSTRAINT CashBundleIdentity CHECK (SlotId BETWEEN 1 AND 99999999 AND StockSetId BETWEEN 2000000 AND 2999999),
    CONSTRAINT CashBundleFields CHECK (CashPrice <= 2147483647 AND BonusLucci <= 2147483647 AND Refundable IN (0,1) AND ComponentCount <= 10 AND (ComponentCount > 0 OR BonusLucci > 0)),
    CONSTRAINT CashBundleClock CHECK (PurchasedAtUnixSeconds BETWEEN 1 AND 253402300799),
    CONSTRAINT CashBundleState CHECK (BundleState IN (0,1,2) AND
        ((BundleState = 0 AND HandledAtUnixSeconds IS NULL) OR
         (BundleState <> 0 AND HandledAtUnixSeconds IS NOT NULL AND
          HandledAtUnixSeconds BETWEEN PurchasedAtUnixSeconds AND 253402300799)))
) ENGINE=InnoDB;
CREATE TABLE CashBundleComponents (
    SlotId INT UNSIGNED NOT NULL,
    ComponentIndex TINYINT UNSIGNED NOT NULL,
    StockId INT UNSIGNED NOT NULL,
    ItemId INT UNSIGNED NOT NULL,
    UsageKind TINYINT UNSIGNED NOT NULL,
    Amount INT UNSIGNED NOT NULL,
    MaxAmount INT UNSIGNED NOT NULL,
    PRIMARY KEY (SlotId, ComponentIndex),
    UNIQUE KEY CashBundleStock (SlotId, StockId),
    CONSTRAINT CashBundleReceipt FOREIGN KEY (SlotId) REFERENCES CashBundles (SlotId),
    CONSTRAINT CashBundleComponentFields CHECK (ComponentIndex < 10 AND StockId BETWEEN 1000000 AND 1999999 AND ItemId BETWEEN 1 AND 99999999 AND UsageKind IN (1,2,3) AND Amount > 0 AND MaxAmount > 0)
) ENGINE=InnoDB;
