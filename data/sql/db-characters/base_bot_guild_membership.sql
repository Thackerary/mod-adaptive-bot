-- AdaptiveBot 玩家所属冒险者公会会籍与每日补给状态
CREATE TABLE IF NOT EXISTS `character_bot_guild_member` (
  `guid` INT UNSIGNED NOT NULL COMMENT '玩家GUID',
  `guild_id` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '公会ID (1-10)',
  `join_time` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '入会时间戳',
  `last_supply_time` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '上次领取每日补给时间戳',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='AdaptiveBot 玩家所属冒险者公会信息';
