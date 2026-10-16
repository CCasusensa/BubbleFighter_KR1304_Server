-- Additive operational schema. Apply after all existing migrations.
CREATE TABLE OperationSettings (
    SettingId TINYINT UNSIGNED NOT NULL PRIMARY KEY,
    Document JSON NOT NULL,
    CONSTRAINT CK_OperationSettings_Id CHECK (SettingId = 1)
) ENGINE=InnoDB;
INSERT INTO OperationSettings VALUES (1, JSON_OBJECT(
    'register_open', true, 'register_require_qq', false, 'register_level', 1,
    'register_cash', 0, 'register_lucci', 0, 'register_items', JSON_ARRAY(),
    'launcher_enforce', false, 'launcher_min_version', '',
    'launcher_latest_version', '', 'launcher_message', ''));
CREATE TABLE OperationAccounts (
    AccountId BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    QQ VARCHAR(20) CHARACTER SET ascii COLLATE ascii_bin NULL,
    LastIp VARCHAR(45) CHARACTER SET ascii NOT NULL DEFAULT '',
    LauncherVersion VARCHAR(32) CHARACTER SET ascii NOT NULL DEFAULT '',
    Whitelisted BOOLEAN NOT NULL DEFAULT FALSE,
    Note VARCHAR(255) NOT NULL DEFAULT '',
    AddedBy VARCHAR(64) NOT NULL DEFAULT '',
    UNIQUE KEY UX_OperationAccounts_QQ (QQ),
    FOREIGN KEY (AccountId) REFERENCES Accounts(AccountId) ON DELETE CASCADE
) ENGINE=InnoDB;
CREATE TABLE OperationQQMembers (
    QQ VARCHAR(20) CHARACTER SET ascii COLLATE ascii_bin NOT NULL PRIMARY KEY,
    Nickname VARCHAR(128) NOT NULL DEFAULT '',
    Card VARCHAR(128) NOT NULL DEFAULT '',
    Role VARCHAR(32) NOT NULL DEFAULT 'member'
) ENGINE=InnoDB;
CREATE TABLE OperationBans (
    BanId BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    Kind VARCHAR(16) CHARACTER SET ascii NOT NULL,
    Target VARCHAR(64) CHARACTER SET ascii COLLATE ascii_bin NOT NULL,
    Reason VARCHAR(255) NOT NULL,
    ExpiresAt BIGINT UNSIGNED NOT NULL DEFAULT 0,
    CreatedBy VARCHAR(64) NOT NULL,
    CreatedAt TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    KEY IX_OperationBans_Target (Kind, Target, ExpiresAt),
    CONSTRAINT CK_OperationBans_Kind CHECK (Kind IN ('account','ip'))
) ENGINE=InnoDB;
CREATE TABLE OperationAdmins (
    AccountId BIGINT UNSIGNED NOT NULL PRIMARY KEY,
    Note VARCHAR(255) NOT NULL DEFAULT '',
    FOREIGN KEY (AccountId) REFERENCES Accounts(AccountId) ON DELETE CASCADE
) ENGINE=InnoDB;
CREATE TABLE OperationIpAllowlist (
    Entry VARCHAR(49) CHARACTER SET ascii NOT NULL PRIMARY KEY,
    Note VARCHAR(255) NOT NULL DEFAULT ''
) ENGINE=InnoDB;
CREATE TABLE OperationSessions (
    TokenHash BINARY(32) NOT NULL PRIMARY KEY,
    AccountId BIGINT UNSIGNED NULL,
    Purpose VARCHAR(16) CHARACTER SET ascii NOT NULL,
    ExpiresAt BIGINT UNSIGNED NOT NULL,
    FOREIGN KEY (AccountId) REFERENCES Accounts(AccountId) ON DELETE CASCADE,
    KEY IX_OperationSessions_Expiry (ExpiresAt),
    CONSTRAINT CK_OperationSessions_Purpose CHECK (Purpose IN ('launcher','gm','bootstrap'))
) ENGINE=InnoDB;
CREATE TABLE OperationCloudFiles (
    AccountId BIGINT UNSIGNED NOT NULL,
    FileName VARCHAR(32) CHARACTER SET ascii NOT NULL,
    Content MEDIUMBLOB NOT NULL,
    ModifiedAt BIGINT UNSIGNED NOT NULL,
    PRIMARY KEY (AccountId, FileName),
    FOREIGN KEY (AccountId) REFERENCES Accounts(AccountId) ON DELETE CASCADE,
    CONSTRAINT CK_OperationCloudFiles_Name CHECK (FileName IN ('option.ini','client_display.ini')),
    CONSTRAINT CK_OperationCloudFiles_Size CHECK (OCTET_LENGTH(Content) <= 65536)
) ENGINE=InnoDB;
CREATE TABLE OperationAudit (
    AuditId BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    Actor VARCHAR(64) NOT NULL,
    Action VARCHAR(128) NOT NULL,
    AccountId BIGINT UNSIGNED NULL,
    Request JSON NOT NULL,
    BeforeValue JSON NULL,
    AfterValue JSON NULL,
    Ok BOOLEAN NOT NULL,
    CreatedAt TIMESTAMP(6) NOT NULL DEFAULT CURRENT_TIMESTAMP(6),
    KEY IX_OperationAudit_Account (AccountId, AuditId)
) ENGINE=InnoDB;
CREATE TABLE OperationAnnouncements (
    AnnouncementId BIGINT UNSIGNED NOT NULL AUTO_INCREMENT PRIMARY KEY,
    Message TEXT NOT NULL,
    Actor VARCHAR(64) NOT NULL,
    Delivered INT UNSIGNED NOT NULL,
    CreatedAt TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP
) ENGINE=InnoDB;
CREATE TABLE OperationShopEntries (
    ItemId INT UNSIGNED NOT NULL PRIMARY KEY,
    Document JSON NOT NULL
) ENGINE=InnoDB;
