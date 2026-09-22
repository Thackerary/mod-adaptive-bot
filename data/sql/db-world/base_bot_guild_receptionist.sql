-- =============================================================================
-- AdaptiveBot Phase 4：公会前台接待员 NPC 模板 + 使魔终端脚本挂载
-- =============================================================================

-- 公会前台接待员 NPC 模板（联盟 / 部落各一名，中立友好阵营 35）
INSERT INTO `creature_template`
(`entry`, `modelid1`, `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `faction`, `npcflag`,
 `speed_walk`, `speed_run`, `scale`, `rank`, `unit_class`, `unit_flags`, `type`, `type_flags`, `RegenHealth`, `flags_extra`, `ScriptName`)
VALUES
(70100, 24979, '公会联络官 艾蕾娜', '全联盟冒险者公会代表', 'Speak', 0, 80, 80, 35, 1,
 1, 1.14286, 1, 0, 1, 0, 7, 0, 1, 0, 'npc_bot_guild_receptionist'),
(70101, 24980, '公会先锋官 高尔克', '全部落冒险者公会代表', 'Speak', 0, 80, 80, 35, 1,
 1, 1.14286, 1, 0, 1, 0, 7, 0, 1, 0, 'npc_bot_guild_receptionist');

-- 世界刷新点（显式 guid 写入，兼容 AUTO_INCREMENT 与非 AUTO_INCREMENT 两种表结构）。
-- 提示：若落点不在理想位置，可直接删除本段并用 GM 指令 `.npc add 70100` / `.npc add 70101` 现场补 spawn。
INSERT INTO `creature`
(`guid`, `id1`, `map`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `curhealth`, `curmana`, `MovementType`)
VALUES
(900000, 70100, 0, -8854.0, 622.0, 94.0, 0.6, 300, 50000, 0, 0),  -- 暴风城 · 贸易区
(900001, 70101, 1, 1596.0, -4400.0, 17.5, 3.2, 300, 50000, 0, 0); -- 奥格瑞玛 · 力量谷

-- 为 10 种公会专属使魔实体的 creature_template 挂载随身公会终端脚本与 Gossip 标志
UPDATE `creature_template` SET `npcflag` = `npcflag` | 1, `ScriptName` = 'npc_bot_guild_pet' WHERE `entry` IN (
  2671,  -- 铁炉堡探险者协会: 机械松鼠
  7384,  -- 暴风城军情七处: 孟加拉虎
  7385,  -- 达拉然银色盟约: 白猫
  7390,  -- 战歌远征突击队: 黑色王蛇
  18839, -- 夺日者议会: 红尾金龙鱼
  7387,  -- 幽暗城死亡猎手狂怒社: 幽暗城蟑螂
  33238, -- 银色北伐军先锋营: 银色侍从
  31575, -- 达拉然下水道黑市行会: 下水道巨鼠
  32791, -- 塞纳里奥议会/远征队: 春兔
  7394   -- 热砂财阀雇佣行: 森金树蛙
);
