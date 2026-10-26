CREATE TABLE IF NOT EXISTS `abyssal_storage_link` (
  `account_id` INT UNSIGNED NOT NULL,
  `owner_account_id` INT UNSIGNED NOT NULL,
  PRIMARY KEY (`account_id`),
  KEY `idx_owner` (`owner_account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
