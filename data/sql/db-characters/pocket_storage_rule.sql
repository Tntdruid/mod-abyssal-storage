-- Store/Keep lists edited from the addon, per account
CREATE TABLE IF NOT EXISTS `pocket_storage_rule` (
  `account_id` INT UNSIGNED NOT NULL,
  `item_entry` INT UNSIGNED NOT NULL,
  `rule` TINYINT UNSIGNED NOT NULL COMMENT '1 = store, 2 = keep',
  PRIMARY KEY (`account_id`, `item_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- Per-account switches; a missing row means the defaults
CREATE TABLE IF NOT EXISTS `pocket_storage_settings` (
  `account_id` INT UNSIGNED NOT NULL,
  `auto_store` TINYINT UNSIGNED NOT NULL DEFAULT 1,
  PRIMARY KEY (`account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
