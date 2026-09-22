-- AdaptiveBot 佣兵信托与信用黑名单持久化
CREATE TABLE IF NOT EXISTS `character_bot_escrow` (
  `guid` INT UNSIGNED NOT NULL COMMENT '玩家GUID',
  `is_bankrupt` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '是否处于信用破产状态 (1=是)',
  `debt_copper` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '逾期拖欠佣金(铜)',
  `updated_time` BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '最后变更时间戳',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COMMENT='AdaptiveBot 佣兵信托与信用黑名单';
