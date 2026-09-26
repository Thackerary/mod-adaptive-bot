-- =============================================================================
-- AdaptiveBot Phase 4：公会前台接待员 NPC 模板 + 使魔终端脚本挂载
-- =============================================================================

-- 公会前台接待员（避开 70100，使用安全 Entry: 70111 暴风城 / 70112 奥格瑞玛）
DELETE FROM `creature_template` WHERE `entry` IN (70111, 70112);
DELETE FROM `creature_template_model` WHERE `CreatureID` IN (70111, 70112);
DELETE FROM `creature` WHERE `guid` IN (4000000, 4000001);

INSERT INTO `creature_template`
(`entry`, `name`, `subname`, `IconName`, `gossip_menu_id`, `minlevel`, `maxlevel`, `faction`, `npcflag`,
 `speed_walk`, `speed_run`, `rank`, `unit_class`, `unit_flags`, `type`, `type_flags`, `RegenHealth`, `flags_extra`, `ScriptName`)
VALUES
(70111, '公会联络官 艾蕾娜', '全联盟冒险者公会代表', 'Speak', 0, 80, 80, 35, 1, 1.0, 1.14286, 0, 1, 0, 7, 0, 1, 0, 'npc_bot_guild_receptionist'),
(70112, '公会先锋官 高尔克', '全部落冒险者公会代表', 'Speak', 0, 80, 80, 190, 1, 1.0, 1.14286, 0, 1, 0, 7, 0, 1, 0, 'npc_bot_guild_receptionist');

-- 写入模型子表
INSERT INTO `creature_template_model` (`CreatureID`, `Idx`, `CreatureDisplayID`, `DisplayScale`, `Probability`, `VerifiedBuild`) VALUES
(70111, 0, 24979, 1.0, 1.0, 0),
(70112, 0, 24980, 1.0, 1.0, 0);

-- 世界刷新点（使用真实字段 id，写入安全空白区 GUID: 4000000 / 4000001）
INSERT INTO `creature`
(`guid`, `id`, `map`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecs`, `wander_distance`, `curhealth`, `curmana`, `MovementType`)
VALUES
(4000000, 70111, 0, -8854.0, 622.0,   94.0, 0.6, 300, 0, 50000, 0, 0),  -- 暴风城 · 贸易区
(4000001, 70112, 1, 1596.0,  -4400.0, 17.5, 3.2, 300, 0, 50000, 0, 0); -- 奥格瑞玛 · 力量谷
