-- Migration21: immutable multi-component gifts; existing ordinary terms unchanged.
-- Gifts has one group unit (UsageKind1/Amount1/MaxAmount1) for a StockSet identity.
-- No CashPurchases/CashBundles receipt is created by a gift or its claim.
CREATE TABLE GiftBundles (
    GiftId INT UNSIGNED NOT NULL PRIMARY KEY,
    BonusLucci INT UNSIGNED NOT NULL,
    ComponentCount TINYINT UNSIGNED NOT NULL,
    CONSTRAINT GiftBundleOwner FOREIGN KEY (GiftId) REFERENCES Gifts (GiftId),
    CONSTRAINT GiftBundleTerms CHECK (BonusLucci <= 2147483647 AND ComponentCount <= 10 AND (ComponentCount > 0 OR BonusLucci > 0))
) ENGINE=InnoDB;
CREATE TABLE GiftBundleComponents (
    GiftId INT UNSIGNED NOT NULL,
    ComponentIndex TINYINT UNSIGNED NOT NULL,
    StockId INT UNSIGNED NOT NULL,
    ItemId INT UNSIGNED NOT NULL,
    UsageKind TINYINT UNSIGNED NOT NULL,
    Amount INT UNSIGNED NOT NULL,
    MaxAmount INT UNSIGNED NOT NULL,
    PRIMARY KEY (GiftId, ComponentIndex),
    UNIQUE KEY GiftBundleStock (GiftId, StockId),
    CONSTRAINT GiftBundleComponentOwner FOREIGN KEY (GiftId) REFERENCES GiftBundles (GiftId),
    CONSTRAINT GiftBundleComponentTerms CHECK (ComponentIndex < 10 AND StockId BETWEEN 1000000 AND 1999999 AND ItemId > 0 AND UsageKind IN (1,2) AND Amount > 0 AND MaxAmount > 0)
) ENGINE=InnoDB;
