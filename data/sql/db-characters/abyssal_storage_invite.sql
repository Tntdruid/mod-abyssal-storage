CREATE TABLE IF NOT EXISTS `abyssal_storage_invite` (
  `invitee_account_id` INT UNSIGNED NOT NULL,
  `inviter_account_id` INT UNSIGNED NOT NULL,
  `inviter_name` VARCHAR(12) NOT NULL DEFAULT '',
  `created_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
  PRIMARY KEY (`invitee_account_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
